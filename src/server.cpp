#include "server.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <map>
#include <sstream>

namespace ss {

namespace {

constexpr int kThreadPoolSize = 8;
constexpr size_t kMaxHttpHeaderBytes = 64 * 1024;

std::string ToLower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string Trim(const std::string& text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r' ||
                           text[begin] == '\n')) {
        ++begin;
    }
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r' ||
                           text[end - 1] == '\n')) {
        --end;
    }
    return text.substr(begin, end - begin);
}

bool StartsWith(const std::string& text, const char* prefix) {
    const size_t length = std::strlen(prefix);
    return text.size() >= length && text.compare(0, length, prefix) == 0;
}

int ParseIntField(const std::string& json, const char* name, int fallback) {
    const std::string needle = std::string("\"") + name + "\"";
    size_t position = json.find(needle);
    if (position == std::string::npos) {
        return fallback;
    }
    position = json.find(':', position + needle.size());
    if (position == std::string::npos) {
        return fallback;
    }
    ++position;
    while (position < json.size() && (json[position] == ' ' || json[position] == '\t')) {
        ++position;
    }
    bool negative = false;
    if (position < json.size() && json[position] == '-') {
        negative = true;
        ++position;
    }
    int value = 0;
    bool any = false;
    while (position < json.size() && json[position] >= '0' && json[position] <= '9') {
        value = value * 10 + (json[position] - '0');
        ++position;
        any = true;
    }
    if (!any) {
        return fallback;
    }
    return negative ? -value : value;
}

bool BoolField(const std::string& json, const char* name) {
    const std::string needle = std::string("\"") + name + "\"";
    size_t position = json.find(needle);
    if (position == std::string::npos) {
        return false;
    }
    position = json.find(':', position + needle.size());
    if (position == std::string::npos) {
        return false;
    }
    const size_t value_pos = json.find("true", position);
    return value_pos != std::string::npos && value_pos - position <= 4;
}

} // namespace

// ---------------------------------------------------------------------------
// Stats
// ---------------------------------------------------------------------------

void Stats::SetCaptureMode(const std::string& name) {
    int value = 0;
    if (name == "dxgi") {
        value = 1;
    } else if (name == "gdi") {
        value = 2;
    }
    capture_mode.store(value);
}

std::string Stats::CaptureModeName() const {
    switch (capture_mode.load()) {
        case 1:
            return "dxgi";
        case 2:
            return "gdi";
        default:
            return "none";
    }
}

StatsSnapshot Stats::Snapshot() const {
    StatsSnapshot snapshot;
    snapshot.capture_mode = CaptureModeName();
    snapshot.width = width.load();
    snapshot.height = height.load();
    snapshot.target_fps = target_fps.load();
    snapshot.producer_fps = producer_fps.load();
    snapshot.frames_produced = frames_produced.load();
    snapshot.bytes_produced = bytes_produced.load();
    snapshot.frames_sent = frames_sent.load();
    snapshot.bytes_sent = bytes_sent.load();
    snapshot.clients = clients.load();
    snapshot.last_frame_bytes = last_frame_bytes.load();
    snapshot.input_events = input_events.load();
    const int64_t started = started_epoch_s.load();
    if (started > 0) {
        snapshot.uptime_seconds = static_cast<int64_t>(std::time(nullptr)) - started;
        if (snapshot.uptime_seconds < 0) {
            snapshot.uptime_seconds = 0;
        }
    }
    if (snapshot.frames_produced > 0) {
        snapshot.avg_frame_kb =
            static_cast<double>(snapshot.bytes_produced) / static_cast<double>(snapshot.frames_produced) / 1024.0;
    }
    if (snapshot.uptime_seconds > 0) {
        snapshot.bitrate_mbps = static_cast<double>(snapshot.bytes_produced) * 8.0 / 1e6 /
                                static_cast<double>(snapshot.uptime_seconds);
    }
    return snapshot;
}

