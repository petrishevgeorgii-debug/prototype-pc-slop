// SPDX-License-Identifier: GPL-2.0-or-later
// PROTOTYPE: the render map, the first step of a native renderer (docs/RENDERER.md).
//
// Pure3D draws a frame as a tree of passes (pure3d::Pass: RootPass, ProtoZPass, ProtoOpaquePass,
// ProtoShadowPass, the post-processing FrameBufferPasses...), each one's Render (its vtable slot 5)
// drawing its part and its children's. Here every pass class's Render is wrapped: it writes a
// debug marker with the class's name into the game's own command buffer before it (a PM4 NOP,
// the packet sceGnmInsertPushMarker writes) and one that ends it after. The GPU side then knows
// which pass every draw belongs to: gpu/shim/port_render_map.cpp counts them and logs the frame's
// passes in order with their draws, and with THEORDER_VK_MARKERS=1 the passes are named scopes in
// a RenderDoc capture.
//
// The command buffer is the current Gnmx context's (its pointer at 0x1277938, the
// DrawCommandBuffer at its start: end +0x08, write pointer +0x10, the reserve callback +0x18 and
// its user data +0x20, as the game's own draw packets use them). THEORDER_RENDER_MAP=1 turns it on
// (the launcher does in test builds).

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

#include "game/guest.h"
#include "game/native_registry.h"

