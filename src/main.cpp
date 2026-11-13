#include "capture.h"
#include "common.h"
#include "frame_hub.h"
#include "jpeg_encoder.h"
#include "page.h"
#include "server.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

namespace {

struct Options {
    int port = 8203;
    std::string bind_address = "0.0.0.0";
    int fps = 15;
    int width = 1280;
    int height = 720;
    double quality = 0.72;
    int duration_seconds = 0;
    int grab_wait_ms = 0;
    std::string capture_backend = "auto";
    std::string grab_path;
};

std::atomic<bool> g_stop{false};

BOOL WINAPI ConsoleHandler(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT) {
        g_stop.store(true);
        return TRUE;
    }
    return FALSE;
}

void PrintUsage() {
    std::printf(
        "screenstream - Windows desktop streaming to a browser\n"
        "\n"
        "Usage: screenstream.exe [options]\n"
        "  --port <n>        HTTP/WebSocket port (default 8203)\n"
        "  --bind <ip>       Bind address (default 0.0.0.0, use 127.0.0.1 for local only)\n"
        "  --fps <n>         Target frame rate (default 15)\n"
        "  --width <n>       Encoded frame width (default 1280)\n"
        "  --height <n>      Encoded frame height (default 720)\n"
        "  --quality <f>     JPEG quality 0.05-1.0 (default 0.72)\n"
        "  --duration <s>    Stop automatically after <s> seconds (default: run until Ctrl+C)\n"
        "  --capture <auto|dxgi|gdi>  Force a capture backend (default auto: DXGI then GDI)\n"
        "  --grab-wait <ms>  With --grab: keep sampling until a non-black frame arrives (max <ms>)\n"
        "  --grab <path>     Capture exactly one frame, encode it to <path> and exit\n"
        "  --help            Show this help\n"
        "\n"
        "Playback page: http://127.0.0.1:<port>/\n");
}

bool ParseArguments(int argc, char** argv, Options& options, bool& show_help, std::string& error) {
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        const bool has_value = (i + 1 < argc);
        if (argument == "--help" || argument == "-h") {
            show_help = true;
            return true;
        }
        if (argument == "--port") {
            if (!has_value) { error = "missing value for --port"; return false; }
            options.port = std::atoi(argv[++i]);
        } else if (argument == "--bind") {
            if (!has_value) { error = "missing value for --bind"; return false; }
            options.bind_address = argv[++i];
        } else if (argument == "--fps") {
            if (!has_value) { error = "missing value for --fps"; return false; }
            options.fps = std::atoi(argv[++i]);
        } else if (argument == "--width") {
            if (!has_value) { error = "missing value for --width"; return false; }
            options.width = std::atoi(argv[++i]);
        } else if (argument == "--height") {
            if (!has_value) { error = "missing value for --height"; return false; }
            options.height = std::atoi(argv[++i]);
        } else if (argument == "--quality") {
            if (!has_value) { error = "missing value for --quality"; return false; }
            options.quality = std::atof(argv[++i]);
        } else if (argument == "--duration") {
            if (!has_value) { error = "missing value for --duration"; return false; }
            options.duration_seconds = std::atoi(argv[++i]);
        } else if (argument == "--capture") {
            if (!has_value) { error = "missing value for --capture"; return false; }
            options.capture_backend = argv[++i];
        } else if (argument == "--grab-wait") {
            if (!has_value) { error = "missing value for --grab-wait"; return false; }
            options.grab_wait_ms = std::atoi(argv[++i]);
        } else if (argument == "--grab") {
            if (!has_value) { error = "missing value for --grab"; return false; }
            options.grab_path = argv[++i];
        } else {
            error = "unknown argument: " + argument;
            return false;
        }
    }

    if (options.port <= 0 || options.port > 65535) {
        error = "port out of range";
        return false;
    }
    if (options.fps <= 0 || options.fps > 120) {
        error = "fps out of range (1-120)";
        return false;
    }
    if (options.width < 16 || options.height < 16 || options.width > 3840 || options.height > 2160) {
        error = "frame size out of range";
        return false;
    }
    if (options.quality < 0.05 || options.quality > 1.0) {
        error = "quality out of range (0.05-1.0)";
        return false;
    }
    if (options.capture_backend != "auto" && options.capture_backend != "dxgi" &&
        options.capture_backend != "gdi") {
        error = "capture backend must be auto, dxgi or gdi";
        return false;
    }
    return true;
}

