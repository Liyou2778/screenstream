# ScreenStream — Windows 桌面实时串流到浏览器（C++20 MVP）

从零实现的「Windows 桌面 → JPEG → 自定义二进制帧协议 → 自写 HTTP/WebSocket → 浏览器 canvas 渲染」最小可运行串流系统。
单进程、纯 C++20 + Windows 系统库（D3D11 / DXGI / GDI / WIC / Winsock2），**不引入任何第三方依赖**，不依赖 vcpkg，不依赖 PATH 里的 `cl`/`cmake`。

实测结论（本机 Windows 11，1920×1080 主屏）：**DXGI Desktop Duplication 正常工作**，1280×720 JPEG 平均 87.4 KB/帧，
15 fps 连续输出、~11.2 Mbps、WS 服务端→客户端延迟 5.8 ms；浏览器播放页 HUD 实测 15.0 fps / 10.7 Mbps / 7.7 ms。
GDI BitBlt 回退路径也单独跑通（`--capture gdi`：15.1 fps / 11.6 Mbps）。

---

## 1. 目录结构

```
replicas/03-screenstream/
├─ build.ps1                     # 一键构建（vcvars64 + cmake + ninja，全部绝对路径）
├─ CMakeLists.txt
├─ build/                        # cmake/ninja 产物，screenstream.exe（153600 bytes）
├─ src/
│  ├─ main.cpp                   # 入口、参数解析、采集/编码线程、--grab 单帧取证模式
│  ├─ capture.h/.cpp             # DXGI Desktop Duplication + GDI BitBlt 自动回退
│  ├─ jpeg_encoder.h/.cpp        # WIC 缩放 + JPEG 编码（IWICBitmapScaler/IWICFormatConverter）
│  ├─ protocol.h/.cpp            # SSF1 分片协议、WS 帧构造、SHA1+Base64（握手）
│  ├─ frame_hub.h/.cpp           # 最新帧邮箱（慢客户端直接丢帧，不排队）
│  ├─ server.h/.cpp              # HTTP/1.1 + WebSocket 服务、线程池、/api/stats、键鼠注入
│  ├─ thread_pool.h/.cpp         # 固定 8 线程工作池
│  ├─ page.h/.cpp                # 内嵌单页播放器（HTML/CSS/JS 全在一处）
│  └─ common.h/.cpp              # 时间戳、日志、HRESULT 文本、黑帧检测
├─ tools/
│  ├─ ws_probe.mjs               # Node 24 内置 WebSocket 客户端：按 SSF1 收 30 帧并统计
│  └─ browser_shot.mjs           # 通过 Chrome DevTools Protocol 截图并读取 HUD 数值
├─ evidence/                     # 自验证据（截图/日志/JSON/JPEG）
└─ README.md
```

规模：C++ 17 个文件 2449 行（94 KB），Node 探针 2 个文件 226 行，`build.ps1` 18 行，`CMakeLists.txt` 37 行。

---

## 2. 构建

依赖（全部已装好）：Visual Studio 2022 Community + Windows SDK 10.0.22621.0。无需 xmake/vcpkg/子模块。

```powershell
cd C:\Users\Liyou\Documents\deepseek\replicas\03-screenstream
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1
```

> 当前用户执行策略是 `RemoteSigned`，未签名的 `build.ps1` 需要用 `-ExecutionPolicy Bypass` 调用
> （或者先 `Unblock-File .\build.ps1`）。

`build.ps1` 实际执行的命令（工具全部写死绝对路径，不依赖 PATH）：

```
chcp 65001 >nul && call "C:\develop\vs2022\community\VC\Auxiliary\Build\vcvars64.bat" &&
"C:\develop\vs2022\community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
  -G Ninja -B build -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_MAKE_PROGRAM="C:\develop\vs2022\community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe" &&
"C:\...\cmake.exe" --build build
```

`chcp 65001` 是必要的：中文版 Windows 的 MSVC `/showIncludes` 前缀是本地化文本，若控制台代码页与
CMake 记录的 UTF-8 前缀不一致，Ninja 会把每个头文件都当依赖行回显出来（构建仍能成功但日志被刷屏）。

构建输出尾部（完整日志见 `evidence/build.log`，**0 error / 0 warning**）：

