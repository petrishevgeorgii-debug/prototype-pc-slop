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

private:
    float safe_[3] = {0, 0, 0}; ///< the last place it stood, to come back to after a fall out of the city
    bool has_safe_ = false;
};

} // namespace engine
