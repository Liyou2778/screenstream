#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

// winsock2.h must always precede windows.h on Windows.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <mutex>
#include <string>
#include <vector>

namespace ss {

// Microseconds since the Unix epoch (same clock domain as JS Date.now()/performance.timeOrigin).
int64_t NowEpochUs();

// Thread safe timestamped console logging.
void Log(const char* fmt, ...);

std::string Format(const char* fmt, ...);

// HRESULT / winsock error to readable text.
std::string HrToString(long hr);

// Cheap "is this BGRA image completely black" sampler (used to detect capture failures).
bool LooksAllBlack(const uint8_t* bgra, size_t bytes);

} // namespace ss
