// SPDX-License-Identifier: GPL-2.0-or-later
// Manhattan Engine: messages go to the console and to manhattan.log (beside the program's working
// folder), so they can be sent with a problem report.

#pragma once

#include <cstdarg>
#include <cstdio>

namespace engine {

inline std::FILE*& LogFile() {
    static std::FILE* file = nullptr;
    return file;
}

inline void OpenLog(const char* path) {
    LogFile() = std::fopen(path, "w");
}

#if defined(__GNUC__)
__attribute__((format(printf, 1, 2)))
#endif
inline void Log(const char* format, ...) {
    std::va_list args;
    va_start(args, format);
    std::va_list copy;
    va_copy(copy, args);
    std::vfprintf(stdout, format, args);
    std::fflush(stdout);
    if (std::FILE* f = LogFile()) {
        std::vfprintf(f, format, copy);
        std::fflush(f);
    }
    va_end(copy);
    va_end(args);
}

} // namespace engine
