// SPDX-License-Identifier: GPL-2.0-or-later
// PROTOTYPE's trainer, the port's side (gpu/theordergpu.h; game/p1/trainer.cpp does the rest): the
// requests to open it (F1 in the window, the touchpad held: runtime_pad.c), the switches and the
// one-shot actions its rows choose (the Trainer page of the game's menus, port_game_menu.cpp), and
// the Options press the game layer asks for (handed to the pad with the mouse, window.cpp).
// The switches are not saved: every start begins with them off.

#include <atomic>
#include <chrono>
#include <cstdio>

#include "../theordergpu.h"
#include "port_trainer.h"

namespace PortTrainer {
namespace {

std::atomic<int> requests{0};
std::atomic<unsigned> actions{0};
std::atomic<int> switches[THEORDER_TRAINER_ROWS];
std::atomic<long long> options_until{0}, pause_until{0};

long long NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

} // namespace

void Request() {
    ++requests;
}

int Get(int row) {
    return row >= 0 && row < THEORDER_TRAINER_ROWS ? switches[row].load() : 0;
}

void Set(int row, int index) {
    if (row < 0 || row >= THEORDER_TRAINER_ROWS) return;
    if (row == THEORDER_TRAINER_EP || row == THEORDER_TRAINER_UNLOCK) {
        // One-shot: choosing the second choice does it; the row shows it until the screen is left.
        if (index) actions |= 1u << row;
        switches[row] = index ? 1 : 0;
        return;
    }
    switches[row] = index ? 1 : 0;
}

int TakeRequests() {
    return requests.exchange(0);
}

unsigned TakeActions() {
    const unsigned a = actions.exchange(0);
    if (a) {
        // The one-shot rows go back to their first choice for the next time the screen shows.
        switches[THEORDER_TRAINER_EP] = 0;
        switches[THEORDER_TRAINER_UNLOCK] = 0;
    }
    return a;
}

void PressOptions() {
    options_until = NowMs() + 120; // held a few frames: the game reads it as a press
}

bool OptionsHeld() {
    return NowMs() < options_until.load();
}

void PressPause() {
    pause_until = NowMs() + 120;
}

bool PauseHeld() {
    return NowMs() < pause_until.load();
}

} // namespace PortTrainer

extern "C" void theordergpu_trainer_request(void) {
    PortTrainer::Request();
}
