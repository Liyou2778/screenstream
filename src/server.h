#pragma once

#include "common.h"
#include "frame_hub.h"
#include "protocol.h"
#include "thread_pool.h"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

namespace ss {

struct StatsSnapshot {
    std::string capture_mode;
    int width = 0;
    int height = 0;
    int target_fps = 0;
    double producer_fps = 0.0;
    uint64_t frames_produced = 0;
    uint64_t bytes_produced = 0;
    uint64_t frames_sent = 0;
    uint64_t bytes_sent = 0;
    double avg_frame_kb = 0.0;
    double bitrate_mbps = 0.0;
    int clients = 0;
    int last_frame_bytes = 0;
    int64_t uptime_seconds = 0;
    uint64_t input_events = 0;
};

class Stats {
public:
    std::atomic<uint64_t> frames_produced{0};
    std::atomic<uint64_t> bytes_produced{0};
    std::atomic<uint64_t> frames_sent{0};
    std::atomic<uint64_t> bytes_sent{0};
    std::atomic<uint64_t> input_events{0};
    std::atomic<int> clients{0};
    std::atomic<int> last_frame_bytes{0};
    std::atomic<int> width{0};
    std::atomic<int> height{0};
    std::atomic<int> target_fps{0};
    std::atomic<int64_t> started_epoch_s{0};
    std::atomic<double> producer_fps{0.0};

    // Capture backend currently in use: 0 = none, 1 = dxgi, 2 = gdi.
    std::atomic<int> capture_mode{0};

    void SetCaptureMode(const std::string& name);
    std::string CaptureModeName() const;

    StatsSnapshot Snapshot() const;
    std::string ToJson() const;
};

class HttpWsServer {
public:
    HttpWsServer(FrameHub& hub, Stats& stats, std::string page);
    ~HttpWsServer();

    HttpWsServer(const HttpWsServer&) = delete;
    HttpWsServer& operator=(const HttpWsServer&) = delete;

    bool Start(const std::string& bind_address, int port);
    void Stop();

private:
    struct HttpRequest {
        std::string method;
        std::string target;
        std::string path;
        std::string query;
        std::string version;
        std::map<std::string, std::string> headers;
    };

    struct WsClientState {
        bool closed = false;
        bool input_enabled = false;
        std::string rx_buffer;
        std::string fragment;
        int fragment_opcode = 0;
    };

    void AcceptLoop();
    void HandleClient(SOCKET client);
    bool ReadHttpRequest(SOCKET client, std::string& request);
    bool ParseHttpRequest(const std::string& raw, HttpRequest& request);
    void SendHttp(SOCKET client, int status, const char* reason, const std::string& content_type,
                  const std::string& body);
    bool SendAll(SOCKET client, const uint8_t* data, size_t length);
    void ServeWebSocket(SOCKET client, const std::string& accept_key);
    bool ProcessWsIncoming(SOCKET client, WsClientState& state);
    void HandleWsPayload(SOCKET client, WsClientState& state, int opcode, const std::string& payload);
    void HandleInputJson(const std::string& message);
    bool SendFrameToClient(SOCKET client, const Frame& frame);

    FrameHub& hub_;
    Stats& stats_;
    std::string page_;
    ThreadPool pool_;
    std::thread accept_thread_;
    SOCKET listen_socket_ = INVALID_SOCKET;
    std::atomic<bool> running_{false};
    std::mutex clients_mutex_;
    std::set<SOCKET> clients_;
};

} // namespace ss
