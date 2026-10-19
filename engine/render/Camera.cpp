#include "render/Camera.hpp"

#include <algorithm>
#include <cmath>

namespace horror {

void Camera::clampPitch()
{
    pitch = std::clamp(pitch, glm::radians(minPitchDeg), glm::radians(maxPitchDeg));
}

glm::vec3 Camera::forward() const
{
    // Yaw around +Y, then pitch around the local +X axis (right vector).
    // At yaw=0 we look down -Z (GL convention: forward into the screen).
    float cp = std::cos(pitch);
    return glm::vec3(
        -std::sin(yaw) * cp,
        std::sin(pitch),
        -std::cos(yaw) * cp);
}

glm::vec3 Camera::right() const
{
    // Roll-free right vector (depends on yaw only).
    return glm::vec3(std::cos(yaw), 0.0f, -std::sin(yaw));
}

glm::vec3 Camera::up() const
{
    // Rebuild from forward x right to stay orthonormal with the clamped pitch.
    return glm::cross(right(), forward());
}

// The flashlight aims along the *unbobbed* basis so aiming stays level while
// walking; games that want "light driven by the hand" attach it elsewhere.
glm::mat4 Camera::viewMatrixNoRoll() const
{
    return glm::lookAt(position, position + forward(), glm::vec3(0, 1, 0));
}

void Camera::update(float aspect)
{
    clampPitch();

    glm::vec3 eye = position + bobOffset;

    glm::vec3 f = forward();
    glm::vec3 r = right();
    glm::vec3 u = up();

    // Roll only bends the camera axes around forward; the view path stays a
    // plain lookAt so projection math is never affected.
    if (bobRoll != 0.0f) {
        glm::quat rollQ = glm::angleAxis(bobRoll, f);
        r = glm::normalize(glm::vec3(rollQ * r));
        u = glm::normalize(glm::vec3(rollQ * u));
    }

    view_ = glm::lookAt(eye, eye + f, u);
    projection_ = glm::perspective(glm::radians(fovY), aspect, nearPlane, farPlane);
    viewProjection_ = projection_ * view_;
}

} // namespace horror
