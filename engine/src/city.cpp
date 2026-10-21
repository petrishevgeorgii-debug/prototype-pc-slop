// SPDX-License-Identifier: GPL-2.0-or-later
// Manhattan Engine: the city (city.h).

#include "city.h"
#include "log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <array>
#include <cctype>
#include <optional>

namespace engine {

namespace {

constexpr float kLoadRadius = 450.0f;   ///< metres from the camera to a cell's bounds
constexpr float kUnloadRadius = 650.0f;
constexpr std::size_t kMaxCells = 32;

/// "Cell_100" -> 100, else -1.
int CellNumber(const std::string& name) {
    const auto at = name.rfind("Cell_");
    if (at == std::string::npos) return -1;
    try {
        return std::stoi(name.substr(at + 5));
    } catch (...) {
        return -1;
    }
}

} // namespace

struct City::Vehicle {
    std::vector<PropPart> parts; ///< in the vehicle's space: x across, y up, -z forward
    std::vector<std::array<float, 3>> palette;
};

struct City::Pending {
    std::vector<p3d::Vertex> vertices;
    std::vector<std::uint32_t> indices;
    struct Part {
        std::uint32_t first = 0, count = 0;
        std::int32_t offset = 0;
        Look look;
        int group = -1;
    };
    std::vector<Part> parts;
    std::vector<p3d::Texture> textures;
    std::unique_ptr<CollisionMesh> collision;
    std::string error;
};

City::City() = default;

City::~City() {
    Close();
}

namespace {

/// v rotated by the unit quaternion q (x y z w).
void Rotate(const float q[4], const float v[3], float out[3]) {
    const float t[3] = {2 * (q[1] * v[2] - q[2] * v[1]), 2 * (q[2] * v[0] - q[0] * v[2]), 2 * (q[0] * v[1] - q[1] * v[0])};
    out[0] = v[0] + q[3] * t[0] + (q[1] * t[2] - q[2] * t[1]);
    out[1] = v[1] + q[3] * t[1] + (q[2] * t[0] - q[0] * t[2]);
    out[2] = v[2] + q[3] * t[2] + (q[0] * t[1] - q[1] * t[0]);
}

/// How the engine draws a shader; nullopt when it does not draw it (Skip).
std::optional<City::Look> LookOf(const p3d::Shader& s, bool tint) {
    const p3d::Surface surface = p3d::SurfaceOf(s.effect);
    if (surface == p3d::Surface::Skip) return std::nullopt;
    City::Look look;
    look.colour = p3d::ColourTexture(s);
    look.normal = p3d::NormalTexture(s);
    if (p3d::IsPainted(s.effect)) {
        if (auto it = s.textures.find("color_variation_mask"); it != s.textures.end() && !it->second.empty()) {
            look.mask = it->second;
            look.flags |= Material::Paint;
        }
    }
    if (p3d::IsGlass(s.effect)) {
        look.colour.clear();
        look.normal.clear();
        look.flags |= Material::Glass | Material::Decal;
    }
    if (!look.colour.empty()) look.flags |= Material::Textured;
    if (!look.normal.empty()) look.flags |= Material::NormalMap;
    if (surface == p3d::Surface::AlphaTest) look.flags |= Material::AlphaTest;
    if (surface == p3d::Surface::Decal) look.flags |= Material::Decal;
    if (tint && p3d::TintedByVertices(s.effect)) look.flags |= Material::Tint;
    // Cut-out walls and roofs are solid; leaves, branches and flowers are not.
    look.solid = !(look.flags & (Material::Decal | Material::Glass));
    if (look.flags & Material::AlphaTest) {
        std::string name = s.effect + " " + look.colour;
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        for (const char* soft : {"tree", "foliage", "branch", "leaves", "leaf", "grass", "flower", "plant", "hedge", "bush", "ivy", "vine"}) {
            if (name.find(soft) != std::string::npos) look.solid = false;
        }
    }
    return look;
}

} // namespace

void City::LoadLightRig(const std::filesystem::path& file) {
    p3d::File f;
    std::string error;
    if (!p3d::Load(file, f, error)) {
        Log("  light rig: %s (using defaults)\n", error.c_str());
        return;
    }
    std::vector<p3d::Light> lights;
    p3d::ReadLightRig(f, lights, districts_);
    int fills = 0;
    for (const auto& l : lights) {
        const float towards[3] = {-l.direction[0], -l.direction[1], -l.direction[2]};
        if (l.name == "directional_sun") {
            std::copy(towards, towards + 3, lighting_.sun);
            for (int k = 0; k < 3; ++k) lighting_.sun_colour[k] = l.colour[k] * (l.intensity > 0 ? l.intensity : 1.0f);
        } else if (l.name == "ambientLight1") {
            std::copy(l.colour, l.colour + 3, lighting_.ambient);
        } else if (l.name.rfind("fillLight", 0) == 0 && fills < 2) {
            std::copy(towards, towards + 3, lighting_.fill[fills]);
            std::copy(l.colour, l.colour + 3, lighting_.fill_colour[fills]);
            ++fills;
        }
        Log("  %s: towards %.2f %.2f %.2f, colour %.2f %.2f %.2f x %.2f\n", l.name.c_str(), towards[0], towards[1], towards[2],
            l.colour[0], l.colour[1], l.colour[2], l.intensity);
    }
    Log("  %zu lights, %zu districts\n", lights.size(), districts_.size());
}

namespace {

/// FNV-1a: the same choice for the same name on every run and every PC.
std::uint32_t Hash(const std::string& s, std::uint32_t seed = 2166136261u) {
    std::uint32_t h = seed;
    for (unsigned char c : s) h = (h ^ c) * 16777619u;
    return h;
}

/// a * b for row-vector matrices (Pure3D's).
void MultiplyRows(const float a[16], const float b[16], float out[16]) {
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a[r * 4 + k] * b[k * 4 + c];
            out[r * 4 + c] = s;
        }
    }
}

} // namespace

