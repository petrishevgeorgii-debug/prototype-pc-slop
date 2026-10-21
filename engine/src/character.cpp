// SPDX-License-Identifier: GPL-2.0-or-later
// Manhattan Engine: characters (character.h).

#include "character.h"

#include "log.h"

#include <algorithm>
#include <cmath>

namespace engine {

namespace {

using Mat = std::array<float, 16>; // row vectors: rows 0-2 the axes, row 3 the position

Mat Multiply(const Mat& a, const Mat& b) {
    Mat o{};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a[r * 4 + k] * b[k * 4 + c];
            o[r * 4 + c] = s;
        }
    return o;
}

/// The inverse of a rotation (or any linear part) and a translation.
Mat InverseAffine(const Mat& m) {
    const float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
    const float A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    float det = a * A + b * B + c * C;
    if (std::fabs(det) < 1e-12f) det = 1e-12f;
    const float s = 1.0f / det;
    Mat o{};
    o[0] = A * s, o[1] = -(b * i - c * h) * s, o[2] = (b * f - c * e) * s;
    o[4] = B * s, o[5] = (a * i - c * g) * s, o[6] = -(a * f - c * d) * s;
    o[8] = C * s, o[9] = -(a * h - b * g) * s, o[10] = (a * e - b * d) * s;
    for (int k = 0; k < 3; ++k) o[12 + k] = -(m[12] * o[k] + m[13] * o[4 + k] + m[14] * o[8 + k]);
    o[15] = 1;
    return o;
}

/// The game's quaternions as rows (checked against the skeletons' rest poses).
Mat FromPose(const std::array<float, 4>& q, const std::array<float, 3>& t) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    return {1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w), 0,
            2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w), 0,
            2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y), 0,
            t[0], t[1], t[2], 1};
}

std::array<float, 4> ToQuaternion(const float* m) {
    // m holds rows; R = transpose(m) is the usual column form.
    const float r00 = m[0], r11 = m[5], r22 = m[10];
    const float r01 = m[4], r10 = m[1], r02 = m[8], r20 = m[2], r12 = m[9], r21 = m[6];
    std::array<float, 4> q;
    const float trace = r00 + r11 + r22;
    if (trace > 0) {
        const float s = std::sqrt(trace + 1.0f) * 2;
        q = {(r21 - r12) / s, (r02 - r20) / s, (r10 - r01) / s, 0.25f * s};
    } else if (r00 > r11 && r00 > r22) {
        const float s = std::sqrt(1.0f + r00 - r11 - r22) * 2;
        q = {0.25f * s, (r01 + r10) / s, (r02 + r20) / s, (r21 - r12) / s};
    } else if (r11 > r22) {
        const float s = std::sqrt(1.0f + r11 - r00 - r22) * 2;
        q = {(r01 + r10) / s, 0.25f * s, (r12 + r21) / s, (r02 - r20) / s};
    } else {
        const float s = std::sqrt(1.0f + r22 - r00 - r11) * 2;
        q = {(r02 + r20) / s, (r12 + r21) / s, 0.25f * s, (r10 - r01) / s};
    }
    const float l = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    for (float& v : q) v /= l;
    return q;
}

std::array<float, 4> Nlerp(std::array<float, 4> a, std::array<float, 4> b, float t) {
    if (a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3] < 0) b = {-b[0], -b[1], -b[2], -b[3]};
    std::array<float, 4> q;
    for (int k = 0; k < 4; ++k) q[k] = a[k] + (b[k] - a[k]) * t;
    const float l = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (l > 0)
        for (float& v : q) v /= l;
    return q;
}

std::array<float, 4> SampleTrack(const p3d::AnimationTrack& t, float time) {
    const auto& ts = t.times;
    if (ts.size() == 1 || time <= ts.front()) return t.values.front();
    if (time >= ts.back()) return t.values.back();
    const std::size_t i = std::upper_bound(ts.begin(), ts.end(), time) - ts.begin() - 1;
    const float span = std::max(1e-4f, ts[i + 1] - ts[i]);
    const float a = (time - ts[i]) / span;
    if (t.kind == p3d::AnimationTrack::Kind::Rotation) return Nlerp(t.values[i], t.values[i + 1], a);
    std::array<float, 4> v;
    for (int k = 0; k < 4; ++k) v[k] = t.values[i][k] + (t.values[i + 1][k] - t.values[i][k]) * a;
    return v;
}