std::string Stats::ToJson() const {
    const StatsSnapshot snapshot = Snapshot();
    return Format(
        "{\"capture_mode\":\"%s\",\"width\":%d,\"height\":%d,\"target_fps\":%d,"
        "\"producer_fps\":%.2f,\"frames_produced\":%llu,\"bytes_produced\":%llu,"
        "\"frames_sent\":%llu,\"bytes_sent\":%llu,\"avg_frame_kb\":%.2f,"
        "\"bitrate_mbps\":%.2f,\"clients\":%d,\"last_frame_bytes\":%d,"
        "\"uptime_seconds\":%lld,\"input_events\":%llu}",
        snapshot.capture_mode.c_str(), snapshot.width, snapshot.height, snapshot.target_fps,
        snapshot.producer_fps, static_cast<unsigned long long>(snapshot.frames_produced),
        static_cast<unsigned long long>(snapshot.bytes_produced),
        static_cast<unsigned long long>(snapshot.frames_sent),
        static_cast<unsigned long long>(snapshot.bytes_sent), snapshot.avg_frame_kb,
        snapshot.bitrate_mbps, snapshot.clients, snapshot.last_frame_bytes,
        static_cast<long long>(snapshot.uptime_seconds),
        static_cast<unsigned long long>(snapshot.input_events));
}

// ---------------------------------------------------------------------------
// HttpWsServer
// ---------------------------------------------------------------------------

HttpWsServer::HttpWsServer(FrameHub& hub, Stats& stats, std::string page)
    : hub_(hub), stats_(stats), page_(std::move(page)), pool_(kThreadPoolSize) {}

HttpWsServer::~HttpWsServer() {
    Stop();
}

bool HttpWsServer::Start(const std::string& bind_address, int port) {
    WSADATA wsa_data{};
    const int wsa_result = WSAStartup(MAKEWORD(2, 2), &wsa_data);
    if (wsa_result != 0) {
        Log("WSAStartup failed: %d", wsa_result);
        return false;
    }

    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        Log("socket() failed: %d", WSAGetLastError());
        WSACleanup();
        return false;
    }

    BOOL reuse = TRUE;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<unsigned short>(port));
    if (bind_address.empty() || bind_address == "0.0.0.0") {
        address.sin_addr.s_addr = INADDR_ANY;
    } else {
        if (inet_pton(AF_INET, bind_address.c_str(), &address.sin_addr) != 1) {
            Log("invalid bind address: %s", bind_address.c_str());
            closesocket(listener);
            WSACleanup();
            return false;
        }
    }

    if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        Log("bind(port %d) failed: %d", port, WSAGetLastError());
        closesocket(listener);
        WSACleanup();
        return false;
    }
    if (listen(listener, SOMAXCONN) == SOCKET_ERROR) {
        Log("listen() failed: %d", WSAGetLastError());
        closesocket(listener);
        WSACleanup();
        return false;
    }

    listen_socket_ = listener;
    running_.store(true);
    accept_thread_ = std::thread([this] { AcceptLoop(); });
    Log("HTTP/WebSocket server listening on %s:%d (threads=%d)", bind_address.c_str(), port,
        kThreadPoolSize);
    return true;
}

void HttpWsServer::Stop() {
    if (!running_.exchange(false)) {
        if (listen_socket_ != INVALID_SOCKET) {
            closesocket(listen_socket_);
            listen_socket_ = INVALID_SOCKET;
        }
        return;
    }

    if (listen_socket_ != INVALID_SOCKET) {
        closesocket(listen_socket_);
        listen_socket_ = INVALID_SOCKET;
    }
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        for (SOCKET client : clients_) {
            shutdown(client, SD_BOTH);
        }
    }
    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }
    pool_.Shutdown();
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        clients_.clear();
    }
    WSACleanup();
    Log("HTTP/WebSocket server stopped");
}

void HttpWsServer::AcceptLoop() {
    while (running_.load()) {
        sockaddr_in peer{};
        int peer_length = static_cast<int>(sizeof(peer));
        SOCKET client = accept(listen_socket_, reinterpret_cast<sockaddr*>(&peer), &peer_length);
        if (client == INVALID_SOCKET) {
            if (!running_.load()) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            if (clients_.size() >= 64) {
                closesocket(client);
                continue;
            }
            clients_.insert(client);
        }
        char peer_text[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &peer.sin_addr, peer_text, sizeof(peer_text));
        Log("connection from %s:%u", peer_text, static_cast<unsigned>(ntohs(peer.sin_port)));
        pool_.Submit([this, client] { HandleClient(client); });
    }
}

