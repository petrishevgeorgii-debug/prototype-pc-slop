// SPDX-License-Identifier: GPL-2.0-or-later
// Manhattan Engine: characters - the game's skinned models posed by the game's own animations
// (docs/ENGINE.md, "Characters"). First: Alex, from art/alex/alex.p3d.rz.

#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "p3d.h"
#include "renderer.h"

namespace engine {

/// What the body is doing, for choosing its animation.
struct CharacterState {
    float speed = 0;        ///< over the ground, m/s
    float vertical = 0;     ///< m/s, up
    bool grounded = true;
    bool wall_running = false;
};

class Character {
public:
    /// `file`: the character's .p3d.rz; `drawables`: the CompositeDrawables that make its body
    /// (Alex: alex_reg_body and alex_reg_arms).
    bool Load(const std::filesystem::path& file, const std::vector<std::string>& drawables, Renderer& renderer,
              std::string& error);
    void Destroy(Renderer& renderer);

    /// Chooses and advances the animation.
    void Animate(float dt, const CharacterState& state);
    /// Plays one animation only (by name; checking clips): false when there is none.
    bool Force(const std::string& animation, float time = 0);
    /// Poses the skeletons, skins the meshes and hands them to the renderer, placed by
    /// `placement` (row vectors: right, up, back, position).
    void Draw(Renderer& renderer, const float placement[16]);

    const Model& GetModel() const { return model_; }
    std::size_t AnimationCount() const { return animations_.size(); }
    std::string Playing() const;

private:
    struct Pose {
        std::array<float, 4> rotation; ///< quaternion
        std::array<float, 3> translation;
    };
    struct Skeleton {
        std::string name;
        std::vector<p3d::Joint> joints;
        std::vector<Pose> rest;                      ///< local
        std::vector<std::array<float, 16>> inverse;  ///< of each joint's rest pose in the model
        std::vector<std::array<float, 16>> world;    ///< this frame's
        int motion_root = -1;                        ///< kept still (the engine moves the body)
    };
    struct Piece {
        int skeleton = 0;
        p3d::SkinPart skin;
        std::uint32_t base = 0; ///< its first vertex in the model
    };
    struct Clip {
        int animation = -1;
        float time = 0, rate = 1;
        bool loop = true;
    };

    int Find(const std::string& name) const;
    void Play(const std::string& name, float rate, bool loop);
    void Advance(Clip& clip, float dt) const;
    /// The clip's local pose of a skeleton (joints it does not animate keep the rest pose).
    void Sample(const Clip& clip, const Skeleton& skeleton, std::vector<Pose>& out);
    const std::vector<int>& Tracks(int animation, const Skeleton& skeleton);

    std::vector<Skeleton> skeletons_;
    std::vector<Piece> pieces_;
    std::vector<p3d::Animation> animations_;
    std::unordered_map<std::string, int> by_name_;
    std::unordered_map<std::uint64_t, std::vector<int>> track_joints_;
    Clip current_, previous_;
    float blend_ = 1.0f; ///< of the current clip over the previous one
    bool forced_ = false;
    Model model_;
    std::vector<p3d::Vertex> skinned_;
    std::vector<Pose> pose_a_, pose_b_;
};

} // namespace engine
