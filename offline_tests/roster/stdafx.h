#pragma once
// stand-in for csgo_gc's stdafx.h for the offline backend_client test (no protobuf, no game)
#include <assert.h>
#include <array>
#include <atomic>
#include <charconv>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <list>
#include <optional>
#include <queue>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <vector>
namespace Platform { void Print(const char *format, ...); std::string CommandLine(); }