void City::LoadTextures(const p3d::File& f) {
    std::vector<p3d::Texture> textures;
    p3d::ReadTextures(f, textures);
    for (const auto& t : textures) {
        if (shared_.count(t.name)) continue;
        GpuTexture g;
        if (renderer_->CreateTexture(t, g)) {
            shared_[t.name] = g;
            library_.textures.push_back(g);
        }
    }
}

void City::LoadVehicles(const std::filesystem::path& folder) {
    std::error_code ec;
    if (!std::filesystem::is_directory(folder, ec)) {
        Log("  no %s\n", folder.string().c_str());
        return;
    }
    {
        p3d::File shared;
        std::string error;
        if (p3d::Load(folder / "shared.p3d.rz", shared, error)) LoadTextures(shared);
    }
    std::vector<std::string> names;
    for (const auto& d : districts_) {
        for (const auto& [name, weight] : d.vehicles) {
            if (std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
        }
    }
    for (const auto& name : names) {
        p3d::File f;
        std::string error;
        if (!p3d::Load(folder / name / (name + ".p3d.rz"), f, error)) continue;
        LoadTextures(f);
        std::vector<p3d::Shader> shaders;
        std::vector<p3d::Mesh> meshes;
        std::vector<p3d::Skeleton> skeletons;
        std::vector<p3d::Composite> composites;
        p3d::ReadShaders(f, shaders);
        p3d::ReadMeshes(f, meshes);
        p3d::ReadSkeletons(f, skeletons);
        p3d::ReadComposites(f, composites);
        // The whole vehicle (the composite named after it), not its damaged or distant forms.
        const p3d::Composite* composite = nullptr;
        for (const auto& c : composites) {
            if (c.name == name || (!composite && c.name.find("lod") == std::string::npos && c.name.find("crushed") == std::string::npos)) {
                composite = &c;
            }
        }
        const p3d::Skeleton* skeleton = nullptr;
        for (const auto& s : skeletons) {
            if (composite && s.name == composite->skeleton) skeleton = &s;
        }
        if (!composite || !skeleton || skeleton->joints.empty()) continue;
        // Each joint's place in the vehicle: its own, then its parents'.
        std::vector<std::array<float, 16>> world(skeleton->joints.size());
        for (std::size_t j = 0; j < skeleton->joints.size(); ++j) {
            const auto& joint = skeleton->joints[j];
            if (j == 0 || joint.parent >= j) std::copy(joint.matrix, joint.matrix + 16, world[j].begin());
            else MultiplyRows(joint.matrix, world[joint.parent].data(), world[j].data());
        }
        std::unordered_map<std::string, std::optional<Look>> looks;
        for (const auto& s : shaders) looks[s.name] = LookOf(s, false);
        auto vehicle = std::make_unique<Vehicle>();
        for (const auto& prop : composite->props) {
            // Parked: nobody at the wheel; the headlights' glow is not geometry.
            std::string lower = prop.mesh;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            if (lower.find("driver") != std::string::npos || lower.find("shadow") != std::string::npos ||
                lower.find("headlight") != std::string::npos || prop.joint >= world.size()) {
                continue;
            }
            const float* m = world[prop.joint].data();
            for (auto& mesh : meshes) {
                if (mesh.name != prop.mesh) continue;
                for (auto& part : mesh.parts) {
                    PropPart pp;
                    if (auto it = looks.find(part.shader); it != looks.end()) {
                        if (!it->second) continue;
                        pp.look = *it->second;
                    }
                    for (auto v : part.vertices) {
                        const float p[3] = {v.position[0], v.position[1], v.position[2]};
                        const float n[3] = {v.normal[0], v.normal[1], v.normal[2]};
                        const float t[3] = {v.tangent[0], v.tangent[1], v.tangent[2]};
                        for (int k = 0; k < 3; ++k) {
                            v.position[k] = p[0] * m[k] + p[1] * m[4 + k] + p[2] * m[8 + k] + m[12 + k];
                            v.normal[k] = n[0] * m[k] + n[1] * m[4 + k] + n[2] * m[8 + k];
                            v.tangent[k] = t[0] * m[k] + t[1] * m[4 + k] + t[2] * m[8 + k];
                        }
                        pp.vertices.push_back(v);
                    }
                    pp.indices = part.indices;
                    vehicle->parts.push_back(std::move(pp));
                }
            }
        }
        p3d::File tod;
        if (p3d::Load(folder / name / (name + "_tod.p3d.rz"), tod, error)) p3d::ReadPalette(tod, vehicle->palette);
        if (!vehicle->parts.empty()) vehicles_[name] = std::move(vehicle);
    }
    Log("  %zu of the districts' %zu vehicles\n", vehicles_.size(), names.size());
}

const City::Vehicle* City::PickVehicle(const p3d::Placement& place, std::uint32_t& paint) const {
    if (vehicles_.empty()) return nullptr;
    // The district's vehicles, by their weights; parking places are for cars, not buses.
    const p3d::District* district = nullptr;
    float best = 1e30f;
    for (const auto& d : districts_) {
        const float dx = d.position[0] - place.position[0], dz = d.position[2] - place.position[2];
        if (!d.vehicles.empty() && dx * dx + dz * dz < best) best = dx * dx + dz * dz, district = &d;
    }
    const auto parkable = [](const std::string& n) {
        for (const char* big : {"bus", "Bus", "tanker", "transport", "humvee", "ambulance", "police"}) {
            if (n.find(big) != std::string::npos) return false;
        }
        return true;
    };
    std::vector<std::pair<const Vehicle*, float>> choices;
    float total = 0;
    if (district) {
        for (const auto& [name, weight] : district->vehicles) {
            auto it = vehicles_.find(name);
            if (it == vehicles_.end() || !parkable(name)) continue;
            choices.emplace_back(it->second.get(), weight);
            total += weight;
        }
    }
    if (choices.empty()) {
        for (const auto& [name, v] : vehicles_) {
            if (parkable(name)) choices.emplace_back(v.get(), 1.0f), total += 1.0f;
        }
    }
    if (choices.empty()) return nullptr;
    const std::uint32_t h = Hash(place.name);
    float x = float(h % 10007u) / 10007.0f * total;
    const Vehicle* v = choices.back().first;
    for (const auto& [c, w] : choices) {
        if (x < w) {
            v = c;
            break;
        }
        x -= w;
    }
    // The paint: one of its palette's, else a grey.
    std::array<float, 3> rgb = {0.2f, 0.2f, 0.21f};
    if (!v->palette.empty()) rgb = v->palette[Hash(place.name, 0x9e3779b9u) % v->palette.size()];
    paint = 0xff000000u;
    for (int k = 0; k < 3; ++k) paint |= std::uint32_t(std::clamp(rgb[k], 0.0f, 1.0f) * 255.0f + 0.5f) << (8 * k);
    return v;
}

std::string City::DistrictAt(const float p[3]) const {
    const p3d::District* best = nullptr;
    float best_d = 1e30f;
    for (const auto& d : districts_) {
        const float dx = d.position[0] - p[0], dz = d.position[2] - p[2];
        if (dx * dx + dz * dz < best_d) best_d = dx * dx + dz * dz, best = &d;
    }
    return best ? best->name : std::string();
}

void City::LoadProps(const std::filesystem::path& file) {
    p3d::File f;
    std::string error;
    if (!p3d::Load(file, f, error)) {
        Log("  props: %s\n", error.c_str());
        return;
    }
    std::vector<p3d::Texture> textures;
    std::vector<p3d::Shader> shaders;
    std::vector<p3d::Mesh> meshes;
    p3d::ReadTextures(f, textures);
    p3d::ReadShaders(f, shaders);
    p3d::ReadMeshes(f, meshes);
    LoadTextures(f);
    std::unordered_map<std::string, std::optional<Look>> looks;
    for (const auto& s : shaders) looks[s.name] = LookOf(s, true);
    const std::string suffix = "InitialShape";
    for (auto& m : meshes) {
        if (m.name.size() <= suffix.size() || m.name.compare(m.name.size() - suffix.size(), suffix.size(), suffix) != 0) continue;
        auto& parts = props_[m.name.substr(0, m.name.size() - suffix.size())];
        for (auto& part : m.parts) {
            PropPart p;
            if (auto it = looks.find(part.shader); it != looks.end()) {
                if (!it->second) continue; // not a surface
                p.look = *it->second;
            }
            p.vertices = std::move(part.vertices);
            p.indices = std::move(part.indices);
            parts.push_back(std::move(p));
        }
    }
    Log("  %zu props\n", props_.size());
}

std::unique_ptr<City::Pending> City::Build(const std::filesystem::path& file, int group_from_name,
                                           const std::filesystem::path& placements) {
    auto p = std::make_unique<Pending>();
    p3d::File f;
    if (!p3d::Load(file, f, p->error)) return p;
    std::vector<p3d::Mesh> meshes;
    std::vector<p3d::Shader> shaders;
    p3d::ReadMeshes(f, meshes);
    p3d::ReadShaders(f, shaders);
    p3d::ReadTextures(f, p->textures);
    // The low-detail city's vertex colours are not a tint.
    std::unordered_map<std::string, std::optional<Look>> looks;
    for (const auto& s : shaders) looks[s.name] = LookOf(s, group_from_name == 0);
    for (const auto& mesh : meshes) {
        // Props are modelled around their own origin and placed by instances the engine does not
        // read yet: leave out small meshes at the origin.
        const float cx = (mesh.min[0] + mesh.max[0]) / 2, cz = (mesh.min[2] + mesh.max[2]) / 2;
        const float span = std::max({mesh.max[0] - mesh.min[0], mesh.max[1] - mesh.min[1], mesh.max[2] - mesh.min[2]});
        if (std::sqrt(cx * cx + cz * cz) < 60.0f && span < 60.0f) continue;
        const int group = group_from_name ? CellNumber(mesh.name) : -1;
        for (const auto& part : mesh.parts) {
            Pending::Part d;
            if (auto it = looks.find(part.shader); it != looks.end()) {
                if (!it->second) continue; // not a surface
                d.look = *it->second;
            }
            d.first = static_cast<std::uint32_t>(p->indices.size());
            d.count = static_cast<std::uint32_t>(part.indices.size());
            d.offset = static_cast<std::int32_t>(p->vertices.size());
            d.group = group;
            p->vertices.insert(p->vertices.end(), part.vertices.begin(), part.vertices.end());
            p->indices.insert(p->indices.end(), part.indices.begin(), part.indices.end());
            p->parts.push_back(std::move(d));
        }
    }
    // Props where the cell places them (the intact shape, as each object's whole).
    std::error_code ec;
    if (!placements.empty() && std::filesystem::file_size(placements, ec) > 64 && !ec) {
        p3d::File ft;
        std::string error;
        std::vector<p3d::Placement> objects;
        if (p3d::Load(placements, ft, error)) p3d::ReadPlacements(ft, objects);
        for (const auto& o : objects) {
            const std::vector<PropPart>* parts = nullptr;
            std::uint32_t paint = 0;
            if (o.type == "proto::ParkedCarSpawner") {
                const Vehicle* v = PickVehicle(o, paint);
                if (!v) continue;
                parts = &v->parts;
            } else {
                // Collectibles are the game's pick-ups, not the city's props.
                if (o.prop.empty() || o.prop.rfind("Collectible", 0) == 0) continue;
                auto it = props_.find(o.prop);
                if (it == props_.end()) continue;
                parts = &it->second;
            }
            for (const auto& part : *parts) {
                Pending::Part d;
                d.first = static_cast<std::uint32_t>(p->indices.size());
                d.count = static_cast<std::uint32_t>(part.indices.size());
                d.offset = static_cast<std::int32_t>(p->vertices.size());
                d.look = part.look;
                for (p3d::Vertex v : part.vertices) {
                    float r[3];
                    Rotate(o.rotation, v.position, r);
                    for (int k = 0; k < 3; ++k) v.position[k] = r[k] + o.position[k];
                    Rotate(o.rotation, v.normal, r);
                    std::copy(r, r + 3, v.normal);
                    Rotate(o.rotation, v.tangent, r);
                    std::copy(r, r + 3, v.tangent);
                    if (part.look.flags & Material::Paint) v.colour = paint;
                    p->vertices.push_back(v);
                }
                p->indices.insert(p->indices.end(), part.indices.begin(), part.indices.end());
                p->parts.push_back(std::move(d));
            }
        }
    }
    // One draw per material (and group): the parts' indices regrouped, each made absolute.
    std::map<std::pair<int, Look>, std::vector<std::uint32_t>> merged;
    for (const auto& part : p->parts) {
        auto& out = merged[{part.group, part.look}];
        for (std::uint32_t i = 0; i < part.count; ++i) out.push_back(p->indices[part.first + i] + static_cast<std::uint32_t>(part.offset));
    }
    p->indices.clear();
    p->parts.clear();
    for (auto& [key, indices] : merged) {
        Pending::Part d;
        d.group = key.first;
        d.look = key.second;
        d.first = static_cast<std::uint32_t>(p->indices.size());
        d.count = static_cast<std::uint32_t>(indices.size());
        p->indices.insert(p->indices.end(), indices.begin(), indices.end());
        p->parts.push_back(std::move(d));
    }
    // What can be stood on and walked into: the solid surfaces (not glass, decals or leaves).
    p->collision = std::make_unique<CollisionMesh>();
    for (const auto& part : p->parts) {
        if (!part.look.solid) continue;
        p->collision->Add(p->vertices, p->indices.data() + part.first, part.count, part.group);
    }
    p->collision->Build();
    return p;
}

bool City::Upload(Pending& p, Model& model) {
    std::unordered_map<std::string, GpuTexture> own;
    for (const auto& t : p.textures) {
        if (shared_.count(t.name)) continue; // the library has it
        GpuTexture g;
        if (renderer_->CreateTexture(t, g)) {
            own[t.name] = g;
            model.textures.push_back(g);
        }
    }
    const auto texture = [&](const std::string& name) -> const GpuTexture* {
        if (name.empty()) return nullptr;
        if (auto it = own.find(name); it != own.end()) return &it->second;
        if (auto it = shared_.find(name); it != shared_.end()) return &it->second;
        return nullptr;
    };
    std::map<std::tuple<std::string, std::string, std::string>, VkDescriptorSet> materials;
    if (!p.vertices.empty() && !renderer_->CreateModel(p.vertices, p.indices, model)) return false;
    if (&model != &low_ && !p.vertices.empty()) {
        std::fill(model.min, model.min + 3, 1e30f);
        std::fill(model.max, model.max + 3, -1e30f);
        for (const auto& v : p.vertices) {
            for (int k = 0; k < 3; ++k) {
                model.min[k] = std::min(model.min[k], v.position[k]);
                model.max[k] = std::max(model.max[k], v.position[k]);
            }
        }
    }
    for (const auto& part : p.parts) {
        Draw d;
        d.first_index = part.first;
        d.index_count = part.count;
        d.vertex_offset = part.offset;
        d.group = part.group;
        const GpuTexture* colour = texture(part.look.colour);
        const GpuTexture* normal = texture(part.look.normal);
        const GpuTexture* mask = texture(part.look.mask);
        d.flags = part.look.flags;
        if (!colour && !(d.flags & Material::Glass)) d.flags &= ~(Material::Textured | Material::AlphaTest | Material::Decal);
        if (!normal) d.flags &= ~Material::NormalMap;
        if (!mask) d.flags &= ~Material::Paint;
        if (colour || normal || mask) {
            const std::tuple<std::string, std::string, std::string> key{colour ? part.look.colour : "", normal ? part.look.normal : "",
                                                                        mask ? part.look.mask : ""};
            auto it = materials.find(key);
            if (it == materials.end()) {
                const VkDescriptorSet set = renderer_->CreateMaterial(colour, normal, mask);
                if (set) model.materials.push_back(set);
                it = materials.emplace(key, set).first;
            }
            d.material = it->second;
        }
        model.draws.push_back(d);
    }
    return true;
}

bool City::Open(const std::filesystem::path& game, Renderer& renderer, std::string& error) {
    renderer_ = &renderer;
    for (const auto& base : {game / "p1", game}) {
        if (std::filesystem::is_directory(base / "art" / "locations" / "manhattan")) {
            location_ = base / "art" / "locations" / "manhattan";
            break;
        }
    }
    if (location_.empty()) {
        error = "no p1/art/locations/manhattan in " + game.string() + " (choose the folder with eboot.bin)";
        return false;
    }
    // Shared textures first: the low-detail city and the cells use them.
    Log("Loading the city's shared textures ...\n");
    {
        p3d::File f;
        std::vector<p3d::Texture> textures;
        std::vector<std::string> skipped;
        if (p3d::Load(location_ / "textures.p3d.rz", f, error)) {
            p3d::ReadTextures(f, textures, &skipped);
            for (const auto& t : textures) {
                GpuTexture g;
                if (renderer.CreateTexture(t, g)) {
                    shared_[t.name] = g;
                    library_.textures.push_back(g);
                }
            }
            Log("  %zu textures (%zu in layouts not read yet)\n", library_.textures.size(), skipped.size());
        } else {
            Log("  %s\n", error.c_str());
        }
        error.clear();
    }
    Log("Loading the light rig ...\n");
    LoadLightRig(location_ / "manhattan_tod.p3d.rz");
    Log("Loading the vehicles ...\n");
    LoadVehicles(location_.parent_path().parent_path() / "packages" / "vehicles");
    Log("Loading the props ...\n");
    LoadProps(location_ / "props.p3d.rz");
    Log("Loading the low-detail city ...\n");
    auto low = Build(location_ / "midgeo.p3d.rz", 1);
    if (!low->error.empty()) {
        error = low->error;
        return false;
    }
    if (!Upload(*low, low_)) {
        error = "cannot upload the low-detail city";
        return false;
    }
    low_collision_ = std::move(low->collision);
    // The cells' bounds: the low-detail city's mesh for each.
    {
        p3d::File f;
        std::string e;
        std::vector<p3d::Mesh> meshes;
        if (p3d::Load(location_ / "midgeo.p3d.rz", f, e)) p3d::ReadMeshes(f, meshes);
        for (const auto& m : meshes) {
            const int n = CellNumber(m.name);
            if (n < 0) continue;
            Cell& c = cells_[n];
            c.index = n;
            std::copy(m.min, m.min + 3, c.min);
            std::copy(m.max, m.max + 3, c.max);
            c.bounded = true;
        }
    }
    for (auto it = cells_.begin(); it != cells_.end();) {
        auto file = location_ / ("manhattan_Cell_" + std::to_string(it->first) + ".p3d.rz");
        std::error_code ec;
        if (std::filesystem::file_size(file, ec) <= 64 || ec) {
            it = cells_.erase(it); // empty or missing
            continue;
        }
        it->second.file = file;
        max_index_ = std::max(max_index_, it->first);
        ++it;
    }
    Log("City: %zu detailed cells\n", cells_.size());
    worker_ = std::thread([this] { Worker(); });
    return true;
}

void City::Close() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
    if (!renderer_) return;
    for (auto& [n, c] : cells_) renderer_->DestroyModel(c.model), c.collision.reset();
    low_collision_.reset();
    renderer_->DestroyModel(low_);
    renderer_->DestroyModel(library_);
    cells_.clear();
    renderer_ = nullptr;
}

