// SPDX-License-Identifier: GPL-2.0-or-later
// Manhattan Engine: the city (city.h).

#include "city.h"
#include "log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace engine {

namespace {

constexpr float kLoadRadius = 450.0f;   ///< metres from the camera to a cell's bounds
constexpr float kUnloadRadius = 650.0f;
constexpr std::size_t kMaxCells = 48;

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

struct City::Pending {
    std::vector<p3d::Vertex> vertices;
    std::vector<std::uint32_t> indices;
    struct Part {
        std::uint32_t first = 0, count = 0;
        std::int32_t offset = 0;
        std::string texture;
        int group = -1;
    };
    std::vector<Part> parts;
    std::vector<p3d::Texture> textures;
    std::string error;
};

City::City() = default;

City::~City() {
    Close();
}

std::unique_ptr<City::Pending> City::Build(const std::filesystem::path& file, int group_from_name) {
    auto p = std::make_unique<Pending>();
    p3d::File f;
    if (!p3d::Load(file, f, p->error)) return p;
    std::vector<p3d::Mesh> meshes;
    std::vector<p3d::Shader> shaders;
    p3d::ReadMeshes(f, meshes);
    p3d::ReadShaders(f, shaders);
    p3d::ReadTextures(f, p->textures);
    std::unordered_map<std::string, std::string> colour;
    for (const auto& s : shaders) colour[s.name] = p3d::ColourTexture(s);
    for (const auto& mesh : meshes) {
        // Props are modelled around their own origin and placed by instances the engine does not
        // read yet: leave out small meshes at the origin.
        const float cx = (mesh.min[0] + mesh.max[0]) / 2, cz = (mesh.min[2] + mesh.max[2]) / 2;
        const float span = std::max({mesh.max[0] - mesh.min[0], mesh.max[1] - mesh.min[1], mesh.max[2] - mesh.min[2]});
        if (std::sqrt(cx * cx + cz * cz) < 60.0f && span < 60.0f) continue;
        const int group = group_from_name ? CellNumber(mesh.name) : -1;
        for (const auto& part : mesh.parts) {
            Pending::Part d;
            d.first = static_cast<std::uint32_t>(p->indices.size());
            d.count = static_cast<std::uint32_t>(part.indices.size());
            d.offset = static_cast<std::int32_t>(p->vertices.size());
            if (auto it = colour.find(part.shader); it != colour.end()) d.texture = it->second;
            d.group = group;
            p->vertices.insert(p->vertices.end(), part.vertices.begin(), part.vertices.end());
            p->indices.insert(p->indices.end(), part.indices.begin(), part.indices.end());
            p->parts.push_back(std::move(d));
        }
    }
    return p;
}

bool City::Upload(Pending& p, Model& model) {
    std::unordered_map<std::string, VkDescriptorSet> own;
    for (const auto& t : p.textures) {
        if (shared_.count(t.name)) continue; // the library has it
        GpuTexture g;
        if (renderer_->CreateTexture(t, g)) {
            own[t.name] = g.set;
            model.textures.push_back(g);
        }
    }
    if (!p.vertices.empty() && !renderer_->CreateModel(p.vertices, p.indices, model)) return false;
    for (const auto& part : p.parts) {
        Draw d;
        d.first_index = part.first;
        d.index_count = part.count;
        d.vertex_offset = part.offset;
        d.group = part.group;
        if (!part.texture.empty()) {
            if (auto it = own.find(part.texture); it != own.end()) d.texture = it->second;
            else if (auto s = shared_.find(part.texture); s != shared_.end()) d.texture = s->second;
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
                    shared_[t.name] = g.set;
                    library_.textures.push_back(g);
                }
            }
            Log("  %zu textures (%zu in layouts not read yet)\n", library_.textures.size(), skipped.size());
        } else {
            Log("  %s\n", error.c_str());
        }
        error.clear();
    }
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
    for (auto& [n, c] : cells_) renderer_->DestroyModel(c.model);
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
        auto p = Build(file, 0);
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
        if (Upload(*p, c.model)) c.state = Cell::State::Ready;
        else renderer_->DestroyModel(c.model), c.state = Cell::State::Unloaded;
    }
    // Far cells go.
    for (auto& [n, c] : cells_) {
        if (c.state == Cell::State::Ready && Distance(c, camera) > kUnloadRadius) {
            renderer_->DestroyModel(c.model);
            c.state = Cell::State::Unloaded;
        }
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
