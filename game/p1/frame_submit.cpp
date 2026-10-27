// SPDX-License-Identifier: GPL-2.0-or-later
// PROTOTYPE: the shadow maps and the depth pre-pass were drawn twice every frame.
//
// ProtoMidgroundZPass's Render (0x348bd0) ends with the ps4Context's kick (vtable slot 70,
// 0x236810): an end-of-pipe label and a Gnmx submit() of the frame so far, so the GPU starts on
// the shadow maps and the depth pre-pass while the CPU goes on (it waits for the label later, for
// what they tell it). But submit() leaves the context's submission start where it was, at the
// frame's start (only reset() and a full command buffer move it, 0x921101), so the frame's
// submitAndFlip sends the whole frame again from there: the GPU draws the shadow maps and the
// depth pre-pass a second time (the PS4's too). The render map showed it (docs/RENDERER.md): in
// the open city 688 shadow draws and 1181 depth pre-pass draws, twice, in a frame of 5098.
//
// Here the kick moves the submission start to where it ended, as Gnmx does when a command buffer
// fills up: the final submit sends the rest of the frame only. The draw command buffer's start is
// at +0x108 (its write pointer +0x10), the other two buffers' at +0x110 (+0x50) and +0x118
// (+0x90), the ranges kept from full buffers counted at +0x2b8 (submitted by the kick too).
// THEORDER_NATIVE=-framesubmit leaves it out.

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "game/guest.h"
#include "game/native_registry.h"

namespace {

constexpr std::uint64_t kKick = 0x236810;               // pure3d::ps4Context, vtable slot 70
constexpr std::uint64_t kKickSlot = 0x105bc70 + 70 * 8; // its vtable (functions from 0x105bc70)
constexpr std::uint64_t kContext = 0x1277938;           // the current Gnmx context
constexpr std::uint64_t kKickLabel = 0x1277b30;         // the kick's label: 0 when its submit failed

unsigned g_kicks = 0;

GUEST_ABI void Kick(void* self, std::uint64_t flags) {
    Guest::Call<void>(kKick, self, flags);
    auto* context = Guest::At<std::uint8_t*>(kContext);
    if (!context || !Guest::At<std::uint64_t>(kKickLabel)) return; // nothing was submitted
    Guest::Field<std::uint64_t>(context, 0x108) = Guest::Field<std::uint64_t>(context, 0x10);
    Guest::Field<std::uint64_t>(context, 0x110) = Guest::Field<std::uint64_t>(context, 0x50);
    Guest::Field<std::uint64_t>(context, 0x118) = Guest::Field<std::uint64_t>(context, 0x90);
    Guest::Field<std::uint32_t>(context, 0x2b8) = 0;
    if (++g_kicks == 1) {
        std::printf("Frame submit: the first kick sent; the frame's last submit starts after it\n");
        std::fflush(stdout);
    }
}

bool InstallFrameSubmit(std::uint64_t) {
    auto& slot = Guest::At<std::uintptr_t>(kKickSlot);
    if (slot != reinterpret_cast<std::uintptr_t>(Guest::Image()) + kKick) return false;
    // Gnmx's submit() (0x91d790) as read: the draw buffer's range from +0x108 to the write pointer.
    if (std::memcmp(Guest::Image() + 0x91d920, "\x41\x8b\x57\x10\x49\x8b\xb7\x08\x01\x00\x00", 11) != 0)
        return false;
    slot = reinterpret_cast<std::uintptr_t>(&Kick);
    return true;
}

NATIVE_REPLACEMENT({"the frame's last submit without the kicked part (shadows and depth pre-pass once)",
                    "framesubmit", Native::Kind::Custom, kKick,
                    {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x83, 0xec, 0x10, 0x48, 0x8d, 0x05, 0x16,
                     0x11, 0x04, 0x01},
                    nullptr, nullptr, &InstallFrameSubmit});

} // namespace
