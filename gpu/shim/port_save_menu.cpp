// SPDX-License-Identifier: GPL-2.0-or-later
// PROTOTYPE: the save list on the game's own save screen, the port's side (port_save_menu.h).

#include "port_save_menu.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace PortSaveMenu {
namespace {

std::mutex mutex;
std::atomic<bool> ready{false};
enum class State { Idle, Waiting, Shown, Finished };
State state = State::Idle;
TheOrderSaveMenu pending{};
int result = -2;
std::chrono::steady_clock::time_point asked;

/// "Title\nSubtitle\nYYYY-MM-DD HH:MM" (runtime_services.c's save_label) into a row.
void Row(const std::string& item, TheOrderSaveRow& row) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start <= item.size()) {
        std::size_t end = item.find('\n', start);
        if (end == std::string::npos) end = item.size();
        lines.push_back(item.substr(start, end - start));
        start = end + 1;
    }
    row = {};
    row.hour = -1;
    std::snprintf(row.name, sizeof(row.name), "%s", lines.empty() ? "" : lines[0].c_str());
    const std::string& last = lines.back();
    int y, mo, d, h, mi;
    if (lines.size() >= 2 && std::sscanf(last.c_str(), "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) == 5) {
        row.year = y, row.month = mo, row.day = d, row.hour = h, row.minute = mi;
        if (lines.size() >= 3) std::snprintf(row.detail, sizeof(row.detail), "%s", lines[1].c_str());
    } else if (lines.size() >= 2) {
        std::snprintf(row.detail, sizeof(row.detail), "%s", lines[1].c_str());
    }
}

} // namespace

bool Begin(const std::string& title, const std::vector<std::string>& items) {
    if (!ready) return false;
    const int mode = title == "Save game" ? 1 : title == "Load game" ? 2 : 0;
    if (!mode) return false; // deleting: the overlay's list
    std::scoped_lock lock{mutex};
    pending = {};
    pending.mode = mode;
    std::size_t first = 0;
    if (!items.empty() && items[0] == "New save") {
        pending.has_new = 1;
        first = 1;
    }
    for (std::size_t i = first; i < items.size() && pending.count < 32; ++i) Row(items[i], pending.rows[pending.count++]);
    state = State::Waiting;
    result = -2;
    asked = std::chrono::steady_clock::now();
    std::printf("Save menu: %s list of %d saves for the game's own screen\n", mode == 1 ? "a save" : "a load",
                pending.count);
    std::fflush(stdout);
    return true;
}

int Poll() {
    std::scoped_lock lock{mutex};
    switch (state) {
    case State::Waiting:
        if (std::chrono::steady_clock::now() - asked > std::chrono::seconds(1)) {
            state = State::Idle;
            std::printf("Save menu: the game's screen did not take the list; the overlay's shows it\n");
            std::fflush(stdout);
            return -3;
        }
        return -1;
    case State::Shown: return -1;
    case State::Finished: state = State::Idle; return result;
    default: return -2;
    }
}

bool Active() {
    std::scoped_lock lock{mutex};
    return state != State::Idle;
}

void Ready() {
    if (!ready.exchange(true)) {
        std::printf("Save menu: the game's own save screen shows the save lists\n");
        std::fflush(stdout);
    }
}

int Take(TheOrderSaveMenu* out) {
    std::scoped_lock lock{mutex};
    if (state != State::Waiting || !out) return 0;
    *out = pending;
    state = State::Shown;
    return 1;
}

void Done(int choice) {
    std::scoped_lock lock{mutex};
    if (state != State::Shown) return;
    result = choice;
    state = State::Finished;
    std::printf("Save menu: %s\n", choice == -2 ? "cancelled" : "a save chosen");
    std::fflush(stdout);
}

} // namespace PortSaveMenu
