// SPDX-License-Identifier: GPL-2.0-or-later
// Manhattan Engine: collision (collision.h).

#include "collision.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace engine {

namespace {

struct V3 {
    float x, y, z;
};
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }

/// The point of triangle abc nearest to p (Ericson, Real-Time Collision Detection 5.1.5).
V3 Closest(V3 p, V3 a, V3 b, V3 c) {
    const V3 ab = b - a, ac = c - a, ap = p - a;
    const float d1 = Dot(ab, ap), d2 = Dot(ac, ap);
    if (d1 <= 0 && d2 <= 0) return a;
    const V3 bp = p - b;
    const float d3 = Dot(ab, bp), d4 = Dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) return b;
    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) return a + ab * (d1 / (d1 - d3));
    const V3 cp = p - c;
    const float d5 = Dot(ab, cp), d6 = Dot(ac, cp);
    if (d6 >= 0 && d5 <= d6) return c;
    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) return a + ac * (d2 / (d2 - d6));
    const float va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
    const float denom = 1.0f / (va + vb + vc);
    return a + ab * (vb * denom) + ac * (vc * denom);
}

} // namespace

void CollisionMesh::Add(const std::vector<p3d::Vertex>& vertices, const std::uint32_t* indices, std::size_t count, int group) {
    // Each vertex is copied once per call and shared by its triangles.
    std::vector<std::uint32_t> remap;
    std::uint32_t lo = UINT32_MAX, hi = 0;
    for (std::size_t i = 0; i < count; ++i) lo = std::min(lo, indices[i]), hi = std::max(hi, indices[i]);
    if (count == 0 || hi >= vertices.size()) return;
    remap.assign(hi - lo + 1, UINT32_MAX);
    for (std::size_t i = 0; i + 2 < count; i += 3) {
        std::uint32_t t[3];
        for (int k = 0; k < 3; ++k) {
            std::uint32_t& r = remap[indices[i + k] - lo];
            if (r == UINT32_MAX) {
                const auto& v = vertices[indices[i + k]].position;
                r = static_cast<std::uint32_t>(positions_.size() / 3);
                positions_.insert(positions_.end(), {v[0], v[1], v[2]});
            }
            t[k] = r;
        }
        if (t[0] == t[1] || t[1] == t[2] || t[0] == t[2]) continue;
        tris_.insert(tris_.end(), {t[0], t[1], t[2]});
        if (group >= 0 && groups_.empty()) groups_.assign(tris_.size() / 3 - 1, -1);
        if (!groups_.empty()) groups_.push_back(group);
    }
}

void CollisionMesh::Build(float cell_size) {
    cell_ = cell_size;
    std::fill(min, min + 3, FLT_MAX);
    std::fill(max, max + 3, -FLT_MAX);
    for (std::size_t i = 0; i < positions_.size(); i += 3) {
        for (int k = 0; k < 3; ++k) min[k] = std::min(min[k], positions_[i + k]), max[k] = std::max(max[k], positions_[i + k]);
    }
    if (tris_.empty()) return;
    // A huge mesh (the low-detail city) gets bigger grid cells.
    const float span = std::max(max[0] - min[0], max[2] - min[2]);
    if (span / cell_ > 1024) cell_ = span / 1024;
    nx_ = static_cast<int>((max[0] - min[0]) / cell_) + 1;
    nz_ = static_cast<int>((max[2] - min[2]) / cell_) + 1;
    std::vector<std::uint32_t> counts(std::size_t(nx_) * nz_ + 1, 0);
    const auto range = [&](std::uint32_t t, int& x0, int& z0, int& x1, int& z1) {
        float lo[2] = {FLT_MAX, FLT_MAX}, hi[2] = {-FLT_MAX, -FLT_MAX};
        for (int k = 0; k < 3; ++k) {
            const float* p = &positions_[std::size_t(tris_[t * 3 + k]) * 3];
            lo[0] = std::min(lo[0], p[0]), hi[0] = std::max(hi[0], p[0]);
            lo[1] = std::min(lo[1], p[2]), hi[1] = std::max(hi[1], p[2]);
        }
        x0 = std::clamp(static_cast<int>((lo[0] - min[0]) / cell_), 0, nx_ - 1);
        x1 = std::clamp(static_cast<int>((hi[0] - min[0]) / cell_), 0, nx_ - 1);
        z0 = std::clamp(static_cast<int>((lo[1] - min[2]) / cell_), 0, nz_ - 1);
        z1 = std::clamp(static_cast<int>((hi[1] - min[2]) / cell_), 0, nz_ - 1);
    };
    const auto n = static_cast<std::uint32_t>(tris_.size() / 3);
    for (std::uint32_t t = 0; t < n; ++t) {
        int x0, z0, x1, z1;
        range(t, x0, z0, x1, z1);
        for (int z = z0; z <= z1; ++z)
            for (int x = x0; x <= x1; ++x) ++counts[std::size_t(z) * nx_ + x];
    }
    start_.assign(counts.size(), 0);
    for (std::size_t i = 1; i < counts.size(); ++i) start_[i] = start_[i - 1] + counts[i - 1];
    items_.assign(start_.back(), 0);
    std::vector<std::uint32_t> fill(start_.begin(), start_.end() - 1);
    for (std::uint32_t t = 0; t < n; ++t) {
        int x0, z0, x1, z1;
        range(t, x0, z0, x1, z1);
        for (int z = z0; z <= z1; ++z)
            for (int x = x0; x <= x1; ++x) items_[fill[std::size_t(z) * nx_ + x]++] = t;
    }
    stamp_.assign(n, 0);
}