namespace {

struct PassClass {
    const char* name;
    std::uint64_t vtable;  ///< the class's vtable (slot 0)
    std::uint64_t render;  ///< its Render, slot 5
};

constexpr PassClass kPasses[] = {
    {"FrontEndPass", 0x104ab60, 0xc36f0}, // engine::render::FrontEndPass
    {"LetterboxRenderPass", 0x10c5220, 0x9d0ac0}, // engine::render::LetterboxRenderPass
    {"BloomColourAdjustPass", 0x1060e20, 0x325bf0}, // pure3d::BloomColourAdjustPass
    {"BloomCompositePass", 0x1060e80, 0x3261b0}, // pure3d::BloomCompositePass
    {"BloomPass", 0x1060dc0, 0x325610}, // pure3d::BloomPass
    {"BlurPass", 0x1060f40, 0x3268c0}, // pure3d::BlurPass
    {"CapturePass", 0x1060fc0, 0x328010}, // pure3d::CapturePass
    {"ColourMatrixPass", 0x1061050, 0x329120}, // pure3d::ColourMatrixPass
    {"CopyPass", 0x10610d0, 0x3293c0}, // pure3d::CopyPass
    {"Debug2DPass", 0x1061150, 0x329770}, // pure3d::Debug2DPass
    {"Debug3DPass", 0x10611d0, 0x329ce0}, // pure3d::Debug3DPass
    {"DebugPass", 0x1062e70, 0x3540d0}, // pure3d::DebugPass
    {"DepthOfFieldPass", 0x10612b0, 0x32b880}, // pure3d::DepthOfFieldPass
    {"FastZPass", 0x10618d0, 0x334860}, // pure3d::FastZPass
    {"LetterBoxPass", 0x1062f20, 0x354640}, // pure3d::LetterBoxPass
    {"OldWireframePass", 0x1061770, 0x333760}, // pure3d::OldWireframePass
    {"OpaquePass", 0x10613e0, 0x32eee0}, // pure3d::OpaquePass
    {"OrthoNormalPass", 0x1062fa0, 0x354700}, // pure3d::OrthoNormalPass
    {"PrimColourPass", 0x1063020, 0x3549c0}, // pure3d::PrimColourPass
    {"ProtoAlphaTestZPass", 0x10628f0, 0x349a90}, // pure3d::ProtoAlphaTestZPass
    {"ProtoBloodPass", 0x1061b50, 0x3384c0}, // pure3d::ProtoBloodPass
    {"ProtoBloomColourAdjustPass", 0x1061c30, 0x3397e0}, // pure3d::ProtoBloomColourAdjustPass
    {"ProtoBloomCompositePass", 0x1061c90, 0x339de0}, // pure3d::ProtoBloomCompositePass
    {"ProtoBloomPass", 0x1061bd0, 0x3391a0}, // pure3d::ProtoBloomPass
    {"ProtoCapturePass", 0x1061d50, 0x33a110}, // pure3d::ProtoCapturePass
    {"ProtoColourMatrixPass", 0x1061df0, 0x33b5f0}, // pure3d::ProtoColourMatrixPass
    {"ProtoDecalPass", 0x1061e70, 0x33bab0}, // pure3d::ProtoDecalPass
    {"ProtoFBPass", 0x1061ef0, 0x33ccf0}, // pure3d::ProtoFBPass
    {"ProtoFastZPass", 0x1062890, 0x349300}, // pure3d::ProtoFastZPass
    {"ProtoFogPass", 0x1061ff0, 0x33e2f0}, // pure3d::ProtoFogPass
    {"ProtoHUDPass", 0x1062070, 0x33e8e0}, // pure3d::ProtoHUDPass
    {"ProtoLightDecalPass", 0x10620f0, 0x33ecf0}, // pure3d::ProtoLightDecalPass
    {"ProtoLitRootPass", 0x10622f0, 0x3427e0}, // pure3d::ProtoLitRootPass
    {"ProtoMidgroundPass", 0x1062170, 0x33f300}, // pure3d::ProtoMidgroundPass
    {"ProtoMidgroundZPass", 0x10627d0, 0x348bd0}, // pure3d::ProtoMidgroundZPass
    {"ProtoMotherPass", 0x10621f0, 0x3401e0}, // pure3d::ProtoMotherPass
    {"ProtoOpaquePass", 0x1062270, 0x3405b0}, // pure3d::ProtoOpaquePass
    {"ProtoProperFadeBlurPass", 0x1062c10, 0x350e90}, // pure3d::ProtoProperFadeBlurPass
    {"ProtoProperFadePass", 0x1062c90, 0x351800}, // pure3d::ProtoProperFadePass
    {"ProtoRadialBlurPass", 0x1062470, 0x343e60}, // pure3d::ProtoRadialBlurPass
    {"ProtoSSAOApplyPass", 0x1062d10, 0x352460}, // pure3d::ProtoSSAOApplyPass
    {"ProtoSSAOExtractionPass", 0x1062d90, 0x352a30}, // pure3d::ProtoSSAOExtractionPass
    {"ProtoShadowPass", 0x10624f0, 0x344cd0}, // pure3d::ProtoShadowPass
    {"ProtoSlowZPass", 0x1062830, 0x348f00}, // pure3d::ProtoSlowZPass
    {"ProtoThermalPass", 0x1062570, 0x3468f0}, // pure3d::ProtoThermalPass
    {"ProtoToneMapPass", 0x1061f50, 0x33d580}, // pure3d::ProtoToneMapPass
    {"ProtoTranslucentPass", 0x1062650, 0x347390}, // pure3d::ProtoTranslucentPass
    {"ProtoTranslucentPassBase", 0x10625f0, 0x347390}, // pure3d::ProtoTranslucentPassBase
    {"ProtoWireframePass", 0x10626f0, 0x347b80}, // pure3d::ProtoWireframePass
    {"ProtoZPass", 0x1062770, 0x348560}, // pure3d::ProtoZPass
    {"RootPass", 0x1061460, 0x330380}, // pure3d::RootPass
    {"ShadowDebugPass", 0x10614e0, 0x330550}, // pure3d::ShadowDebugPass
    {"ShadowPass", 0x1061560, 0x330ca0}, // pure3d::ShadowPass
    {"SlowZPass", 0x1061870, 0x334690}, // pure3d::SlowZPass
    {"ToneMapPass", 0x10630a0, 0x3555c0}, // pure3d::ToneMapPass
    {"TranslucentPass", 0x10615e0, 0x332310}, // pure3d::TranslucentPass
    {"ViewPass", 0x1061660, 0x332a00}, // pure3d::ViewPass
    {"WireframeOverlayPass", 0x1061710, 0x333610}, // pure3d::WireframeOverlayPass
    {"ZPass", 0x1061810, 0x333fc0}, // pure3d::ZPass
};
constexpr int kPassCount = int(sizeof(kPasses) / sizeof(kPasses[0]));
std::uint64_t g_original[kPassCount];

constexpr std::uint64_t kContext = 0x1277938; // the current Gnmx context (its DrawCommandBuffer first)
constexpr std::uint32_t kNop = 0x10, kMarkerPush = 0x68750001u, kMarkerPop = 0x68750002u;

struct CommandBuffer {
    std::uint32_t* begin;
    std::uint32_t* end;
    std::uint32_t* write;
    std::uint8_t(GUEST_ABI* reserve)(CommandBuffer*, std::uint32_t, void*); ///< the game's own
    void* user;
};

/// Room for `dwords` more in the context's command buffer, or nullptr.
std::uint32_t* Room(std::uint32_t dwords) {
    auto* buffer = Guest::At<CommandBuffer*>(kContext);
    if (!buffer || !buffer->write || buffer->write < buffer->begin || buffer->write > buffer->end) return nullptr;
    if (std::uint64_t(buffer->end - buffer->write) < dwords) {
        if (!buffer->reserve || !buffer->reserve(buffer, dwords, buffer->user)) return nullptr;
        if (std::uint64_t(buffer->end - buffer->write) < dwords) return nullptr;
    }
    std::uint32_t* at = buffer->write;
    buffer->write += dwords;
    return at;
}

std::uint32_t Nop(std::uint32_t data_dwords) { // a type-3 NOP header for `data_dwords` after it
    return (3u << 30) | ((data_dwords - 1) << 16) | (kNop << 8);
}

/// sceGnmInsertPushMarker's packet: NOP, the push payload, the name (NUL padded).
void Push(const char* name) {
    const std::uint32_t length = std::uint32_t(std::strlen(name));
    const std::uint32_t size = ((length + 8) >> 2) + ((length + 0xc) >> 3) * 2; // its count field
    std::uint32_t* at = Room(size + 2);
    if (!at) return;
    at[0] = Nop(size + 1);
    at[1] = kMarkerPush;
    std::memset(at + 2, 0, size * 4);
    std::memcpy(at + 2, name, length);
}

void Pop() {
    std::uint32_t* at = Room(6);
    if (!at) return;
    at[0] = Nop(5);
    at[1] = kMarkerPop;
    at[2] = at[3] = at[4] = at[5] = 0;
}

template <int I>
GUEST_ABI std::uint64_t Render(std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d, std::uint64_t e,
                               std::uint64_t f) {
    using Original = std::uint64_t(GUEST_ABI*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                               std::uint64_t, std::uint64_t);
    Push(kPasses[I].name);
    const std::uint64_t result = reinterpret_cast<Original>(g_original[I])(a, b, c, d, e, f);
    Pop();
    return result;
}

using Thunk = std::uint64_t(GUEST_ABI*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                        std::uint64_t);

template <int... I>
Thunk ThunkOf(int index, std::integer_sequence<int, I...>) {
    static const Thunk thunks[] = {&Render<I>...};
    return thunks[index];
}

bool InstallRenderMap(std::uint64_t) {
    const char* on = std::getenv("THEORDER_RENDER_MAP");
    if (!on || std::strcmp(on, "1") != 0) {
        std::printf("Render map: off (THEORDER_RENDER_MAP=1 turns it on)\n");
        std::fflush(stdout);
        return true; // nothing to do: not a failure
    }
    const auto image = reinterpret_cast<std::uintptr_t>(Guest::Image());
    int hooked = 0;
    for (int i = 0; i < kPassCount; ++i) {
        auto& slot = Guest::At<std::uintptr_t>(kPasses[i].vtable + 5 * 8);
        if (slot != image + kPasses[i].render) continue; // another build: that class left alone
        g_original[i] = slot;
        slot = reinterpret_cast<std::uintptr_t>(ThunkOf(i, std::make_integer_sequence<int, kPassCount>{}));
        ++hooked;
    }
    std::printf("Render map: on, %d of %d pass classes marked\n", hooked, kPassCount);
    std::fflush(stdout);
    return hooked > 0;
}

// The site checked is the context pointer's user: ps4Context's draw (0x234a82: lea of 0x1277938).
NATIVE_REPLACEMENT({"render map (Pure3D's passes as GPU markers)", "rendermap", Native::Kind::Custom, 0x234a82,
                    {0x48, 0x8d, 0x05, 0xaf, 0x2e, 0x04, 0x01, 0x4c, 0x8b, 0x38}, nullptr, nullptr,
                    &InstallRenderMap});

} // namespace
