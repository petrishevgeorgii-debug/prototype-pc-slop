// SPDX-License-Identifier: GPL-2.0-or-later
// Manhattan Engine: PROTOTYPE's Pure3D files (docs/formats/pure3d.md).

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace p3d {

/// A chunk: its id, its own data (after the 12-byte header) and its children. The data points
/// into the File that holds it.
struct Chunk {
    std::uint32_t id = 0;
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
    std::vector<Chunk> children;

    const Chunk* First(std::uint32_t child_id) const;
};

struct File {
    std::vector<std::uint8_t> bytes;
    std::vector<Chunk> chunks; ///< the top-level chunks
};

/// Reads a .p3d or .p3d.rz file. False with `error` set when it cannot be read.
bool Load(const std::filesystem::path& path, File& file, std::string& error);

/// The engine's vertex: what every part of the game's geometry is converted to.
struct Vertex {
    float position[3];
    float uv[2];
    float normal[3];  ///< zero when the game's vertices have none
    std::uint32_t colour; ///< RGBA8, 0xffffffff when none
};
static_assert(sizeof(Vertex) == 36);

/// One PrimGroup: triangles with one shader.
struct Part {
    std::string shader;
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices; ///< triangle list
    float min[3], max[3];
};

struct Mesh {
    std::string name;
    std::vector<Part> parts;
    float min[3], max[3];
};

struct Shader {
    std::string name;   ///< what the parts refer to
    std::string effect; ///< e.g. zCBV2_ao_building_noglass
    std::unordered_map<std::string, std::string> textures; ///< parameter -> texture name
};

enum class TextureFormat { BC1, BC2, BC3 };

/// A texture's levels, each as linear rows of 4 x 4 blocks (untiled).
struct Texture {
    std::string name;
    TextureFormat format = TextureFormat::BC1;
    std::uint32_t width = 0, height = 0;
    std::vector<std::vector<std::uint8_t>> levels;
};

void ReadMeshes(const File& file, std::vector<Mesh>& meshes);
void ReadShaders(const File& file, std::vector<Shader>& shaders);
/// Textures in a layout the engine reads; the others are listed in `skipped` (name: reason).
void ReadTextures(const File& file, std::vector<Texture>& textures, std::vector<std::string>* skipped = nullptr);

/// The texture a shader shows as its surface colour, or "".
std::string ColourTexture(const Shader& shader);

} // namespace p3d
