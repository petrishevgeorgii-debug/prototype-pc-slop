// SPDX-License-Identifier: GPL-2.0-or-later
// PROTOTYPE's trainer, the port's side (port_trainer.cpp).
#pragma once

namespace PortTrainer {
void Request();
int Get(int row);
void Set(int row, int index);
int TakeRequests();
unsigned TakeActions();
void PressOptions();
bool OptionsHeld();
} // namespace PortTrainer
