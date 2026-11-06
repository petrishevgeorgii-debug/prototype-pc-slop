// SPDX-License-Identifier: GPL-2.0-or-later
// PROTOTYPE's trainer, the port's side (port_trainer.cpp).
#pragma once

namespace PortTrainer {
void Request();
int Get(int row);
void Set(int row, int index);
int TakeRequests();
/// F3: the game's Hide HUD switch (game/p1/hud.cpp).
void HudRequest();
int TakeHudRequests();
unsigned TakeActions();
void PressOptions();
bool OptionsHeld();
void PressPause();
bool PauseHeld();
} // namespace PortTrainer
