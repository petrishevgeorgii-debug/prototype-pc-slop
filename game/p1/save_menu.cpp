// SPDX-License-Identifier: GPL-2.0-or-later
// PROTOTYPE: the save lists on the game's own save screen, as on PC (docs/NATIVE.md, "The PC save
// menu"). The PS4 game asks the system's save dialog for a list of saves (the port answers it,
// src/runtime_services.c); the PC version showed its own screen instead, storage.gfx, through
// engine::StorageManager's dialog, which the PS4 game still makes but never shows. When a list is
// asked for, it is given to that dialog (its fields: the rows' names, details, dates, the OK and
// Cancel buttons) and the overlay is put up as the PC version did (0xe9080, then 0xe8320); the
// row and button chosen (+0x5c, +0x58, written by the screen's SetSelection / SetButton) go back
// to the system dialog. A new save is the last row ("Create new slot").

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "game/guest.h"
#include "game/native_registry.h"
#include "game/p1/save_menu.h"

namespace {

constexpr std::uint64_t kDialog = 0x126dea0; ///< engine::StorageManager's dialog (0x98 bytes)
constexpr std::uint64_t kButtons = 0xe9080, kShow = 0xe8320, kShown = 0xe8350;
constexpr std::int32_t kPresetOkCancel = 2, kOk = 1;

struct Date {
    std::uint16_t year;
    std::uint8_t month, day, hour, minute;
};

TheOrderSaveMenu g_menu;
const char* g_names[33];
const char* g_details[33];
const Date* g_dates_of[33];
Date g_dates[33];
std::int32_t g_icons[33];
char g_new_name[64];
bool g_active = false;
int g_frames = 0;

bool Matches(std::uint64_t at, std::initializer_list<std::uint8_t> bytes) {
    return std::memcmp(Guest::Image() + at, std::data(bytes), bytes.size()) == 0;
}

bool Ready() {
    static const int ready = [] {
        const bool ok = Matches(kButtons, {0x8b, 0x47, 0x20, 0x48, 0x83, 0xf8, 0x09, 0x0f, 0x87, 0xa0}) &&
                        Matches(kShow, {0x48, 0x8b, 0x7f, 0x08, 0xe9, 0xd7, 0x01, 0x00, 0x00}) &&
                        Matches(kShown, {0x48, 0x8b, 0x7f, 0x08, 0xe9, 0x87, 0x03, 0x00, 0x00});
        std::printf("Save menu: %s\n", ok ? "the PC save screen is there" : "off (the game's code is not where it was)");
        std::fflush(stdout);
        return ok ? 1 : 0;
    }();
    return ready != 0;
}

bool Russian() {
    const char* language = std::getenv("THEORDER_LANGUAGE");
    return language && std::strcmp(language, "8") == 0;
}

void Show(std::uint8_t* dialog) {
    const int rows = g_menu.count + (g_menu.has_new ? 1 : 0);
    for (int i = 0; i < g_menu.count; ++i) {
        const auto& r = g_menu.rows[i];
        g_names[i] = r.name;
        g_details[i] = r.detail;
        g_dates[i] = r.hour < 0 ? Date{0, 0, 0, 0xff, 0}
                                : Date{std::uint16_t(r.year), std::uint8_t(r.month), std::uint8_t(r.day),
                                       std::uint8_t(r.hour), std::uint8_t(r.minute)};
        g_dates_of[i] = &g_dates[i];
        g_icons[i] = 1;
    }
    if (g_menu.has_new) {
        std::snprintf(g_new_name, sizeof(g_new_name), "%s", Russian() ? "Создать новую ячейку" : "Create new slot");
        g_names[g_menu.count] = g_new_name;
        g_details[g_menu.count] = "";
        g_dates[g_menu.count] = Date{0, 0, 0, 0xff, 0};
        g_dates_of[g_menu.count] = &g_dates[g_menu.count];
        g_icons[g_menu.count] = 0;
    }
    static const char* const kTitles[] = {"", "$SelectSaveEntry", "$SelectLoadEntry"};
    Guest::Field<const char*>(dialog, 0x10) = kTitles[g_menu.mode == 2 ? 2 : 1];
    Guest::Field<const char*>(dialog, 0x18) = "";
    Guest::Field<std::int32_t>(dialog, 0x20) = kPresetOkCancel;
    Guest::Field<const char*>(dialog, 0x28) = nullptr;
    Guest::Field<std::int32_t*>(dialog, 0x30) = g_icons;
    Guest::Field<const char**>(dialog, 0x38) = g_details;
    Guest::Field<const char**>(dialog, 0x40) = g_names;
    Guest::Field<const Date**>(dialog, 0x48) = g_dates_of;
    Guest::Field<std::uint32_t>(dialog, 0x50) = std::uint32_t(rows);
    Guest::Field<std::uint32_t>(dialog, 0x54) = 0;
    Guest::Field<std::int32_t>(dialog, 0x58) = 0;
    Guest::Field<std::int32_t>(dialog, 0x5c) = 0;
    Guest::Call<void>(kButtons, dialog);
    Guest::Call<void>(kShow, dialog, -1, 0);
    std::printf("Save menu: the PC save screen up with %d rows\n", rows);
    std::fflush(stdout);
}

} // namespace

namespace Native {

void SaveMenuFrame() {
    const TheOrderGameMenu* menu = Native::Menu();
    if (!menu || !menu->save_menu_ready || !menu->save_menu_take || !menu->save_menu_done || !Ready()) return;
    auto* dialog = Guest::At<std::uint8_t*>(kDialog);
    if (!dialog) return;
    menu->save_menu_ready();
    if (!g_active) {
        if (!menu->save_menu_take(&g_menu)) return;
        Show(dialog);
        g_active = true;
        g_frames = 0;
        return;
    }
    ++g_frames;
    const std::int32_t button = Guest::Field<std::int32_t>(dialog, 0x58);
    if (!button) return;
    if (Guest::Call<std::uint8_t>(kShown, dialog) && g_frames < 600) return; // still closing
    g_active = false;
    const int row = Guest::Field<std::int32_t>(dialog, 0x5c);
    int choice = -2;
    if (button == kOk && row >= 0 && row <= g_menu.count) {
        if (row == g_menu.count) {
            choice = g_menu.has_new ? 0 : -2; // the new save: the dialog's first item
        } else {
            choice = row + (g_menu.has_new ? 1 : 0);
        }
    }
    std::printf("Save menu: button %d, row %d\n", button, row);
    std::fflush(stdout);
    menu->save_menu_done(choice);
}

} // namespace Native