```
-- The CXX compiler identification is MSVC 19.43.34809.0
...
[1/10] Building CXX object CMakeFiles\screenstream.dir\src\page.cpp.obj
[2/10] Building CXX object CMakeFiles\screenstream.dir\src\frame_hub.cpp.obj
[3/10] Building CXX object CMakeFiles\screenstream.dir\src\common.cpp.obj
[4/10] Building CXX object CMakeFiles\screenstream.dir\src\protocol.cpp.obj
[5/10] Building CXX object CMakeFiles\screenstream.dir\src\thread_pool.cpp.obj
[6/10] Building CXX object CMakeFiles\screenstream.dir\src\capture.cpp.obj
[7/10] Building CXX object CMakeFiles\screenstream.dir\src\jpeg_encoder.cpp.obj
[8/10] Building CXX object CMakeFiles\screenstream.dir\src\server.cpp.obj
[9/10] Building CXX object CMakeFiles\screenstream.dir\src\main.cpp.obj
[10/10] Linking CXX executable screenstream.exe
== build OK ==
```

链接的系统库：`d3d11 dxgi ws2_32 windowscodecs ole32 oleaut32 user32 gdi32`。

---

## 3. 运行

```powershell
cd C:\Users\Liyou\Documents\deepseek\replicas\03-screenstream

# 默认启动：0.0.0.0:8203，15 fps，1280x720，JPEG 质量 0.72
.\build\screenstream.exe

# 浏览器打开（同机）
start http://127.0.0.1:8203/
# 局域网其它设备用  http://<本机IP>:8203/
```

常用变体：

```powershell
.\build\screenstream.exe --port 8203 --fps 20 --width 1600 --height 900 --quality 0.65
.\build\screenstream.exe --bind 127.0.0.1            # 只监听回环，最安全
.\build\screenstream.exe --duration 10               # 跑 10 秒自动退出（便于脚本化验证）
.\build\screenstream.exe --grab evidence\frame.jpg --grab-wait 3000   # 抓一帧落盘后退出
.\build\screenstream.exe --capture gdi --grab evidence\frame_gdi.jpg  # 强制 GDI 后端取证
```

`Ctrl+C` 停止（已注册 `SetConsoleCtrlHandler`），进程退出前会打印最终 stats JSON。

| 参数 | 默认 | 说明 |
|---|---|---|
| `--port <n>` | 8203 | HTTP/WebSocket 端口 |
| `--bind <ip>` | 0.0.0.0 | 监听地址，`127.0.0.1` 仅本机 |
| `--fps <n>` | 15 | 目标帧率 |
| `--width/--height <n>` | 1280/720 | 编码分辨率（源分辨率任意，WIC 缩放） |
| `--quality <f>` | 0.72 | JPEG 质量 0.05–1.0 |
| `--capture <auto\|dxgi\|gdi>` | auto | 强制采集后端，用于复现回退路径 |
| `--grab <path>` | - | 只抓一帧、编码落盘后退出（取证模式） |
| `--grab-wait <ms>` | 0 | 取证模式下最多等 N ms 直到拿到非全黑帧 |
| `--duration <s>` | 0 | N 秒后自动退出，0 = 一直跑 |

服务器启动日志（`evidence/server.log`）：

```
[21:30:42.945] capture backend: dxgi
[21:30:42.947] HTTP/WebSocket server listening on 0.0.0.0:8203 (threads=8)
[21:30:42.949] playback page : http://127.0.0.1:8203/
[21:30:42.949] stats API     : http://127.0.0.1:8203/api/stats
[21:30:42.949] websocket     : ws://127.0.0.1:8203/ws
[21:30:42.966] first frame: JPEG 89500 bytes, 1280x720, capture=dxgi, black=no
[21:30:45.165] websocket client connected, clients=1
```

---

## 4. 实测指标（真实跑出来的数字）

测试机：Windows 11，主屏 1920×1080，本机回环（`127.0.0.1`），Node 24.19 / Chrome headless。

### 4.1 HTTP 播放页

`evidence/http_check.txt`：

```
HTTP 200 length=11470 content_type=text/html; charset=utf-8 contains_canvas=True contains_ws_js=True
```

### 4.2 WebSocket 收 30 帧（`node tools/ws_probe.mjs ws://127.0.0.1:8203/ws 30`，`evidence/ws_probe.txt`）