float City::Distance(const Cell& c, const float p[3]) const {
    const float dx = std::max({c.min[0] - p[0], 0.0f, p[0] - c.max[0]});
    const float dz = std::max({c.min[2] - p[2], 0.0f, p[2] - c.max[2]});
    return std::sqrt(dx * dx + dz * dz);
}

void City::Worker() {
    for (;;) {
        int n = -1;
        std::filesystem::path file;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [this] { return stop_ || !queue_.empty(); });
            if (stop_) return;
            // The nearest queued cell first.
            auto best = queue_.begin();
            for (auto it = queue_.begin(); it != queue_.end(); ++it) {
                if (Distance(cells_.at(*it), camera_) < Distance(cells_.at(*best), camera_)) best = it;
            }
            n = *best;
            queue_.erase(best);
            file = cells_.at(n).file;
            ++loading_;
        }
        auto ft = file;
        ft.replace_filename(file.stem().stem().string() + "_ft.p3d.rz");
        auto p = Build(file, 0, ft);
        std::lock_guard lock(mutex_);
        --loading_;
        done_.emplace_back(n, std::move(p));
    }
}

void City::Update(const float camera[3]) {
    std::vector<std::pair<int, std::unique_ptr<Pending>>> finished;
    {
        std::lock_guard lock(mutex_);
        std::copy(camera, camera + 3, camera_);
        // Wanted: the nearest cells within reach.
        std::vector<std::pair<float, int>> near;
        for (auto& [n, c] : cells_) {
            const float d = Distance(c, camera);
            if (d < kLoadRadius) near.emplace_back(d, n);
        }
        std::sort(near.begin(), near.end());
        if (near.size() > kMaxCells) near.resize(kMaxCells);
        for (const auto& [d, n] : near) {
            Cell& c = cells_[n];
            if (c.state == Cell::State::Unloaded) {
                c.state = Cell::State::Queued;
                queue_.push_back(n);
            }
        }
        // Queued cells that are no longer wanted are dropped.
        for (auto it = queue_.begin(); it != queue_.end();) {
            if (Distance(cells_[*it], camera) > kUnloadRadius) {
                cells_[*it].state = Cell::State::Unloaded;
                it = queue_.erase(it);
            } else {
                ++it;
            }
        }
        if (!done_.empty()) {
            finished.push_back(std::move(done_.front()));
            done_.pop_front();
        }
    }
    wake_.notify_one();
    for (auto& [n, p] : finished) {
        Cell& c = cells_[n];
        if (!p->error.empty()) {
            Log("Cell %d: %s\n", n, p->error.c_str());
            c.state = Cell::State::Ready; // drawn as low detail; not retried
            continue;
        }
        if (Distance(c, camera) > kUnloadRadius) {
            c.state = Cell::State::Unloaded;
            continue;
        }
        if (Upload(*p, c.model)) {
            c.state = Cell::State::Ready;
            c.collision = std::move(p->collision);
        } else {
            renderer_->DestroyModel(c.model), c.state = Cell::State::Unloaded;
        }
    }
    // Far cells go.
    for (auto& [n, c] : cells_) {
        if (c.state == Cell::State::Ready && Distance(c, camera) > kUnloadRadius) {
            renderer_->DestroyModel(c.model);
            c.collision.reset();
            c.state = Cell::State::Unloaded;
        }
    }
    low_hidden_.assign(static_cast<std::size_t>(max_index_) + 1, false);
    for (const auto& [n, c] : cells_) {
        if (c.state == Cell::State::Ready && !c.model.draws.empty()) low_hidden_[n] = true;
    }
}

