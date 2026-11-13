#include "frame_hub.h"

namespace ss {

void FrameHub::Publish(std::shared_ptr<const Frame> frame) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        latest_ = std::move(frame);
        ++version_;
    }
}

std::shared_ptr<const Frame> FrameHub::Latest(uint64_t& seen_version) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (version_ <= seen_version) {
        return nullptr;
    }
    seen_version = version_;
    return latest_;
}

} // namespace ss