| 指标 | 实测值 |
|---|---|
| 收到帧数 | 30 / 30（无丢帧） |
| 编码分辨率 / 分片 | 1280×720 / 每帧 3 个分片（32 KB 上限） |
| 平均帧大小 | 89 502 B = **87.4 KB** |
| 30 帧总字节 | 2 685 052 B |
| 平均帧间隔 | **66.34 ms**（目标 66.67 ms，jitter 6.81 ms） |
| 实测帧率 | **15.07 fps** |
| 字节率 | **11.17 Mbps** |
| 服务端 ts_us → 客户端收到 | 平均 **5.83 ms** |
| JPEG 完整性（SOI/EOI） | 30/30 有效 |

### 4.3 浏览器端渲染（Chrome headless + CDP，`evidence/browser.png` / `evidence/browser_hud.txt`）

播放页 HUD 真实读数（DOM 文本）：

```
FPS 15.0 | 带宽 10.74 Mbps | 端到端延迟 7.7 ms | 帧大小 87.4 KB |
分辨率 1280x720 | 采集 DXGI | 服务端编码 15.2 fps | 已显示 106 帧 | 连接 connected
```

截图 `evidence/browser.png` 里可以直接看到 canvas 渲染出的真实桌面画面 + HUD。
（延迟是浏览器用 `performance.timeOrigin + performance.now()` 与帧内 Unix 微秒时间戳相比得到的，
同一台机器共享时钟，因此是真实的端到端管线延迟，不含显示器刷新。）

### 4.4 单帧落盘取证（采集链路真的工作）

`evidence/frame.jpg`（DXGI 后端）：

```
[21:30:42.388] capture backend: dxgi
[21:30:42.403] captured 1920x1080 frame via dxgi (1 frame(s) sampled, 0 black), all-black=no
[21:30:42.419] wrote ...\evidence\frame.jpg (89508 bytes JPEG, 1280x720)
```

- 采集源 1920×1080 → 缩放 1280×720 → JPEG **89 508 B（> 10 KB，远超要求）**，非全黑。
- `evidence/frame_gdi.jpg` 为同一时刻 `--capture gdi` 的落盘结果（**89 473 B**），两条采集路径都有产物。

### 4.5 GDI 回退路径独立实测

用专门端口（避免与本机其它服务冲突）启动 `--capture gdi`：

```
.\build\screenstream.exe --port 18204 --capture gdi
# /api/stats: {"capture_mode":"gdi", ...}
```

`evidence/ws_probe_gdi.txt`：15 帧、87.4 KB/帧、间隔 66.23 ms、**15.1 fps**、11.58 Mbps、延迟 9.48 ms、JPEG 全部有效。
即 GDI BitBlt 后端端到端可用（`evidence/gdi_server.log` 记录了 `capture backend: gdi`）。

### 4.6 DXGI 黑帧与自动回退（如实记录）

- 第一次直接 `--grab`（未加 `--grab-wait`）时，DXGI 在 Duplication 刚建立后返回过一帧**全黑空帧**
  （JPEG 15027 B、`all-black=yes`）。这是 Desktop Duplication 启动瞬间的已知现象。
- 之后所有测试（20+ 次 `--grab`、多次长时间服务端运行、浏览器渲染）**DXGI 均为真实画面**，
  所以本机最终走的是 **DXGI**，不是 GDI 回退。
- 代码里内置了两层保护并会把实际路径打印出来：
  1. `DesktopCapture::Grab()` 中 DXGI 出错（ACCESS_LOST / 设备丢失等）→ 立即重建/切换 GDI；
  2. 服务端采集线程发现「首帧全黑且 1.5 s 内没有出现非黑帧」→ 自动切 GDI；
  3. DXGI 3 s 内一帧都不给（静态桌面）→ 自动切 GDI。
  切换会打印 `switching capture backend to GDI BitBlt`，`/api/stats` 的 `capture_mode` 同步变成 `gdi`。

---

## 5. 架构与数据流