void City::Models(std::vector<const Model*>& models, std::vector<bool>& hidden) const {
    hidden.assign(static_cast<std::size_t>(max_index_) + 1, false);
    models.push_back(&low_);
    for (const auto& [n, c] : cells_) {
        if (c.state == Cell::State::Ready && !c.model.draws.empty()) {
            models.push_back(&c.model);
            hidden[n] = true; // the detailed cell replaces its low-detail mesh
        }
    }
}

void City::StartView(float position[3], float target[3]) const {
    auto it = cells_.find(100);
    if (it == cells_.end() && !cells_.empty()) it = std::next(cells_.begin(), cells_.size() / 2);
    float centre[3] = {0, 0, 0};
    float height = 0;
    if (it != cells_.end()) {
        for (int k = 0; k < 3; ++k) centre[k] = (it->second.min[k] + it->second.max[k]) / 2;
        height = it->second.max[1];
    }
    target[0] = centre[0];
    target[1] = centre[1];
    target[2] = centre[2];
    position[0] = centre[0] - 140.0f;
    position[1] = height + 60.0f;
    position[2] = centre[2] - 160.0f;
}

bool City::Ground(float x, float z, float top, float bottom, float& y) const {
    bool found = false;
    const auto take = [&](const CollisionMesh& m, const std::vector<bool>* skip) {
        float h = 0;
        if (m.Ground(x, z, top, bottom, 0.5f, h, skip) && (!found || h > y)) y = h, found = true;
    };
    for (const auto& [n, c] : cells_) {
        if (c.state == Cell::State::Ready && c.collision && x >= c.collision->min[0] - 1 && x <= c.collision->max[0] + 1 &&
            z >= c.collision->min[2] - 1 && z <= c.collision->max[2] + 1) {
            take(*c.collision, nullptr);
        }
    }
    if (low_collision_) take(*low_collision_, &low_hidden_);
    return found;
}

int City::Push(float centre[3], float radius, float push[3]) const {
    int contacts = 0;
    for (const auto& [n, c] : cells_) {
        if (c.state != Cell::State::Ready || !c.collision) continue;
        const auto& m = *c.collision;
        if (centre[0] + radius < m.min[0] || centre[0] - radius > m.max[0] || centre[2] + radius < m.min[2] ||
            centre[2] - radius > m.max[2]) {
            continue;
        }
        contacts += m.PushSphere(centre, radius, push);
    }
    if (low_collision_) contacts += low_collision_->PushSphere(centre, radius, push, &low_hidden_);
    return contacts;
}

std::string City::Status() const {
    int ready = 0;
    for (const auto& [n, c] : cells_) ready += c.state == Cell::State::Ready;
    std::lock_guard lock(mutex_);
    const int waiting = static_cast<int>(queue_.size() + done_.size()) + loading_.load();
    return std::to_string(ready) + " cells" + (waiting ? ", " + std::to_string(waiting) + " loading" : "");
}

bool City::Idle() const {
    std::lock_guard lock(mutex_);
    return queue_.empty() && done_.empty() && loading_.load() == 0;
}

} // namespace engine