bool HttpWsServer::SendAll(SOCKET client, const uint8_t* data, size_t length) {
    size_t sent_total = 0;
    while (sent_total < length) {
        const int chunk = static_cast<int>(std::min<size_t>(length - sent_total, 256 * 1024));
        const int sent = send(client, reinterpret_cast<const char*>(data + sent_total), chunk, 0);
        if (sent <= 0) {
            return false;
        }
        sent_total += static_cast<size_t>(sent);
    }
    return true;
}

bool HttpWsServer::ReadHttpRequest(SOCKET client, std::string& request) {
    char buffer[4096];
    while (request.size() < kMaxHttpHeaderBytes) {
        const int received = recv(client, buffer, sizeof(buffer), 0);
        if (received <= 0) {
            return false;
        }
        request.append(buffer, static_cast<size_t>(received));
        if (request.find("\r\n\r\n") != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool HttpWsServer::ParseHttpRequest(const std::string& raw, HttpRequest& request) {
    const size_t first_line_end = raw.find("\r\n");
    if (first_line_end == std::string::npos) {
        return false;
    }
    std::istringstream line_stream(raw.substr(0, first_line_end));
    if (!(line_stream >> request.method >> request.target >> request.version)) {
        return false;
    }
    const size_t query_position = request.target.find('?');
    if (query_position == std::string::npos) {
        request.path = request.target;
    } else {
        request.path = request.target.substr(0, query_position);
        request.query = request.target.substr(query_position + 1);
    }

    size_t position = first_line_end + 2;
    while (position < raw.size()) {
        const size_t line_end = raw.find("\r\n", position);
        if (line_end == std::string::npos || line_end == position) {
            break;
        }
        const std::string header = raw.substr(position, line_end - position);
        const size_t colon = header.find(':');
        if (colon != std::string::npos) {
            request.headers[ToLower(Trim(header.substr(0, colon)))] = Trim(header.substr(colon + 1));
        }
        position = line_end + 2;
    }
    return true;
}

void HttpWsServer::SendHttp(SOCKET client, int status, const char* reason, const std::string& content_type,
                            const std::string& body) {
    const std::string head = Format(
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
        "Cache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n",
        status, reason, content_type.c_str(), body.size());
    if (!SendAll(client, reinterpret_cast<const uint8_t*>(head.data()), head.size())) {
        return;
    }
    if (!body.empty()) {
        SendAll(client, reinterpret_cast<const uint8_t*>(body.data()), body.size());
    }
}

void HttpWsServer::HandleClient(SOCKET client) {
    DWORD timeout_ms = 5000;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
    setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));

    std::string raw;
    HttpRequest request;
    const bool read_ok = ReadHttpRequest(client, raw);
    if (read_ok && ParseHttpRequest(raw, request)) {
        const bool wants_websocket = request.headers.find("sec-websocket-key") != request.headers.end() &&
                                     request.headers.find("upgrade") != request.headers.end() &&
                                     ToLower(request.headers["upgrade"]).find("websocket") != std::string::npos;
        if (request.path == "/ws") {
            if (wants_websocket) {
                ServeWebSocket(client, request.headers["sec-websocket-key"]);
            } else {
                SendHttp(client, 400, "Bad Request", "text/plain; charset=utf-8",
                         "WebSocket upgrade required\n");
            }
        } else if (request.path == "/" || request.path == "/index.html") {
            SendHttp(client, 200, "OK", "text/html; charset=utf-8", page_);
        } else if (request.path == "/api/stats") {
            SendHttp(client, 200, "OK", "application/json; charset=utf-8", stats_.ToJson());
        } else {
            SendHttp(client, 404, "Not Found", "text/plain; charset=utf-8", "not found\n");
        }
    } else if (read_ok) {
        SendHttp(client, 400, "Bad Request", "text/plain; charset=utf-8", "bad request\n");
    }

    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        clients_.erase(client);
    }
    closesocket(client);
}