```
┌──────────────────────────── 单进程 screenstream.exe ────────────────────────────┐
│                                                                                 │
│  main thread            capture/producer thread            worker pool (8)      │
│  ┌─────────────┐        ┌───────────────────────┐         ┌───────────────────┐ │
│  │ 参数解析     │        │ DXGI Desktop Dup.     │         │ HTTP 请求处理      │ │
│  │ 控制台信号   │        │   └─(失败/黑帧)─→ GDI │         │ WS 客户端事件循环   │ │
│  │ accept 循环  │        │        BitBlt          │         │  - select(5ms)    │ │
│  └──────┬──────┘        │ BGRA 1920×1080         │         │  - 收文本/控制帧    │ │
│         │ submit        │   ↓ WIC 缩放 1280×720  │         │  - 取最新帧→分片→发 │ │
│         ↓               │   ↓ WIC JPEG (~87KB)   │         │  - /api/stats JSON │ │
│  ┌──────────────┐       │   ↓ 发布时间戳 ts_us   │         └─────────┬─────────┘ │
│  │ listen:8203  │       └──────────┬────────────┘                   │           │
│  └──────────────┘                  │ publish                        │ pull      │
│                          ┌─────────▼────────────┐                   │           │
│                          │ FrameHub 最新帧邮箱   │◄──────────────────┘           │
│                          │ (mutex + 版本号, 单槽) │  慢客户端自然丢帧，不堆积     │
│                          └──────────────────────┘                              │
└─────────────────────────────────────────────────────────────────────────────────┘
                                     │ WebSocket binary（TCP 127.0.0.1:8203）
                                     ▼
┌──────────────────────────── 浏览器播放页（内嵌 HTML） ──────────────────────────┐
│ WS binaryType=arraybuffer → 按 seq 重组 32KB 分片 → Blob → createImageBitmap    │
│ → canvas.drawImage → HUD（FPS/带宽 Mbps/端到端延迟/帧大小/采集后端/连接状态）      │
│ 可选：勾选「远程输入注入」后鼠标移动/左键点击 → JSON 文本帧 → 服务端 SendInput      │
└─────────────────────────────────────────────────────────────────────────────────┘
```

关键设计点：

1. **采集线程独立**：DXGI/GDI、缩放、JPEG 编码都在一个生产者线程里完成，绝不阻塞网络线程。
2. **最新帧邮箱（FrameHub）**：只保留最新一帧 + 单调递增版本号；客户端按版本号取新帧，
   慢客户端自动跳帧（实时串流的正确背压策略，不排队、不累积延迟）。
3. **静态桌面也有流**：DXGI 只在画面变化时出帧；若本轮无新帧就重发上一张 JPEG 并打新 `seq/ts_us`，
   保证浏览器 FPS/延迟统计连续（重发不重新编码，CPU 开销为零）。
4. **线程池 8 线程**：每个 WS 连接独占一个 worker 跑 `select` 事件循环（5 ms 轮询），
   HTTP 一次性请求也走同一个池；连接间互不影响。
5. **WIC 编码**：`CreateBitmapFromMemory(32bppBGRA)` → `IWICBitmapScaler`（线性插值缩到 1280×720）
   → `IWICFormatConverter`（BGRA→24bppBGR）→ JPEG encoder（质量 0.72）。
6. **指针/Pi 信息不做特殊处理**：桌面复制纹理始终包含完整桌面，移动/脏矩形只用于优化，本实现每帧全量取。

---

## 6. 自定义帧协议 SSF1（1 字节 fmt 区分编码，当前只有 JPEG）

每个 WebSocket **binary 消息 = 一个分片**，分片头固定 33 字节，**每个分片都重复携带完整帧元数据**
（这样即使分片交错/乱序，接收端也能独立重组；慢速链路丢片只影响该帧）。

| 偏移 | 大小 | 字段 | 类型 | 说明 |
|---:|---:|---|---|---|
| 0 | 4 | `magic` | 4×u8 | 固定 ASCII `SSF1`（小端读作 `0x31465353`） |
| 4 | 4 | `seq` | u32 LE | 帧序号，单调递增，接收端按它重组 |
| 8 | 8 | `ts_us` | u64 LE | Unix 纪元微秒（服务端时钟），HUD 用它算端到端延迟 |
| 16 | 2 | `width` | u16 LE | 编码帧宽（像素） |
| 18 | 2 | `height` | u16 LE | 编码帧高（像素） |
| 20 | 1 | `fmt` | u8 | 1 = JPEG |
| 21 | 4 | `total_len` | u32 LE | 整帧 payload 总字节数（不含任何头） |
| 25 | 2 | `chunk_index` | u16 LE | 分片下标 0..chunk_count-1 |
| 27 | 2 | `chunk_count` | u16 LE | 分片总数 = ceil(total_len / 32768) |
| 29 | 4 | `chunk_len` | u32 LE | 本分片 payload 字节数 |
| 33 | N | `payload` | bytes | JPEG 字节切片（1 ≤ N ≤ 32768） |

示例：87.4 KB 的 JPEG → `chunk_count = 3`（32768 + 32768 + 23966），客户端收到 3 条 binary WS 消息后
按 `chunk_index` 拼接，校验拼出的 JPEG 以 `FF D8` 开头、`FF D9` 结尾。

