// SPDX-License-Identifier: GPL-2.0-or-later
// Manhattan Engine: on foot (player.h).

#include "player.h"

#include "city.h"

#include <algorithm>
#include <cmath>

namespace engine {

namespace {

constexpr float kRadius = 0.4f;    ///< the body's spheres
constexpr float kStep = 0.55f;     ///< kerbs and stairs it walks up
constexpr float kHead = 1.45f;     ///< the upper sphere's centre above the feet
constexpr float kEye = 1.7f;
constexpr float kWalk = 6.0f;      ///< metres per second
constexpr float kSprint = 22.0f;   ///< Alex runs as fast as traffic
constexpr float kJump = 13.0f;     ///< about 8 m up
constexpr float kGravity = 28.0f;
constexpr float kWallRun = 16.0f;  ///< up a wall while sprinting into it

} // namespace

void Player::Place(const float feet[3]) {
    std::copy(feet, feet + 3, position);
    std::fill(velocity, velocity + 3, 0.0f);
    grounded = wall_running = false;
}

void Player::Eye(float out[3]) const {
    out[0] = position[0];
    out[1] = position[1] + kEye;
    out[2] = position[2];
}

void Player::Update(float dt, const PlayerInput& in, const City& city) {
    if (dt <= 0) return;
    // Where it wants to go, on the ground plane.
    const float fx = std::sin(in.yaw), fz = -std::cos(in.yaw);
    float wish[2] = {fx * in.forward + -fz * in.right, fz * in.forward + fx * in.right};
    const float wl = std::sqrt(wish[0] * wish[0] + wish[1] * wish[1]);
    if (wl > 1.0f) wish[0] /= wl, wish[1] /= wl;
    const float speed = in.sprint ? kSprint : kWalk;
    const float rate = grounded ? 12.0f : wall_running ? 10.0f : 2.5f;
    const float k = std::min(1.0f, rate * dt);
    velocity[0] += (wish[0] * speed - velocity[0]) * k;
    velocity[2] += (wish[1] * speed - velocity[2]) * k;
    if (in.jump && (grounded || wall_running)) {
        velocity[1] = kJump;
        grounded = false;
    }
    velocity[1] = std::max(velocity[1] - kGravity * dt, -60.0f);

    // Moved in steps short enough not to pass through a wall.
    const float travel = std::sqrt(velocity[0] * velocity[0] + velocity[1] * velocity[1] + velocity[2] * velocity[2]) * dt;
    const int steps = std::clamp(static_cast<int>(std::ceil(travel / 0.25f)), 1, 64);
    const float h = dt / steps;
    bool wall = false, on_ground = false;
    float wall_normal[2] = {0, 0};
    for (int s = 0; s < steps; ++s) {
        const bool was_grounded = grounded || on_ground;
        for (int k3 = 0; k3 < 3; ++k3) position[k3] += velocity[k3] * h;
        // The body's two spheres, out of walls and ceilings.
        for (const float offset : {kStep + kRadius, kHead}) {
            float c[3] = {position[0], position[1] + offset, position[2]};
            float push[3] = {0, 0, 0};
            if (!city.Push(c, kRadius, push)) continue;
            position[0] = c[0];
            position[2] = c[2];
            const float pl = std::sqrt(push[0] * push[0] + push[1] * push[1] + push[2] * push[2]);
            if (pl <= 0) continue;
            if (push[1] < -0.5f * pl) { // a ceiling
                position[1] = c[1] - offset;
                velocity[1] = std::min(velocity[1], 0.0f);
            }
            const float hl = std::sqrt(push[0] * push[0] + push[2] * push[2]);
            if (hl > 0.3f * pl) {
                wall = true;
                wall_normal[0] = push[0] / hl;
                wall_normal[1] = push[2] / hl;
                const float into = velocity[0] * wall_normal[0] + velocity[2] * wall_normal[1];
                if (into < 0) velocity[0] -= wall_normal[0] * into, velocity[2] -= wall_normal[1] * into;
            }
        }
        // The ground: up a step, and down one while walking (not off a ledge mid-jump).
        if (velocity[1] <= 0) {
            const float top = position[1] + kStep;
            const float bottom = position[1] - (was_grounded ? kStep : 0.02f) + std::min(velocity[1] * h, 0.0f);
            float y = 0;
            if (city.Ground(position[0], position[2], top, bottom, y)) {
                position[1] = y;
                velocity[1] = 0;
                on_ground = true;
            } else {
                on_ground = false;
            }
        } else {
            on_ground = false;
        }
    }
    grounded = on_ground;
    // Running up walls: sprinting into one.
    wall_running = false;
    if (wall && in.sprint && wish[0] * -wall_normal[0] + wish[1] * -wall_normal[1] > 0.3f) {
        velocity[1] = std::max(velocity[1], kWallRun);
        wall_running = true;
    }
    if (grounded) {
        std::copy(position, position + 3, safe_);
        has_safe_ = true;
    }
    // Out of the city (the river, a gap): back to where it last stood.
    if (position[1] < -60.0f) {
        if (has_safe_) Place(safe_);
        else position[1] = 200.0f, velocity[1] = 0;
    }
}

} // namespace engine
