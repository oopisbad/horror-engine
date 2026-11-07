#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace horror {

// First-person camera. Deliberately dumb: it stores orientation/limbs and
// produces matrices; the character controller (or any code) drives it.
//
// Head-bob is exposed as a hook: controller writes a bobOffset + bobRoll it
// computes from walk phase; camera folds it into view/orientation so effects
// like weapon sway can read the same value later.
class Camera {
public:
    // Yaw/pitch in radians. Right-handed convention consistent with GLM:
    // yaw rotates around +Y; pitch around +X (look up = positive pitch).
    float yaw = 0.0f;
    float pitch = 0.0f;

    float fovY = 75.0f; // degrees
    float nearPlane = 0.05f;
    float farPlane = 200.0f;

    glm::vec3 position{0.0f};

    // Head-bob contribution published by the controller each frame and folded
    // into the view by update(), exactly once. The offset is camera-local:
    // x = right, y = up, z = forward. position stays the un-bobbed eye so
    // gameplay (flashlight, self-tests, specular) can rely on it.
    glm::vec3 bobOffset{0.0f};
    float bobRoll = 0.0f; // radians of roll around forward axis

    void setPitchClamp(float minDeg, float maxDeg)
    {
        minPitchDeg = minDeg;
        maxPitchDeg = maxDeg;
    }
    void clampPitch();

    // Basis vectors matching yaw/pitch (no roll) -- movement and the
    // flashlight want these rather than the full bobbed basis.
    [[nodiscard]] glm::vec3 forward() const;
    [[nodiscard]] glm::vec3 right() const;
    [[nodiscard]] glm::vec3 up() const;

    // View matrix from the unbobbed/unrolled basis; the flashlight's shadow
    // projection looks through this so aim shadows stay stable while bobbing.
    [[nodiscard]] glm::mat4 viewMatrixNoRoll() const;

    // Build the standard matrices; projection * view with the bobbed basis.
    void update(float aspect);

    const glm::mat4 &view() const { return view_; }
    const glm::mat4 &projection() const { return projection_; }
    const glm::mat4 &viewProjection() const { return viewProjection_; }

private:
    float minPitchDeg = -89.0f;
    float maxPitchDeg = 89.0f;
    glm::mat4 view_{1.0f};
    glm::mat4 projection_{1.0f};
    glm::mat4 viewProjection_{1.0f};
};

} // namespace horror