WebSocket 层：

- 服务端 → 客户端：`opcode=0x2`（binary），不加掩码；单条消息 ≤ 33+32768 B。
- 客户端 → 服务端：`opcode=0x1`（text）JSON：
  - `{"type":"input","enable":true|false}` 开启/关闭输入注入（页面复选框，默认关闭）
  - `{"type":"mouse","x":0-1279,"y":0-719,"click":0|1}` 鼠标移动/左键点击
- 控制帧：PING→PONG 自动回复，CLOSE 正常握手关闭（服务端也实现了客户端分片消息的重组）。

HTTP/1.1 接口：

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/`、`/index.html` | 内嵌播放页（`text/html; charset=utf-8`，11470 B） |
| GET | `/api/stats` | JSON 统计（见下） |
| GET | `/ws` | WebSocket Upgrade（返回 101 + `Sec-WebSocket-Accept`，SHA1 自实现） |
| 其它 | * | 404 |

`/api/stats` 字段：`capture_mode`（`dxgi`/`gdi`）、`width/height`、`target_fps`、`producer_fps`、
`frames_produced`、`bytes_produced`、`frames_sent`、`bytes_sent`、`avg_frame_kb`、`bitrate_mbps`、
`clients`、`last_frame_bytes`、`uptime_seconds`、`input_events`。

---

## 7. 与 CrossDesk 的差异

CrossDesk（kunkundi/crossdesk，LGPL-3.0）是跨平台远程桌面软件，核心是 MiniRTC 实时传输库
（ICE/RFC5245 网络透传、H.264/AV1 软硬编解码、Opus 音频、SRTP 加密、信令与拥塞控制），
PC 端用 xmake 构建，另有 Web 客户端。本仓库**只参考了它的分层思路**（采集 → 编码 → 传输 → Web 播放），
代码完全自写、未复制。

| 维度 | CrossDesk（参考） | 本实现（ScreenStream MVP） |
|---|---|---|
| 传输 | MiniRTC / WebRTC（ICE，可 NAT 穿透） | 自写 HTTP/1.1 + WebSocket over TCP，仅同机/局域网 |
| 视频编码 | H.264 / AV1，软编 + CUDA/硬编 | 每帧独立 JPEG（WIC），无帧间预测 |
| 音频 | Opus（采集+播放） | **无** |
| 加密 | SRTP | 无 TLS（可后续加 WSS） |
| 网络自适应 | 拥塞控制、码率自适应 | 固定分辨率/帧率/质量，慢客户端丢帧 |
| 节点发现/信令 | 有（设备 ID + 密码） | 无（直接 IP:端口） |
| 平台 | Windows / macOS / Linux | Windows 10+ x64 |
| 采集 | 平台各自实现（Desktop Duplication / X11 / CG） | DXGI Desktop Duplication + GDI BitBlt 回退 |
| 输入回传 | 完整键鼠 + 多显示器 | 鼠标移动/左键 + 可选开关（SendInput） |
| 构建 | xmake + 子模块 | 单个 CMakeLists + Ninja + Windows SDK，无子模块 |
| 规模 | 大型多模块工程 | 17 个 C++ 文件 / 2449 行 |

---

## 8. 已知限制

- **无音频**（只有视频帧流）。
- **无 NAT 穿透/中继**：只能同机或同局域网直连；公网需要端口映射或后续接 WebRTC/QUIC。
- **无鉴权、无 TLS**：默认 `0.0.0.0:8203` 会暴露给局域网任何主机；只在本机用请加 `--bind 127.0.0.1`。
- **JPEG 每帧独立编码**：无运动估计，带宽约为 H.264 的 3–10 倍（本机 1280×720@15fps ≈ 11 Mbps）。
- **只有主显示器**：采集选桌面原点在 (0,0) 的 DXGI output；多屏/负坐标屏未做选择与拼接。
- **DXGI 在部分环境会返回黑帧**（本机启动瞬间碰到过一次）；已做自动回退 GDI，但若整个会话都不允许抓屏
  （锁屏、Session 0、受保护内容），两条路径都会是黑屏，此时本程序会打印 `all-black=yes` 警告，不会假装成功。
- **无丢包重传**：TCP 保证可靠，但某帧分片异常时该帧直接丢弃（浏览器最多缓存 24 个半成品帧并及时清理）。
- **无键盘注入**：只有鼠标移动和左键单击，且必须先勾选页面上的「远程输入注入」。
- **CPU 编码**：WIC JPEG 走 CPU，高分辨率/高帧率时占用明显；无 GPU 编码。
- **端口冲突**：HTTP 服务用了 `SO_REUSEADDR`（Windows 下允许重复 bind），同端口若已有别的服务在跑，
  连接可能被内核分给另一个进程。验证时建议用不常用端口（本 README 的 GDI 测试用的就是 18204）。

---

## 9. 阶段 2 TODO（未完成）

1. Media Foundation H.264 硬编（MFT `CODECAPI_AVEncCommonQuality`）+ 浏览器 WebCodecs `VideoDecoder`，
   关键帧 + 增量帧，带宽目标 < 3 Mbps @ 1280×720@30。
2. 码率/帧率自适应：根据 WS 发送队列与实测吞吐动态调 JPEG 质量。
3. 多显示器枚举与选择、缩放/DPI 映射修正。
4. 音频：WASAPI loopback → Opus → 浏览器 `AudioContext`。
5. WSS/TLS + token 鉴权；`--bind` 默认改为 `127.0.0.1`。
6. 完整键鼠：键盘扫描码、滚轮、右键、拖拽。
7. WebSocket permessage-deflate 或按帧差分压缩。

---

## 10. 排错

| 现象 | 处理 |
|---|---|
| 浏览器一直「等待视频帧」 | 打开 `http://127.0.0.1:8203/api/stats` 看 `frames_produced` 是否增长；F12 Console 看 WS 状态 |
| 画面全黑 | `.\build\screenstream.exe --grab evidence\check.jpg` 看日志 `all-black=yes/no`；锁屏或远程会话限制无法解决 |
| 启动报 `bind(port 8203) failed` | 端口被占用，换 `--port 18203` |
| 浏览器连不上 | 确认 exe 正在跑、防火墙放行、`--bind` 不是 `127.0.0.1` 时用本机 IP 访问 |
| 构建脚本被拦截 | 用 `powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1` |
| 构建日志被头文件刷屏 | 说明控制台不是 UTF-8 代码页，`build.ps1` 已内置 `chcp 65001` |