void HttpWsServer::ServeWebSocket(SOCKET client, const std::string& accept_key) {
    const std::string accept =
        Sha1Base64(accept_key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
    const std::string response = Format(
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n\r\n",
        accept.c_str());
    if (!SendAll(client, reinterpret_cast<const uint8_t*>(response.data()), response.size())) {
        return;
    }

    stats_.clients.fetch_add(1);
    Log("websocket client connected, clients=%d", stats_.clients.load());

    WsClientState state;
    uint64_t seen_version = 0;
    uint8_t buffer[65536];

    while (running_.load() && !state.closed) {
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(client, &read_set);
        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = 5000; // 5 ms
        const int selected = select(0, &read_set, nullptr, nullptr, &timeout);
        if (selected == SOCKET_ERROR) {
            break;
        }
        if (selected > 0 && FD_ISSET(client, &read_set)) {
            const int received = recv(client, reinterpret_cast<char*>(buffer), sizeof(buffer), 0);
            if (received <= 0) {
                break;
            }
            state.rx_buffer.append(reinterpret_cast<char*>(buffer), static_cast<size_t>(received));
            if (!ProcessWsIncoming(client, state)) {
                break;
            }
            if (state.closed) {
                break;
            }
        }

        const std::shared_ptr<const Frame> frame = hub_.Latest(seen_version);
        if (frame && frame->jpeg && !frame->jpeg->empty()) {
            if (!SendFrameToClient(client, *frame)) {
                break;
            }
        }
    }

    if (!state.closed) {
        const std::vector<uint8_t> close_frame = MakeWebSocketFrame(0x8, nullptr, 0);
        SendAll(client, close_frame.data(), close_frame.size());
    }
    stats_.clients.fetch_sub(1);
    Log("websocket client disconnected, clients=%d", stats_.clients.load());
}

bool HttpWsServer::ProcessWsIncoming(SOCKET client, WsClientState& state) {
    size_t offset = 0;
    while (offset + 2 <= state.rx_buffer.size()) {
        const uint8_t byte0 = static_cast<uint8_t>(state.rx_buffer[offset]);
        const uint8_t byte1 = static_cast<uint8_t>(state.rx_buffer[offset + 1]);
        const bool fin = (byte0 & 0x80) != 0;
        const int opcode = byte0 & 0x0F;
        const bool masked = (byte1 & 0x80) != 0;
        uint64_t length = byte1 & 0x7F;
        size_t header_length = 2;

        if (length == 126) {
            if (offset + 4 > state.rx_buffer.size()) {
                break;
            }
            length = (static_cast<uint64_t>(static_cast<uint8_t>(state.rx_buffer[offset + 2])) << 8) |
                     static_cast<uint64_t>(static_cast<uint8_t>(state.rx_buffer[offset + 3]));
            header_length = 4;
        } else if (length == 127) {
            if (offset + 10 > state.rx_buffer.size()) {
                break;
            }
            length = 0;
            for (int i = 0; i < 8; ++i) {
                length = (length << 8) | static_cast<uint8_t>(state.rx_buffer[offset + 2 + i]);
            }
            header_length = 10;
        }
        if (!masked) {
            Log("protocol error: client frame is not masked");
            return false;
        }
        if (length > 16 * 1024 * 1024) {
            Log("protocol error: client frame too large (%llu bytes)", static_cast<unsigned long long>(length));
            return false;
        }
        const size_t total = header_length + 4 + static_cast<size_t>(length);
        if (offset + total > state.rx_buffer.size()) {
            break; // wait for the rest
        }

        const uint8_t* mask = reinterpret_cast<const uint8_t*>(state.rx_buffer.data()) + offset + header_length;
        const char* masked_payload = state.rx_buffer.data() + offset + header_length + 4;
        std::string payload(static_cast<size_t>(length), '\0');
        for (size_t i = 0; i < static_cast<size_t>(length); ++i) {
            payload[i] = static_cast<char>(static_cast<uint8_t>(masked_payload[i]) ^ mask[i % 4]);
        }
        offset += total;

        if (opcode == 0x8) { // close
            const std::vector<uint8_t> reply = MakeWebSocketFrame(0x8, nullptr, 0);
            SendAll(client, reply.data(), reply.size());
            state.closed = true;
            break;
        }
        if (opcode == 0x9) { // ping -> pong
            const std::vector<uint8_t> pong =
                MakeWebSocketFrame(0xA, reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
            if (!SendAll(client, pong.data(), pong.size())) {
                return false;
            }
            continue;
        }
        if (opcode == 0xA) { // pong
            continue;
        }
        if (opcode == 0x1 || opcode == 0x2) {
            if (!fin) {
                state.fragment_opcode = opcode;
                state.fragment = payload;
            } else {
                HandleWsPayload(client, state, opcode, payload);
            }
            continue;
        }
        if (opcode == 0x0) {
            state.fragment += payload;
            if (fin) {
                HandleWsPayload(client, state, state.fragment_opcode, state.fragment);
                state.fragment.clear();
                state.fragment_opcode = 0;
            }
        }
    }
    state.rx_buffer.erase(0, offset);
    return true;
}

void HttpWsServer::HandleWsPayload(SOCKET client, WsClientState& state, int opcode,
                                   const std::string& payload) {
    (void)client;
    (void)state;
    if (opcode != 0x1) {
        return; // only text control messages are used
    }
    if (payload.find("\"type\"") == std::string::npos) {
        return;
    }
    if (payload.find("\"input\"") != std::string::npos) {
        state.input_enabled = BoolField(payload, "enable");
        Log("remote input %s", state.input_enabled ? "enabled" : "disabled");
        return;
    }
    if (payload.find("\"mouse\"") != std::string::npos) {
        if (!state.input_enabled) {
            return;
        }
        HandleInputJson(payload);
    }
}

void HttpWsServer::HandleInputJson(const std::string& message) {
    const int x = ParseIntField(message, "x", -1);
    const int y = ParseIntField(message, "y", -1);
    const int click = ParseIntField(message, "click", 0);
    if (x < 0 || y < 0) {
        return;
    }
    const int target_width = std::max(1, stats_.width.load());
    const int target_height = std::max(1, stats_.height.load());
    const int screen_width = GetSystemMetrics(SM_CXSCREEN);
    const int screen_height = GetSystemMetrics(SM_CYSCREEN);
    if (screen_width <= 1 || screen_height <= 1) {
        return;
    }

    const int clamped_x = std::clamp(x, 0, target_width - 1);
    const int clamped_y = std::clamp(y, 0, target_height - 1);
    const double normalized_x = static_cast<double>(clamped_x) / static_cast<double>(target_width - 1);
    const double normalized_y = static_cast<double>(clamped_y) / static_cast<double>(target_height - 1);
    const int absolute_x = static_cast<int>(std::lround(normalized_x * (screen_width - 1)));
    const int absolute_y = static_cast<int>(std::lround(normalized_y * (screen_height - 1)));

    INPUT inputs[3] = {};
    int count = 0;
    inputs[count].type = INPUT_MOUSE;
    inputs[count].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    inputs[count].mi.dx = static_cast<LONG>(std::lround(normalized_x * 65535.0));
    inputs[count].mi.dy = static_cast<LONG>(std::lround(normalized_y * 65535.0));
    ++count;
    if (click != 0) {
        inputs[count].type = INPUT_MOUSE;
        inputs[count].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
        ++count;
        inputs[count].type = INPUT_MOUSE;
        inputs[count].mi.dwFlags = MOUSEEVENTF_LEFTUP;
        ++count;
    }
    if (SendInput(static_cast<UINT>(count), inputs, sizeof(INPUT)) != static_cast<UINT>(count)) {
        return;
    }
    (void)absolute_x;
    (void)absolute_y;
    stats_.input_events.fetch_add(1);
}

bool HttpWsServer::SendFrameToClient(SOCKET client, const Frame& frame) {
    FrameMeta meta;
    meta.seq = frame.seq;
    meta.ts_us = frame.ts_us;
    meta.width = frame.width;
    meta.height = frame.height;
    meta.format = kFrameFormatJpeg;

    const std::vector<std::vector<uint8_t>> chunks = ChunkFrame(meta, frame.jpeg->data(), frame.jpeg->size());
    uint64_t sent_bytes = 0;
    for (const std::vector<uint8_t>& chunk : chunks) {
        const std::vector<uint8_t> message = MakeWebSocketFrame(0x2, chunk.data(), chunk.size());
        if (!SendAll(client, message.data(), message.size())) {
            return false;
        }
        sent_bytes += message.size();
    }
    stats_.frames_sent.fetch_add(1);
    stats_.bytes_sent.fetch_add(sent_bytes);
    return true;
}

} // namespace ss
