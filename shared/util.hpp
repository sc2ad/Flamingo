#pragma once
#include <cassert>

#ifdef ANDROID
#include "beatsaber-hook/shared/utils/utils-functions.h"
#include "paper/shared/logger.hpp"

#ifndef NDEBUG
#define FLAMINGO_ASSERT(...) assert(__VA_ARGS__)
#define FLAMINGO_DEBUG(...) Paper::Logger::fmtLog<Paper::LogLevel::DBG>(__VA_ARGS__)
#else
#define FLAMINGO_ASSERT(...) __builtin_assume(__VA_ARGS__)
#define FLAMINGO_DEBUG(...)
#endif

#define FLAMINGO_CRITICAL(...) Paper::Logger::fmtLog<Paper::LogLevel::CRIT>(__VA_ARGS__)
#define FLAMINGO_ABORT(...)             \
    do {                                \
        FLAMINGO_CRITICAL(__VA_ARGS__); \
        Paper::Logger::WaitForFlush();  \
        SAFE_ABORT();                   \
    } while (0)

#else
#include <fmt/core.h>
#include <cstddef>
#include <cstdio>

#define FLAMINGO_ASSERT(...) assert(__VA_ARGS__)
#define FLAMINGO_DEBUG(...)  \
    fmt::print(__VA_ARGS__); \
    puts("")
#define FLAMINGO_CRITICAL(...) \
    fmt::print(__VA_ARGS__);   \
    puts("")
#define FLAMINGO_ABORT(...)  \
    fmt::print(__VA_ARGS__); \
    puts("");                \
    std::abort()
#endif