/// The clips the body uses, by what it does (all the game's: art/alex/alex.p3d.rz).
const char* kIdle = "alex_amb_stand";
const char* kWalk = "alex_loco_walk_n";       // 1.4 m/s
const char* kRun = "alex_loco_run_n";         // 6.0 m/s
const char* kSprint = "alex_loco_run_sprint_n"; // 8.1 m/s
const char* kRise = "alex_loco_amb_air";
const char* kFall = "alex_loco_jump_fall_cycle";
const char* kWallRun = "alex_wall_run_up";
const char* kAirForward = "alex_loco_jump_long_all1_start";  // leaning into a long jump
const char* kFallForward = "alex_loco_jump_long_all1_end";
const char* kTakeOff = "alex_loco_jump_long";
const char* kFlip = "alex_loco_jump_long_flip";              // the double jump
const char* kLandSoft = "alex_loco_jump_land_soft";
const char* kLandHeavy = "alex_loco_jump_land_heavy";
const char* kLandRun = "alex_loco_jump_land_touch_fwd";
const char* kDiveRoll = "alex_loco_diveroll";
const char* kRunStop = "alex_loco_run_stop";
const char* kSprintStop = "alex_loco_run_sprint_stop";
const char* kWallTop = "alex_wall_run_top";

bool Locomotion(const std::string& name) {
    return name == kWalk || name == kRun || name == kSprint;
}

} // namespace

