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
#include <filesystem>
#include <fstream>
#include <iterator>

#include "game/guest.h"
#include "game/native_registry.h"
#include "game/p1/save_menu.h"

namespace {

constexpr std::uint64_t kDialog = 0x126dea0; ///< engine::StorageManager's dialog (0x98 bytes)
constexpr std::uint64_t kButtons = 0xe9080, kShow = 0xe8320, kShown = 0xe8350;
constexpr std::uint64_t kInvoke = 0x4055e0; ///< GFxMovie::Invoke(method, result, args, count)
constexpr std::int32_t kPresetOkCancel = 2, kOk = 1;

struct Date {
    std::uint16_t year;
    std::uint8_t month, day, hour, minute;
};

TheOrderSaveMenu g_menu;
// The screen's names and details are wide strings (GFx StringW; the PS4's wchar_t is 16-bit).
const char16_t* g_names[33];
const char16_t* g_details[33];
char16_t g_name_text[33][160], g_detail_text[33][160];
const Date* g_dates_of[33];
Date g_dates[33];
std::int32_t g_icons[33];
char g_new_name[160];
bool g_active = false;
int g_frames = 0, g_gone = 0;
bool g_seen = false;

/// UTF-8 into the screen's 16-bit text.
const char16_t* Wide(const char* text, char16_t* out, std::size_t size) {
    std::size_t n = 0;
    const auto* p = reinterpret_cast<const unsigned char*>(text ? text : "");
    while (*p && n + 2 < size) {
        std::uint32_t c = *p++;
        int more = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
        if (more) c &= 0x3f >> more;
        for (; more && (*p & 0xc0) == 0x80; --more) c = (c << 6) | (*p++ & 0x3f);
        if (more || (c >= 0x80 && c < 0xa0)) c = u'?';
        if (c >= 0x10000) {
            c -= 0x10000;
            out[n++] = char16_t(0xd800 + (c >> 10));
            out[n++] = char16_t(0xdc00 + (c & 0x3ff));
        } else {
            out[n++] = char16_t(c);
        }
    }
    out[n] = 0;
    return out;
}

// The save pictures (protolistsaveimages.gfx, slotImage): frame 1 is the [PROTOTYPE] card, then
// one per mission, labelled with the mission's id. The PC set a row's icon to its mission's frame.
constexpr const char* kPictures[] = {
    "none",   "e01m01", "e01m02", "e02m01", "e02m02", "e02m05", "e03m01", "e03m02", "e03m03",
    "e04m01", "e04m02", "e04m03", "e04m04", "e04m05", "e05m01", "e05m02", "e05m03", "e06m01",
    "e06m02", "e06m03", "e06m04", "e06m05", "e06m06", "e07m01", "e07m02", "e07m04", "e08m02",
    "e08m03", "e08m04", "e09m01", "e09m03", "e10m04", "e11m01"};
constexpr long kMissionAt = 0x524; ///< the save's mission id ("e03m02") in the game's save file

/// The picture frame of a save's mission, read from the save the game wrote; 1 (the card) if unknown.
int Picture(const char* folder) {
    if (!folder || !*folder) return 1;
    std::error_code error;
    const std::filesystem::path path{std::u8string(reinterpret_cast<const char8_t*>(folder))};
    for (const auto& entry : std::filesystem::directory_iterator(path, error)) {
        if (!entry.is_regular_file(error)) continue;
        std::ifstream file(entry.path(), std::ios::binary);
        char id[7] = {};
        if (!file.seekg(kMissionAt) || !file.read(id, 6)) continue;
        for (int frame = 1; frame < int(std::size(kPictures)); ++frame)
            if (std::strcmp(id, kPictures[frame]) == 0) return frame + 1;
    }
    return 1;
}

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

struct GfxValue {
    std::uint32_t type, pad;
    union {
        double number;
        const char* string;
        std::uint64_t raw;
    };
};

/// The game's text for `key` (protomain's gLoc.Loc, as storage.gfx asks it), or nullptr.
bool Localized(void* movie, const char* key, char* out, std::size_t size) {
    if (!movie) return false;
    GfxValue arg{};
    arg.type = 4;
    arg.string = key;
    GfxValue result{};
    if (!Guest::Call<std::uint8_t>(kInvoke, movie, "_global.gLoc.Loc", &result, &arg, 1u)) return false;
    if (result.type != 4 || !result.string) return false;
    std::snprintf(out, size, "%s", result.string);
    return true;
}

bool Russian() {
    const char* language = std::getenv("THEORDER_LANGUAGE");
    return language && std::strcmp(language, "8") == 0;
}

void Show(std::uint8_t* dialog, void* movie) {
    const int rows = g_menu.count + (g_menu.has_new ? 1 : 0);
    for (int i = 0; i < g_menu.count; ++i) {
        const auto& r = g_menu.rows[i];
        g_names[i] = Wide(r.name, g_name_text[i], std::size(g_name_text[i]));
        g_details[i] = Wide(r.detail, g_detail_text[i], std::size(g_detail_text[i]));
        g_dates[i] = r.hour < 0 ? Date{0, 0, 0, 0xff, 0}
                                : Date{std::uint16_t(r.year), std::uint8_t(r.month), std::uint8_t(r.day),
                                       std::uint8_t(r.hour), std::uint8_t(r.minute)};
        g_dates_of[i] = &g_dates[i];
        g_icons[i] = Picture(r.folder);
    }
    if (g_menu.has_new) {
        // storage.gfx takes the last row for the new slot when its name is the game's text for
        // $NewDataSlot (and puts it in its place).
        if (!Localized(movie, "$NewDataSlot", g_new_name, sizeof(g_new_name)))
            std::snprintf(g_new_name, sizeof(g_new_name), "%s", Russian() ? "Создать новую ячейку" : "Create new slot");
        std::printf("Save menu: the new slot is \"%s\"\n", g_new_name);
        g_names[g_menu.count] = Wide(g_new_name, g_name_text[g_menu.count], std::size(g_name_text[g_menu.count]));
        g_details[g_menu.count] = Wide("", g_detail_text[g_menu.count], std::size(g_detail_text[g_menu.count]));
        g_dates[g_menu.count] = Date{0, 0, 0, 0xff, 0};
        g_dates_of[g_menu.count] = &g_dates[g_menu.count];
        g_icons[g_menu.count] = 0;
    }
    // storage.gfx shows the slots only when the message is one of these keys (isSaveLoad).
    const bool load = g_menu.mode == 2;
    Guest::Field<const char*>(dialog, 0x10) = load ? "$LOAD_GAME" : "$SAVE";
    Guest::Field<const char*>(dialog, 0x18) = load ? "$SelectLoadEntry" : "$SelectSaveEntry";
    Guest::Field<std::int32_t>(dialog, 0x20) = kPresetOkCancel;
    Guest::Field<const char*>(dialog, 0x28) = nullptr;
    Guest::Field<std::int32_t*>(dialog, 0x30) = g_icons;
    Guest::Field<const char16_t**>(dialog, 0x38) = g_details;
    Guest::Field<const char16_t**>(dialog, 0x40) = g_names;
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

void SaveMenuFrame(void* movie) {
    const TheOrderGameMenu* menu = Native::Menu();
    if (!menu || !menu->save_menu_ready || !menu->save_menu_take || !menu->save_menu_done || !Ready()) return;
    auto* dialog = Guest::At<std::uint8_t*>(kDialog);
    if (!dialog) return;
    menu->save_menu_ready();
    if (!g_active) {
        if (!menu->save_menu_take(&g_menu)) return;
        Show(dialog, movie);
        g_active = true;
        g_frames = g_gone = 0;
        g_seen = false;
        return;
    }
    ++g_frames;
    const std::int32_t button = Guest::Field<std::int32_t>(dialog, 0x58);
    if (!button) {
        // Closed without a button (or never came up): the port's own list takes over as a cancel.
        if (Guest::Call<std::uint8_t>(kShown, dialog)) {
            g_seen = true;
            g_gone = 0;
            return;
        }
        if (++g_gone < (g_seen ? 15 : 120)) return;
        g_active = false;
        std::printf("Save menu: the screen %s without a choice; cancelled\n", g_seen ? "closed" : "never came up");
        std::fflush(stdout);
        menu->save_menu_done(-2);
        return;
    }
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
