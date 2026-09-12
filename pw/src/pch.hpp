#pragma once

// Precompiled header for the Peace Walker ASI (MGSPWEnabler). The same set as the MGS4
// build's: the shared core sources are compiled against it, and both renderers are kept
// because which one Peace Walker uses is settled in-process, not on disk.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <SDKDDKVer.h>
#include <Windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <d3d11_1.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iomanip>
#include <limits>
#include <map>
#include <mutex>
#include <numbers>
#include <optional>
#include <regex>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <safetyhook.hpp>
#include <spdlog/spdlog.h>
