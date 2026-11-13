#pragma once

#include "common.h"

#include <memory>
#include <mutex>

namespace ss {

// One encoded video frame, shared between the producer and every connected client.
struct Frame {
    uint32_t seq = 0;
    int64_t ts_us = 0; // Unix epoch microseconds, stamped when the frame was published
    uint16_t width = 0;
    uint16_t height = 0;
    std::shared_ptr<const std::vector<uint8_t>> jpeg;
};

// Single-slot latest-frame mailbox. Clients poll Latest() and only get new versions,
// which naturally drops frames for slow consumers instead of queueing them.
class FrameHub {
public:
    void Publish(std::shared_ptr<const Frame> frame);
    std::shared_ptr<const Frame> Latest(uint64_t& seen_version) const;

private:
    mutable std::mutex mutex_;
    uint64_t version_ = 0;
    std::shared_ptr<const Frame> latest_;
};

} // namespace ss