bool Character::Load(const std::filesystem::path& file, const std::vector<std::string>& drawables, Renderer& renderer,
                     std::string& error) {
    p3d::File f;
    if (!p3d::Load(file, f, error)) return false;
    std::vector<p3d::Skeleton> skeletons;
    std::vector<p3d::Composite> composites;
    std::vector<p3d::Skin> skins;
    std::vector<p3d::Shader> shaders;
    std::vector<p3d::Texture> textures;
    p3d::ReadSkeletons(f, skeletons);
    p3d::ReadComposites(f, composites);
    p3d::ReadSkins(f, skins);
    p3d::ReadShaders(f, shaders);
    p3d::ReadTextures(f, textures);
    p3d::ReadAnimations(f, animations_);
    for (std::size_t i = 0; i < animations_.size(); ++i) by_name_[animations_[i].name] = static_cast<int>(i);

    std::unordered_map<std::string, GpuTexture> gpu;
    for (const auto& t : textures) {
        GpuTexture g;
        if (renderer.CreateTexture(t, g)) gpu[t.name] = g, model_.textures.push_back(g);
    }
    std::unordered_map<std::string, const p3d::Shader*> shader_by_name;
    for (const auto& s : shaders) shader_by_name[s.name] = &s;

    std::vector<std::uint32_t> indices;
    std::uint32_t vertices = 0;
    for (const auto& name : drawables) {
        const p3d::Composite* composite = nullptr;
        for (const auto& c : composites) {
            if (c.name == name) composite = &c;
        }
        if (!composite) continue;
        int skeleton = -1;
        for (std::size_t i = 0; i < skeletons_.size(); ++i) {
            if (skeletons_[i].name == composite->skeleton) skeleton = static_cast<int>(i);
        }
        if (skeleton < 0) {
            for (const auto& s : skeletons) {
                if (s.name != composite->skeleton) continue;
                Skeleton sk;
                sk.name = s.name;
                sk.joints = s.joints;
                // The rest pose, and its inverse in the model: what the skins are bound to.
                std::vector<Mat> world(s.joints.size());
                for (std::size_t j = 0; j < s.joints.size(); ++j) {
                    const auto& joint = s.joints[j];
                    Mat local;
                    std::copy(joint.matrix, joint.matrix + 16, local.begin());
                    sk.rest.push_back({ToQuaternion(joint.matrix), {joint.matrix[12], joint.matrix[13], joint.matrix[14]}});
                    world[j] = j == 0 || joint.parent >= j ? local : Multiply(local, world[joint.parent]);
                    sk.inverse.push_back(InverseAffine(world[j]));
                    if (joint.name == "Motion_Root") sk.motion_root = static_cast<int>(j);
                }
                sk.world = world;
                skeletons_.push_back(std::move(sk));
                skeleton = static_cast<int>(skeletons_.size()) - 1;
            }
        }
        if (skeleton < 0) continue;
        for (const auto& prop : composite->props) {
            for (const auto& skin : skins) {
                if (skin.name != prop.mesh) continue;
                for (const auto& part : skin.parts) {
                    Piece piece;
                    piece.skeleton = skeleton;
                    piece.skin = part;
                    piece.base = vertices;
                    engine::Draw d;
                    d.first_index = static_cast<std::uint32_t>(indices.size());
                    d.index_count = static_cast<std::uint32_t>(part.part.indices.size());
                    for (std::uint32_t i : part.part.indices) indices.push_back(i + vertices);
                    vertices += static_cast<std::uint32_t>(part.part.vertices.size());
                    if (auto it = shader_by_name.find(part.part.shader); it != shader_by_name.end()) {
                        const auto colour = gpu.find(p3d::ColourTexture(*it->second));
                        const auto normal = gpu.find(p3d::NormalTexture(*it->second));
                        const GpuTexture* c = colour != gpu.end() ? &colour->second : nullptr;
                        const GpuTexture* n = normal != gpu.end() ? &normal->second : nullptr;
                        if (c) d.flags |= Material::Textured;
                        if (n) d.flags |= Material::NormalMap;
                        d.material = renderer.CreateMaterial(c, n);
                        if (d.material) model_.materials.push_back(d.material);
                    }
                    model_.draws.push_back(d);
                    pieces_.push_back(std::move(piece));
                }
            }
        }
    }
    if (pieces_.empty()) {
        error = "no skinned meshes for " + file.filename().string();
        return false;
    }
    skinned_.resize(vertices);
    if (!renderer.CreateDynamicModel(vertices, indices, model_)) {
        error = "cannot create the character's buffers";
        return false;
    }
    Log("  %s: %zu meshes, %u vertices, %zu skeletons, %zu animations\n", file.filename().string().c_str(), pieces_.size(),
        vertices, skeletons_.size(), animations_.size());
    Play(kIdle, 1.0f, true);
    blend_ = 1.0f;
    return true;
}

void Character::Destroy(Renderer& renderer) {
    renderer.DestroyModel(model_);
    pieces_.clear();
    skeletons_.clear();
}

int Character::Find(const std::string& name) const {
    auto it = by_name_.find(name);
    return it == by_name_.end() ? -1 : it->second;
}

std::string Character::Playing() const {
    return current_.animation >= 0 ? animations_[current_.animation].name : std::string();
}

void Character::Play(const std::string& name, float rate, bool loop, float start, bool restart) {
    const int a = Find(name);
    if (a < 0) return;
    if (a == current_.animation && !restart) {
        current_.rate = rate;
        current_.loop = loop;
        return;
    }
    const float length = std::max(1.0f, animations_[a].frames - 1);
    Clip next{a, start * length, rate, loop};
    // Running into sprinting (and back) keeps the stride where it was.
    if (!restart && current_.animation >= 0 && Locomotion(animations_[current_.animation].name) && Locomotion(name)) {
        const float from = std::max(1.0f, animations_[current_.animation].frames - 1);
        next.time = current_.time / from * length;
    }
    previous_ = current_;
    current_ = next;
    blend_ = previous_.animation >= 0 ? 0.0f : 1.0f;
}

void Character::Act(Action action, const std::string& name, float rate, float start) {
    if (Find(name) < 0) return;
    Play(name, rate, false, start, true);
    action_ = action;
    action_time_ = 0;
}

