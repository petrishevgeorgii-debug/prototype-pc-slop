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
#include <unordered_map>
#include <vector>

#include "p3d.h"
#include "renderer.h"

namespace engine {

class City {
public:
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
    };
    struct Pending; // CPU data of a loaded cell, waiting for upload

    struct PropPart {
        std::vector<p3d::Vertex> vertices;
        std::vector<std::uint32_t> indices;
        std::string texture;
    };

    void Worker();
    /// A cell's (or the low-detail city's) geometry; `placements`: the cell's _ft file, whose
    /// props are added where it places them.
    std::unique_ptr<Pending> Build(const std::filesystem::path& file, int group_from_name,
                                   const std::filesystem::path& placements = {});
    void LoadProps(const std::filesystem::path& file);
    bool Upload(Pending& p, Model& model);
    float Distance(const Cell& cell, const float camera[3]) const;

    Renderer* renderer_ = nullptr;
    std::filesystem::path location_;
    std::map<int, Cell> cells_;
    Model library_;  ///< textures shared by the cells (textures.p3d.rz)
    Model low_;      ///< the low-detail city (midgeo.p3d.rz), grouped by cell
    std::unordered_map<std::string, VkDescriptorSet> shared_;
    /// Props by name (props.p3d.rz: their intact shape), placed by the cells' _ft files.
    std::unordered_map<std::string, std::vector<PropPart>> props_;
    int max_index_ = 0;

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
