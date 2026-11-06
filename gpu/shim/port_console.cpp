// SPDX-License-Identifier: GPL-2.0-or-later
// PROTOTYPE's Lua console, the port's side (port_console.h).

#include "port_console.h"

#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>

#include "../theordergpu.h"

namespace PortConsole {
namespace {

constexpr std::size_t kMaxLines = 400, kMaxHistory = 100;
std::mutex mutex;
std::deque<std::string> queue;
std::deque<Line> lines;
std::vector<std::string> history;
unsigned version = 0;

void Add(Kind kind, const std::string& text) {
    // One line per line of text: the log scrolls by lines.
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        lines.push_back({kind, text.substr(start, end - start)});
        if (lines.size() > kMaxLines) lines.pop_front();
        start = end + 1;
    }
    ++version;
}

} // namespace

void Submit(const std::string& command) {
    std::scoped_lock lock{mutex};
    Add(Kind::Input, "> " + command);
    if (history.empty() || history.back() != command) {
        history.push_back(command);
        if (history.size() > kMaxHistory) history.erase(history.begin());
    }
    queue.push_back(command);
    std::printf("Console: > %s\n", command.c_str());
    std::fflush(stdout);
}

bool Take(char* out, int size) {
    std::scoped_lock lock{mutex};
    if (queue.empty() || size <= 0) return false;
    std::snprintf(out, std::size_t(size), "%s", queue.front().c_str());
    queue.pop_front();
    return true;
}

void Print(const char* text, Kind kind) {
    if (!text) return;
    std::scoped_lock lock{mutex};
    Add(kind, text);
    std::printf("Console: %s%s\n", kind == Kind::Error ? "error: " : "", text);
    std::fflush(stdout);
}

std::vector<Line> Lines(unsigned* out_version) {
    std::scoped_lock lock{mutex};
    if (out_version) *out_version = version;
    return {lines.begin(), lines.end()};
}

void Clear() {
    std::scoped_lock lock{mutex};
    lines.clear();
    ++version;
}

std::vector<std::string> History() {
    std::scoped_lock lock{mutex};
    return history;
}

} // namespace PortConsole
