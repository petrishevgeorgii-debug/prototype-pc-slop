// SPDX-License-Identifier: GPL-2.0-or-later
// port: graphics modding with the game's own shaders (docs/SHADER_MODS.md).
//
// Shader mods: the game's shaders are recompiled to SPIR-V by the port; a mod replaces the
// SPIR-V of one shader (all of its versions, or one) with its own, compiled from GLSL the port
// exports for editing. Mods live in mods/shaders (and mods/<name>/shaders), are compiled with
// glslangValidator when they change and are applied when that shader is next built: at the
// next start (pipelines already made keep the original until then).
//
// The shader browser (in-game menu, SHADERS): the pixel shaders the last frame drew, by draw
// count; one can be hidden (its draws are skipped) to see what it draws, and exported.

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "common/types.h"

namespace PortShaderMods {

/// The SPIR-V a mod gives version `perm` of shader `hash`, or nothing (the original is used).
std::optional<std::vector<u32>> Find(u64 hash, size_t perm);

/// Shader mods found at start (after the first Find).
size_t Loaded();

// ---- shader browser (GPU thread: NoteDraw, OnFrame; menu: the rest)
void NoteDraw(u64 ps_hash);
void OnFrame();

struct ShaderUse {
    u64 hash;
    u32 draws;
    bool hidden;
    bool modded;
};
/// A short name of a pixel shader the port knows in PROTOTYPE (the browser shows it next to the
/// ID), English and Russian; nullptr for the others.
const char* KnownName(u64 ps_hash, bool russian);

/// The last frame's pixel shaders, most draws first.
std::vector<ShaderUse> LastFrame();

/// Draws of hidden pixel shaders are skipped. AnyHidden is a cheap check for every draw.
bool AnyHidden();
bool IsHidden(u64 ps_hash);
void SetHidden(u64 ps_hash, bool hidden);
void ShowAll();

/// Copies the shader's versions from the shader cache into mods/shaders/_originals and
/// decompiles each to editable GLSL in mods/shaders (in the background; Status() tells).
void Export(u64 hash);

/// The last message of the shader mods (loading, export), for the menu.
std::string Status();
/// The mods folder, for the menu.
std::string Folder();

} // namespace PortShaderMods
