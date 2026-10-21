// SPDX-License-Identifier: GPL-2.0-or-later
// Manhattan Engine: on foot - a body that walks, runs, jumps and runs up walls against the
// city's collision (docs/ENGINE.md, "On foot"). The first step towards Alex.

#pragma once

namespace engine {

class City;

struct PlayerInput {
    float forward = 0, right = 0; ///< -1..1
    bool sprint = false, jump = false;
    float yaw = 0;                ///< the camera's, radians (0 looks along -z)
};

class Player {
public:
    /// Puts the body at `feet` (it falls from there).
    void Place(const float feet[3]);
    void Update(float dt, const PlayerInput& input, const City& city);
    /// Where the camera is: the eyes.
    void Eye(float out[3]) const;

    float position[3] = {0, 0, 0}; ///< the feet
    float velocity[3] = {0, 0, 0};
    bool grounded = false, wall_running = false;
    float facing = 0;                ///< where the body faces (yaw, radians; 0: -z)
    float wall_normal[2] = {0, 1};   ///< x z, away from the wall it runs up
    // What happened in the last Update:
    bool jumped = false, double_jumped = false, landed = false, left_wall_top = false;
    float impact = 0;                ///< falling speed when it landed

private:
    float safe_[3] = {0, 0, 0}; ///< the last place it stood, to come back to after a fall out of the city
    bool has_safe_ = false;
    bool double_used_ = false;
    float stun_ = 0; ///< seconds held after a heavy landing
};

} // namespace engine
