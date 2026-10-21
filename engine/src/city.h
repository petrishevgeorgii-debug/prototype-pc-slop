// SPDX-License-Identifier: GPL-2.0-or-later
// Manhattan Engine: the city - the low-detail whole city always, the detailed cells streamed in
// around the camera on a worker thread (docs/ENGINE.md).

#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "collision.h"
#include "p3d.h"
#include "renderer.h"

namespace engine {

class City {
public:
    /// What a part is drawn with: texture names and Material flags.
    struct Look {
        std::string colour, normal, mask;
        std::uint32_t flags = 0;
        bool solid = true; ///< walked on and into (not glass, decals, leaves)
        bool operator<(const Look& o) const {
            return std::tie(colour, normal, mask, flags, solid) < std::tie(o.colour, o.normal, o.mask, o.flags, o.solid);
        }
    };

    City();
    ~City();
    /// `game`: the folder with eboot.bin (or its p1 folder). Loads the shared textures and the
    /// low-detail city, then starts the streaming thread.
    bool Open(const std::filesystem::path& game, Renderer& renderer, std::string& error);
    void Close();

    /// Streams cells around `camera` (x, y, z) and uploads at most one finished cell.
    void Update(const float camera[3]);
    void Models(std::vector<const Model*>& models, std::vector<bool>& hidden) const;

    /// Where to start: above Columbia University (cell 100), looking at it.
    void StartView(float position[3], float target[3]) const;
    /// "12 cells, 2 loading".
    std::string Status() const;
    /// The city's light rig (manhattan_tod), or the defaults when it cannot be read.
    const engine::Lighting& Lighting() const { return lighting_; }
    /// The city's districts (Times Square, Central Park...), and the one nearest to a point.
    const std::vector<p3d::District>& Districts() const { return districts_; }
    std::string DistrictAt(const float position[3]) const;

    /// Collision with what is loaded: the detailed cells, else the low-detail city.
    /// The highest surface to stand on at (x, z) between `top` and `bottom`.
    bool Ground(float x, float z, float top, float bottom, float& y) const;
    /// Moves a sphere out of the city; adds the push's normals to `push`; the triangles touched.
    int Push(float centre[3], float radius, float push[3]) const;
    /// No cell is waiting to be loaded or uploaded.
    bool Idle() const;

private:
    struct Cell {
        int index = -1;
        std::filesystem::path file;
        float min[3] = {0, 0, 0}, max[3] = {0, 0, 0};
        bool bounded = false;
        enum class State { Unloaded, Queued, Loading, Ready } state = State::Unloaded;
        Model model;
        std::unique_ptr<CollisionMesh> collision;
    };
    struct Pending; // CPU data of a loaded cell, waiting for upload

    struct PropPart {
        std::vector<p3d::Vertex> vertices;
        std::vector<std::uint32_t> indices;
        Look look;
    };

    void Worker();
    /// A cell's (or the low-detail city's) geometry; `placements`: the cell's _ft file, whose
    /// props are added where it places them.
    std::unique_ptr<Pending> Build(const std::filesystem::path& file, int group_from_name,
                                   const std::filesystem::path& placements = {});
    void LoadProps(const std::filesystem::path& file);
    void LoadLightRig(const std::filesystem::path& file);
    /// The vehicles the districts spawn (art/packages/vehicles), for the parked cars.
    void LoadVehicles(const std::filesystem::path& folder);
    void LoadTextures(const p3d::File& file);
    struct Vehicle;
    /// The vehicle a parking place gets (by its district and name), or null; `paint` its colour.
    const Vehicle* PickVehicle(const p3d::Placement& place, std::uint32_t& paint) const;
    bool Upload(Pending& p, Model& model);
    float Distance(const Cell& cell, const float camera[3]) const;

    Renderer* renderer_ = nullptr;
    std::filesystem::path location_;
    std::map<int, Cell> cells_;
    Model library_;  ///< textures shared by the cells (textures.p3d.rz)
    Model low_;      ///< the low-detail city (midgeo.p3d.rz), grouped by cell
    std::unique_ptr<CollisionMesh> low_collision_;
    std::vector<bool> low_hidden_; ///< the low-detail groups whose detailed cell is in
    std::unordered_map<std::string, GpuTexture> shared_;
    /// Props by name (props.p3d.rz: their intact shape), placed by the cells' _ft files.
    std::unordered_map<std::string, std::vector<PropPart>> props_;
    int max_index_ = 0;
    engine::Lighting lighting_;
    std::vector<p3d::District> districts_;
    std::map<std::string, std::unique_ptr<Vehicle>> vehicles_;

    std::thread worker_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<int> queue_;
    std::deque<std::pair<int, std::unique_ptr<Pending>>> done_;
    float camera_[3] = {0, 0, 0};
    bool stop_ = false;
    std::atomic<int> loading_{0};
};

} // namespace engine