bool Character::Finished(const Clip& clip) const {
    if (clip.animation < 0 || clip.loop) return false;
    return clip.time >= std::max(1.0f, animations_[clip.animation].frames - 1) - 0.01f;
}

bool Character::Force(const std::string& name, float time) {
    const int a = Find(name);
    if (a < 0) return false;
    current_ = {a, time, 1.0f, true};
    previous_ = {};
    blend_ = 1.0f;
    forced_ = true;
    return true;
}

void Character::Advance(Clip& clip, float dt) const {
    if (clip.animation < 0) return;
    const auto& a = animations_[clip.animation];
    const float length = std::max(1.0f, a.frames - 1);
    clip.time += dt * a.fps * clip.rate;
    if (clip.loop) clip.time = std::fmod(clip.time, length);
    else clip.time = std::min(clip.time, length);
}

void Character::Animate(float dt, const CharacterState& s) {
    if (!forced_) {
        // What happened starts a move.
        if (s.jumped) Act(Action::TakeOff, kTakeOff, 1.3f);
        if (s.double_jumped) Act(Action::Flip, kFlip, 1.25f, 0.2f);
        if (s.left_wall_top) Act(Action::WallTop, kWallTop, 1.0f);
        if (s.landed) {
            if (s.impact > 28.0f) Act(Action::HeavyLand, s.speed > 10.0f ? kDiveRoll : kLandHeavy, 1.1f, s.speed > 10.0f ? 0.1f : 0.0f);
            else if (s.speed > 10.0f) action_ = Action::None; // sprinting on
            else if (s.speed > 3.0f) Act(Action::Land, kLandRun, 1.2f);
            else if (s.impact > 6.0f) Act(Action::Land, kLandSoft, 1.2f, 0.05f);
            else action_ = Action::None;
        }
        if (s.grounded && was_moving_ && !s.moving && last_speed_ > 4.0f && (action_ == Action::None || action_ == Action::Land)) {
            Act(Action::Stop, last_speed_ > 12.0f ? kSprintStop : kRunStop, 1.2f);
        }
        // A move plays out unless what the body does ends it.
        if (action_ != Action::None) {
            action_time_ += dt;
            bool end = Finished(current_);
            switch (action_) {
            case Action::TakeOff: end = end || (s.grounded && action_time_ > 0.2f) || s.wall_running; break;
            case Action::Flip: end = end || s.grounded || s.wall_running; break;
            case Action::Land:
            case Action::Stop: end = end || !s.grounded || (s.moving && action_time_ > 0.15f && action_ == Action::Stop) ||
                                     (s.moving && action_time_ > 0.35f); break;
            case Action::HeavyLand: end = end || !s.grounded || s.wall_running || (s.moving && action_time_ > 0.9f); break;
            case Action::WallTop: end = end || s.wall_running || (s.grounded && action_time_ > 0.25f); break;
            case Action::None: break;
            }
            if (end) action_ = Action::None;
        }
        if (action_ == Action::None) {
            if (s.wall_running) Play(kWallRun, 1.5f, true);
            else if (!s.grounded) {
                // In the air: leaning into it when moving fast.
                if (s.vertical > 1.0f) Play(s.speed > 6.0f ? kAirForward : kRise, 1.0f, true);
                else Play(s.speed > 6.0f ? kFallForward : kFall, 1.0f, true);
            } else if (s.speed < 0.4f) Play(kIdle, 1.0f, true);
            else if (s.speed < 2.5f) Play(kWalk, s.speed / 1.39f, true);
            else if (s.speed < 11.0f) Play(kRun, std::max(0.7f, s.speed / 6.0f), true);
            else Play(kSprint, std::clamp(s.speed / 8.14f, 1.0f, 1.9f), true);
        }
    }
    last_speed_ = s.speed;
    was_moving_ = s.moving;
    Advance(current_, dt);
    Advance(previous_, dt);
    blend_ = std::min(1.0f, blend_ + dt / 0.15f);
}