// Initializes capture honoring the --capture preference.
bool InitCapture(ss::DesktopCapture& capture, const Options& options) {
    if (!capture.Init()) {
        ss::Log("failed to initialize desktop capture: %s", capture.last_error().c_str());
        return false;
    }
    if (options.capture_backend == "gdi" && capture.IsDxgi()) {
        ss::Log("--capture gdi requested, releasing DXGI backend");
        if (!capture.ForceGdiFallback()) {
            ss::Log("GDI fallback failed: %s", capture.last_error().c_str());
            return false;
        }
    } else if (options.capture_backend == "dxgi" && !capture.IsDxgi()) {
        ss::Log("--capture dxgi requested but DXGI is unavailable, using: %s", capture.mode().c_str());
    }
    return true;
}

bool GrabWithTimeout(ss::DesktopCapture& capture, ss::CaptureFrame& frame, int timeout_ms,
                     int* frames_seen = nullptr) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (capture.Grab(frame) == ss::GrabResult::Fresh) {
            if (frames_seen != nullptr) {
                ++*frames_seen;
            }
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

int RunGrabOnce(const Options& options) {
    ss::DesktopCapture capture;
    if (!InitCapture(capture, options)) {
        return 2;
    }
    ss::Log("capture backend: %s", capture.mode().c_str());

    ss::JpegEncoder encoder;
    if (!encoder.Init(options.width, options.height, options.quality)) {
        ss::Log("failed to initialize JPEG encoder: %s", encoder.last_error().c_str());
        return 3;
    }

    ss::CaptureFrame frame;
    int frames_seen = 0;
    int black_frames = 0;
    bool have_frame = false;
    bool have_non_black = false;

    // --grab-wait N: keep sampling up to N ms until a non-black frame shows up.
    // Without it the very first frame is accepted (which can be an empty frame
    // right after the duplication session starts).
    const int wait_ms = options.grab_wait_ms > 0 ? options.grab_wait_ms : 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms > 0 ? wait_ms : 3000);
    while (std::chrono::steady_clock::now() < deadline) {
        if (capture.Grab(frame) == ss::GrabResult::Fresh) {
            ++frames_seen;
            have_frame = true;
            if (ss::LooksAllBlack(frame.bgra.data(), frame.bgra.size())) {
                ++black_frames;
            } else {
                have_non_black = true;
                break;
            }
            if (wait_ms <= 0) {
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    if (!have_frame) {
        ss::Log("no frame captured within %d ms (last error: %s)", wait_ms > 0 ? wait_ms : 3000,
                capture.last_error().c_str());
        if (capture.IsDxgi()) {
            ss::Log("retrying with GDI BitBlt");
            if (capture.ForceGdiFallback()) {
                have_frame = GrabWithTimeout(capture, frame, 2000, &frames_seen);
            }
        }
    } else if (!have_non_black && capture.IsDxgi()) {
        ss::Log("DXGI returned only black frames (%d sampled), retrying with GDI BitBlt", black_frames);
        if (capture.ForceGdiFallback()) {
            int gdi_frames = 0;
            const auto gdi_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
            while (std::chrono::steady_clock::now() < gdi_deadline) {
                if (capture.Grab(frame) == ss::GrabResult::Fresh) {
                    ++gdi_frames;
                    frames_seen++;
                    if (!ss::LooksAllBlack(frame.bgra.data(), frame.bgra.size())) {
                        have_non_black = true;
                        break;
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            ss::Log("GDI fallback produced %d frame(s), non-black=%s", gdi_frames,
                    have_non_black ? "yes" : "no");
        }
    }

    if (!have_frame) {
        ss::Log("no frame captured (last error: %s)", capture.last_error().c_str());
        return 4;
    }

    ss::Log("captured %dx%d frame via %s (%d frame(s) sampled, %d black), all-black=%s",
            frame.width, frame.height, capture.mode().c_str(), frames_seen, black_frames,
            ss::LooksAllBlack(frame.bgra.data(), frame.bgra.size()) ? "yes" : "no");

    std::vector<uint8_t> jpeg;
    if (!encoder.Encode(frame, jpeg) || jpeg.empty()) {
        ss::Log("JPEG encode failed: %s", encoder.last_error().c_str());
        return 5;
    }

    namespace fs = std::filesystem;
    const fs::path output_path = fs::absolute(fs::path(options.grab_path));
    if (output_path.has_parent_path()) {
        std::error_code ec;
        fs::create_directories(output_path.parent_path(), ec);
    }
    std::ofstream file(output_path, std::ios::binary);
    if (!file) {
        ss::Log("cannot open output file: %s", output_path.string().c_str());
        return 6;
    }
    file.write(reinterpret_cast<const char*>(jpeg.data()), static_cast<std::streamsize>(jpeg.size()));
    file.close();

    ss::Log("wrote %s (%zu bytes JPEG, %dx%d)",
            output_path.string().c_str(), jpeg.size(), options.width, options.height);
    return 0;
}

void ProducerLoop(const Options& options, ss::DesktopCapture& capture, ss::FrameHub& hub,
                  ss::Stats& stats, std::atomic<bool>& stop) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    ss::JpegEncoder encoder;
    if (!encoder.Init(options.width, options.height, options.quality)) {
        ss::Log("producer: JPEG encoder init failed: %s", encoder.last_error().c_str());
        CoUninitialize();
        return;
    }

    ss::CaptureFrame frame;
    std::shared_ptr<const std::vector<uint8_t>> last_jpeg;
    uint32_t sequence = 0;
    int fps_frames = 0;
    bool first_frame_logged = false;
    bool first_black_logged = false;
    bool ever_non_black = false;
    bool ever_fresh = false;
    std::chrono::steady_clock::time_point first_black_at{};

    const auto tick = std::chrono::microseconds(1000000 / std::max(1, options.fps));
    auto next_tick = std::chrono::steady_clock::now();
    auto fps_window_start = next_tick;
    const auto started = next_tick;

    while (!stop.load()) {
        const auto now = std::chrono::steady_clock::now();
        const ss::GrabResult result = capture.Grab(frame);
        if (result == ss::GrabResult::Fresh) {
            ever_fresh = true;
            const bool black = ss::LooksAllBlack(frame.bgra.data(), frame.bgra.size());
            if (!black) {
                ever_non_black = true;
                first_black_at = {};
            } else {
                if (!first_black_logged) {
                    ss::Log("warning: captured frame is completely black - DXGI can return a blank first frame, "
                            "waiting for a real one");
                    first_black_logged = true;
                }
                if (first_black_at == std::chrono::steady_clock::time_point{}) {
                    first_black_at = now;
                }
                // DXGI duplication on some configurations (hybrid GPUs, connected
                // sessions) only ever produces black frames. Detect that quickly and
                // switch to the GDI BitBlt backend which does see the desktop.
                if (!ever_non_black && capture.IsDxgi() &&
                    now - first_black_at > std::chrono::milliseconds(1500)) {
                    ss::Log("DXGI keeps returning black frames, switching to GDI BitBlt");
                    if (capture.ForceGdiFallback()) {
                        stats.SetCaptureMode(capture.mode());
                        first_black_at = {};
                        first_frame_logged = false;
                    } else {
                        ss::Log("GDI fallback failed: %s", capture.last_error().c_str());
                    }
                }
            }

            std::vector<uint8_t> jpeg;
            if (encoder.Encode(frame, jpeg) && !jpeg.empty()) {
                last_jpeg = std::make_shared<std::vector<uint8_t>>(std::move(jpeg));
                if (stats.frames_produced.load() == 0 || !first_frame_logged) {
                    first_frame_logged = true;
                    ss::Log("first frame: JPEG %zu bytes, %dx%d, capture=%s, black=%s",
                            last_jpeg->size(), options.width, options.height,
                            capture.mode().c_str(), black ? "yes" : "no");
                }
            } else {
                ss::Log("JPEG encode failed: %s", encoder.last_error().c_str());
            }
        } else if (result == ss::GrabResult::NoNewFrame) {
            // Static desktop: keep the previous JPEG and resend it at the target frame rate.
        }

        // If DXGI is usable but the desktop never changes, it can deliver no frame at all.
        // Fall back to GDI so the stream always has a picture.
        if (!ever_fresh && capture.IsDxgi() &&
            std::chrono::steady_clock::now() - started > std::chrono::seconds(3)) {
            ss::Log("DXGI delivered no frame within 3s, switching to GDI BitBlt");
            if (capture.ForceGdiFallback()) {
                stats.SetCaptureMode(capture.mode());
                first_frame_logged = false;
            } else {
                ss::Log("GDI fallback failed: %s", capture.last_error().c_str());
            }
        }

        if (last_jpeg && !last_jpeg->empty()) {
            auto published = std::make_shared<ss::Frame>();
            published->seq = ++sequence;
            published->ts_us = ss::NowEpochUs();
            published->width = static_cast<uint16_t>(options.width);
            published->height = static_cast<uint16_t>(options.height);
            published->jpeg = last_jpeg;
            hub.Publish(published);
            stats.frames_produced.fetch_add(1);
            stats.bytes_produced.fetch_add(last_jpeg->size());
            stats.last_frame_bytes.store(static_cast<int>(last_jpeg->size()));
            ++fps_frames;
        }

        const auto now_after = std::chrono::steady_clock::now();
        if (now_after - fps_window_start >= std::chrono::seconds(1)) {
            const double elapsed = std::chrono::duration<double>(now_after - fps_window_start).count();
            stats.producer_fps.store(fps_frames / elapsed);
            fps_window_start = now_after;
            fps_frames = 0;
        }

        next_tick += tick;
        std::this_thread::sleep_until(next_tick);
        const auto behind = std::chrono::steady_clock::now() - next_tick;
        if (behind > std::chrono::seconds(2)) {
            next_tick = std::chrono::steady_clock::now();
        }
    }

    encoder.Shutdown();
    CoUninitialize();
}

int RunServer(const Options& options) {
    ss::DesktopCapture capture;
    if (!InitCapture(capture, options)) {
        return 2;
    }
    ss::Log("capture backend: %s", capture.mode().c_str());

    ss::FrameHub hub;
    ss::Stats stats;
    stats.SetCaptureMode(capture.mode());
    stats.width.store(options.width);
    stats.height.store(options.height);
    stats.target_fps.store(options.fps);
    stats.started_epoch_s.store(static_cast<int64_t>(std::time(nullptr)));

    ss::HttpWsServer server(hub, stats, ss::PlaybackHtml());
    if (!server.Start(options.bind_address, options.port)) {
        ss::Log("failed to start HTTP/WebSocket server on port %d", options.port);
        return 4;
    }
    ss::Log("playback page : http://127.0.0.1:%d/", options.port);
    ss::Log("stats API     : http://127.0.0.1:%d/api/stats", options.port);
    ss::Log("websocket     : ws://127.0.0.1:%d/ws", options.port);

    std::thread producer(ProducerLoop, std::cref(options), std::ref(capture), std::ref(hub),
                         std::ref(stats), std::ref(g_stop));

    if (options.duration_seconds > 0) {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(options.duration_seconds);
        while (!g_stop.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    } else {
        while (!g_stop.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    g_stop.store(true);

    if (producer.joinable()) {
        producer.join();
    }
    server.Stop();
    ss::Log("final stats: %s", stats.ToJson().c_str());
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);
    // Per-monitor DPI awareness so GDI BitBlt sees physical pixels.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com_result)) {
        ss::Log("CoInitializeEx failed: %s", ss::HrToString(com_result).c_str());
        return 1;
    }

    Options options;
    bool show_help = false;
    std::string error;
    if (!ParseArguments(argc, argv, options, show_help, error)) {
        ss::Log("argument error: %s", error.c_str());
        PrintUsage();
        CoUninitialize();
        return 1;
    }
    if (show_help) {
        PrintUsage();
        CoUninitialize();
        return 0;
    }

    int result = 0;
    if (!options.grab_path.empty()) {
        result = RunGrabOnce(options);
    } else {
        result = RunServer(options);
    }

    CoUninitialize();
    return result;
}
