#pragma once

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#ifdef _WIN32
#include <windows.h>
#undef CreateEvent
#undef CreateFile
#undef CreateSemaphore
#endif

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include <fmt/core.h>

#include <ppc/ppc_config.h>
#include <ppc/ppc_context.h>
#include <xbox.h>

#include "os/logger.h"

// An environment variable's value, read once per call site (getenv scans the environment block under a lock, too
// slow for per-draw or per-packet checks).
#define AR_ENV(name) ([]() -> const char* { static const char* const value = getenv(name); return value; }())
