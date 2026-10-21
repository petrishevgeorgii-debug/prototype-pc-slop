// SPDX-License-Identifier: GPL-2.0-or-later
// Manhattan Engine: collision against the city's triangles - the ground under a point and
// spheres pushed out of walls (docs/ENGINE.md, "On foot").

#pragma once

#include <cstdint>
#include <vector>

#include "p3d.h"

namespace engine {

/// Triangles in a grid over x and z, for the few queries walking needs.
class CollisionMesh {
public:
    /// Adds triangles (indices into `vertices`); `group` is what Skip() can leave out later
    /// (the low-detail city: the cell a triangle stands for).
    void Add(const std::vector<p3d::Vertex>& vertices, const std::uint32_t* indices, std::size_t count, int group = -1);
    /// Makes the grid; call once all triangles are added.
    void Build(float cell_size = 4.0f);
    bool Empty() const { return tris_.empty(); }
    std::size_t Triangles() const { return tris_.size() / 3; }

    /// The highest surface at (x, z) between `top` and `bottom` that faces up (normal y at
    /// least `min_up`). False when there is none.
    bool Ground(float x, float z, float top, float bottom, float min_up, float& y, const std::vector<bool>* skip = nullptr) const;
    /// Moves the sphere `c`, `r` out of the triangles it is in. Adds the push (summed normals) to
    /// `push`; returns how many triangles it touched.
    int PushSphere(float c[3], float r, float push[3], const std::vector<bool>* skip = nullptr) const;

    float min[3] = {0, 0, 0}, max[3] = {0, 0, 0};

private:
    bool Skipped(std::uint32_t t, const std::vector<bool>* skip) const {
        if (!skip || groups_.empty()) return false;
        const int g = groups_[t];
        return g >= 0 && g < static_cast<int>(skip->size()) && (*skip)[g];
    }
    template <class F>
    void ForCells(float x0, float z0, float x1, float z1, const F& f) const;

    std::vector<float> positions_;   ///< x y z per vertex
    std::vector<std::uint32_t> tris_; ///< 3 per triangle
    std::vector<int> groups_;         ///< per triangle, when any has a group
    float cell_ = 4.0f;
    int nx_ = 0, nz_ = 0;
    std::vector<std::uint32_t> start_, items_; ///< triangles of each grid cell
    mutable std::vector<std::uint32_t> stamp_; ///< visited marks, per triangle
    mutable std::uint32_t query_ = 0;
};

} // namespace engine
