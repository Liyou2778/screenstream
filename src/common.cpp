#include "common.h"

namespace ss {

int64_t NowEpochUs() {
    using namespace std::chrono;
    return duration_cast<microseconds>(system_clock::now().time_since_epoch()).count();
}

std::string Format(const char* fmt, ...) {
    char buffer[4096];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    return std::string(buffer);
}

void Log(const char* fmt, ...) {
    char buffer[4096];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    static std::mutex log_mutex;
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::lock_guard<std::mutex> lock(log_mutex);
    std::printf("[%02d:%02d:%02d.%03d] %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buffer);
    std::fflush(stdout);
}

std::string HrToString(long hr) {
    char* message = nullptr;
    DWORD len = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                   FORMAT_MESSAGE_IGNORE_INSERTS,
                               nullptr, static_cast<DWORD>(hr),
                               MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                               reinterpret_cast<char*>(&message), 0, nullptr);
    std::string text;
    if (len != 0 && message != nullptr) {
        text.assign(message, len);
        while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' ')) {
            text.pop_back();
        }
    } else {
        text = "unknown error";
    }
    if (message != nullptr) {
        LocalFree(message);
    }
    return Format("0x%08lX (%s)", static_cast<unsigned long>(hr), text.c_str());
}

bool LooksAllBlack(const uint8_t* bgra, size_t bytes) {
    if (bgra == nullptr || bytes < 4) {
        return true;
    }
    const size_t pixel_count = bytes / 4;
    // Sample up to 20000 pixels evenly spread across the image.
    const size_t step = pixel_count > 20000 ? pixel_count / 20000 : 1;
    for (size_t i = 0; i < pixel_count; i += step) {
        const uint8_t* p = bgra + i * 4;
        if (p[0] != 0 || p[1] != 0 || p[2] != 0) {
            return false;
        }
    }
    return true;
}

} // namespace ss
