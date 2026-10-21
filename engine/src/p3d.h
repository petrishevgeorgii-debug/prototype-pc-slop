// SPDX-License-Identifier: GPL-2.0-or-later
// Manhattan Engine: PROTOTYPE's Pure3D files (docs/formats/pure3d.md).

#pragma once

#include <array>
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
    float uv1[2];       ///< the second set (grime, light maps); zero when none
    float normal[3];    ///< zero when the game's vertices have none
    float tangent[4];   ///< xyz and the bitangent's sign; zero when none
    std::uint32_t colour;  ///< colour0, RGBA8: the tint and (alpha) the grime; 0xffffffff when none
    std::uint32_t colour1; ///< colour1, RGBA8: light baked into the vertices; 0 when none
};
static_assert(sizeof(Vertex) == 64);

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
/// Its normal map, or "".
std::string NormalTexture(const Shader& shader);

/// How the engine draws a shader's effect, from the effect's name.
enum class Surface {
    Opaque,
    AlphaTest, ///< cut out by the colour texture's alpha (foliage, fences, signs)
    Decal,     ///< blended over what is under it
    Skip,      ///< drawn by systems the engine does not have yet (light cones, particles, HUD)
};
Surface SurfaceOf(const std::string& effect);
/// Glass (vehicles' windows): dark, reflecting the sky, see-through.
bool IsGlass(const std::string& effect);
/// The effect paints the surface (vehicles: env_vehicle_colorvar), where its
/// color_variation_mask is white.
bool IsPainted(const std::string& effect);
/// The effect colours the surface by the vertices' colour0 (the city's "CBV" effects).
bool TintedByVertices(const std::string& effect);

/// An object the city places (the cells' _ft files): its type (e.g. proto::CloudSpawner), name,
/// position and rotation (quaternion x y z w); for a CloudSpawner, the prop it places.
struct Placement {
    std::string type, name, prop;
    float position[3];
    float rotation[4];
};
void ReadPlacements(const File& file, std::vector<Placement>& placements);

/// A light of the city's light rig (manhattan_tod: ambientLight1, directional_sun, fillLight1,
/// fillLight2, shadowLight).
struct Light {
    std::string name;
    float direction[3]; ///< where the light goes (unit), world space
    float colour[3];
    float intensity;    ///< the sun's multiplier (1.1); 0 for the others
};
/// A district of the city (proto::DistrictBehaviour): its name and centre.
struct District {
    std::string name;
    float position[3];
    std::vector<std::pair<std::string, float>> vehicles; ///< what it spawns, with weights
};
void ReadLightRig(const File& file, std::vector<Light>& lights, std::vector<District>& districts);

/// A skeleton: its joints, each placed relative to its parent (the first is the root).
struct Joint {
    std::string name;
    std::uint32_t parent = 0;
    float matrix[16]; ///< row vectors: rotation rows, then the translation (12..14)
};
struct Skeleton {
    std::string name;
    std::vector<Joint> joints;
};
void ReadSkeletons(const File& file, std::vector<Skeleton>& skeletons);

/// A model made of meshes, each on a joint of a skeleton (vehicles: body, doors, wheels).
struct Composite {
    std::string name, skeleton;
    struct Prop {
        std::string mesh;
        std::uint32_t joint = 0;
    };
    std::vector<Prop> props;
};
void ReadComposites(const File& file, std::vector<Composite>& composites);

/// A vehicle's paint colours (its _tod file's proto::ShaderPalette, color_variation).
void ReadPalette(const File& file, std::vector<std::array<float, 3>>& colours);

} // namespace p3d