const std::vector<int>& Character::Tracks(int animation, const Skeleton& skeleton) {
    const std::uint64_t key = (std::uint64_t(animation) << 32) | std::uint64_t(&skeleton - skeletons_.data());
    auto it = track_joints_.find(key);
    if (it != track_joints_.end()) return it->second;
    std::vector<int> map;
    for (const auto& t : animations_[animation].tracks) {
        int joint = -1;
        for (std::size_t j = 0; j < skeleton.joints.size(); ++j) {
            if (skeleton.joints[j].name == t.joint) joint = static_cast<int>(j);
        }
        map.push_back(joint);
    }
    return track_joints_[key] = std::move(map);
}

void Character::Sample(const Clip& clip, const Skeleton& skeleton, std::vector<Pose>& out) {
    out = skeleton.rest;
    if (clip.animation < 0) return;
    const auto& a = animations_[clip.animation];
    const auto& map = Tracks(clip.animation, skeleton);
    for (std::size_t i = 0; i < a.tracks.size(); ++i) {
        const int j = map[i];
        if (j < 0 || j == skeleton.motion_root) continue; // the body is moved by the engine
        const auto v = SampleTrack(a.tracks[i], clip.time);
        if (a.tracks[i].kind == p3d::AnimationTrack::Kind::Rotation) out[j].rotation = v;
        else out[j].translation = {v[0], v[1], v[2]};
    }
}

void Character::Draw(Renderer& renderer, const float placement[16]) {
    Mat place;
    std::copy(placement, placement + 16, place.begin());
    for (auto& sk : skeletons_) {
        Sample(current_, sk, pose_a_);
        if (blend_ < 1.0f) {
            Sample(previous_, sk, pose_b_);
            for (std::size_t j = 0; j < pose_a_.size(); ++j) {
                pose_a_[j].rotation = Nlerp(pose_b_[j].rotation, pose_a_[j].rotation, blend_);
                for (int k = 0; k < 3; ++k)
                    pose_a_[j].translation[k] = pose_b_[j].translation[k] + (pose_a_[j].translation[k] - pose_b_[j].translation[k]) * blend_;
            }
        }
        for (std::size_t j = 0; j < sk.joints.size(); ++j) {
            const Mat local = FromPose(pose_a_[j].rotation, pose_a_[j].translation);
            const std::uint32_t parent = sk.joints[j].parent;
            sk.world[j] = j == 0 || parent >= j ? local : Multiply(local, sk.world[parent]);
        }
        // Each joint's skinning matrix: from the rest pose to this pose, then into the world.
        for (std::size_t j = 0; j < sk.joints.size(); ++j) sk.world[j] = Multiply(Multiply(sk.inverse[j], sk.world[j]), place);
    }
    for (const auto& piece : pieces_) {
        const auto& sk = skeletons_[piece.skeleton];
        const auto& src = piece.skin.part.vertices;
        for (std::size_t i = 0; i < src.size(); ++i) {
            const p3d::Vertex& v = src[i];
            p3d::Vertex& o = skinned_[piece.base + i];
            o = v;
            float p[3] = {0, 0, 0}, n[3] = {0, 0, 0}, t[3] = {0, 0, 0};
            const auto& joints = piece.skin.joints[i];
            const auto& weights = piece.skin.weights[i];
            for (int k = 0; k < 4; ++k) {
                const float w = weights[k];
                if (w <= 1e-4f || joints[k] >= sk.world.size()) continue;
                const Mat& m = sk.world[joints[k]];
                for (int c = 0; c < 3; ++c) {
                    p[c] += w * (v.position[0] * m[c] + v.position[1] * m[4 + c] + v.position[2] * m[8 + c] + m[12 + c]);
                    n[c] += w * (v.normal[0] * m[c] + v.normal[1] * m[4 + c] + v.normal[2] * m[8 + c]);
                    t[c] += w * (v.tangent[0] * m[c] + v.tangent[1] * m[4 + c] + v.tangent[2] * m[8 + c]);
                }
            }
            std::copy(p, p + 3, o.position);
            std::copy(n, n + 3, o.normal);
            std::copy(t, t + 3, o.tangent);
        }
    }
    renderer.UpdateDynamicModel(model_, skinned_);
}

} // namespace engine