收工前请确认没有残留进程：`Get-Process screenstream -EA SilentlyContinue`（本仓库自验后已全部停止，
8203/8204/18204 均无监听）。

---

## 11. 自验证据清单（evidence/）

| 文件 | 说明 |
|---|---|
| `build.log` | 干净重建的完整输出（0 error / 0 warning，`== build OK ==`） |
| `http_check.txt` | `GET /` 的 200 / 长度 / 含 `<canvas` / 含 `/ws` 检查结果 |
| `ws_probe.txt` | 30 帧 WS 探针完整统计（DXGI 后端） |
| `ws_probe_gdi.txt` | 15 帧 WS 探针完整统计（GDI 回退后端，端口 18204） |
| `stats.json` / `stats_gdi.json` | 对应时刻的 `/api/stats` 原始 JSON |
| `server.log` / `gdi_server.log` | 服务端启动、WS 连接、首帧日志 |
| `frame.jpg` | **DXGI 单帧取证**：89 508 B，1280×720 |
| `frame_gdi.jpg` | **GDI 单帧取证**：89 473 B，1280×720 |
| `browser.png` | Chrome headless 真实渲染截图（canvas 画面 + HUD） |
| `browser_hud.txt` | 浏览器 HUD 读数（15.0 fps / 10.74 Mbps / 7.7 ms / 106 帧） |

复现命令：

```powershell
# 1) 构建
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1

# 2) 单帧取证（DXGI / GDI）
.\build\screenstream.exe --grab evidence\frame.jpg --grab-wait 3000
.\build\screenstream.exe --capture gdi --grab evidence\frame_gdi.jpg

# 3) 启动服务
.\build\screenstream.exe --port 8203 --fps 15

# 4) HTTP + 30 帧统计（另开一个终端）
(Invoke-WebRequest -UseBasicParsing http://127.0.0.1:8203/).StatusCode
node tools\ws_probe.mjs ws://127.0.0.1:8203/ws 30

# 5) 浏览器渲染（可选，需要 Chrome）
#    启动 chrome --headless=new --remote-debugging-port=9223 --user-data-dir=<临时目录> about:blank
node tools\browser_shot.mjs 9223 http://127.0.0.1:8203/ evidence\browser.png 7000
```