template <class F>
void CollisionMesh::ForCells(float x0, float z0, float x1, float z1, const F& f) const {
    if (tris_.empty() || x1 < min[0] || x0 > max[0] || z1 < min[2] || z0 > max[2]) return;
    if (++query_ == 0) std::fill(stamp_.begin(), stamp_.end(), 0), query_ = 1;
    const int cx0 = std::clamp(static_cast<int>((x0 - min[0]) / cell_), 0, nx_ - 1);
    const int cx1 = std::clamp(static_cast<int>((x1 - min[0]) / cell_), 0, nx_ - 1);
    const int cz0 = std::clamp(static_cast<int>((z0 - min[2]) / cell_), 0, nz_ - 1);
    const int cz1 = std::clamp(static_cast<int>((z1 - min[2]) / cell_), 0, nz_ - 1);
    for (int z = cz0; z <= cz1; ++z) {
        for (int x = cx0; x <= cx1; ++x) {
            const std::size_t c = std::size_t(z) * nx_ + x;
            for (std::uint32_t i = start_[c]; i < start_[c + 1]; ++i) {
                const std::uint32_t t = items_[i];
                if (stamp_[t] == query_) continue;
                stamp_[t] = query_;
                f(t);
            }
        }
    }
}

bool CollisionMesh::Ground(float x, float z, float top, float bottom, float min_up, float& y, const std::vector<bool>* skip) const {
    bool found = false;
    ForCells(x, z, x, z, [&](std::uint32_t t) {
        if (Skipped(t, skip)) return;
        const float* a = &positions_[std::size_t(tris_[t * 3]) * 3];
        const float* b = &positions_[std::size_t(tris_[t * 3 + 1]) * 3];
        const float* c = &positions_[std::size_t(tris_[t * 3 + 2]) * 3];
        // (x, z) inside the triangle's footprint: barycentric coordinates over x and z.
        const float d = (b[2] - c[2]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[2] - c[2]);
        if (std::fabs(d) < 1e-9f) return; // a wall
        const float u = ((b[2] - c[2]) * (x - c[0]) + (c[0] - b[0]) * (z - c[2])) / d;
        const float v = ((c[2] - a[2]) * (x - c[0]) + (a[0] - c[0]) * (z - c[2])) / d;
        const float w = 1 - u - v;
        if (u < -1e-4f || v < -1e-4f || w < -1e-4f) return;
        const float h = u * a[1] + v * b[1] + w * c[1];
        if (h > top || h < bottom || (found && h <= y)) return;
        V3 n = Cross(V3{b[0] - a[0], b[1] - a[1], b[2] - a[2]}, V3{c[0] - a[0], c[1] - a[1], c[2] - a[2]});
        const float len = std::sqrt(Dot(n, n));
        if (len <= 0 || std::fabs(n.y) / len < min_up) return; // too steep to stand on (either winding)
        y = h;
        found = true;
    });
    return found;
}

int CollisionMesh::PushSphere(float c[3], float r, float push[3], const std::vector<bool>* skip) const {
    int contacts = 0;
    ForCells(c[0] - r, c[2] - r, c[0] + r, c[2] + r, [&](std::uint32_t t) {
        if (Skipped(t, skip)) return;
        const float* a = &positions_[std::size_t(tris_[t * 3]) * 3];
        const float* b = &positions_[std::size_t(tris_[t * 3 + 1]) * 3];
        const float* cc = &positions_[std::size_t(tris_[t * 3 + 2]) * 3];
        const float ylo = std::min({a[1], b[1], cc[1]}), yhi = std::max({a[1], b[1], cc[1]});
        if (c[1] + r < ylo || c[1] - r > yhi) return;
        const V3 p{c[0], c[1], c[2]};
        const V3 q = Closest(p, {a[0], a[1], a[2]}, {b[0], b[1], b[2]}, {cc[0], cc[1], cc[2]});
        const V3 d = p - q;
        const float dist2 = Dot(d, d);
        if (dist2 >= r * r || dist2 < 1e-12f) return;
        const float dist = std::sqrt(dist2);
        const V3 n = d * (1.0f / dist);
        const float depth = r - dist;
        c[0] += n.x * depth, c[1] += n.y * depth, c[2] += n.z * depth;
        push[0] += n.x, push[1] += n.y, push[2] += n.z;
        ++contacts;
    });
    return contacts;
}

} // namespace engine
