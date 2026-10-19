#include "scene/CharacterController.hpp"

#include "core/Input.hpp"
#include "render/Camera.hpp"
#include "scene/Scene.hpp"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>

namespace horror {

namespace {
// Slightly stronger than real gravity; FPS games cheat gravity for snappier
// jump arcs. Corridor horror playtests usually land near these values.
constexpr float kGravity = -13.0f;
constexpr float kGroundProbe = 0.05f;  // extra downward probe height for `grounded`
constexpr float kBobFrequency = 9.5f;  // rad/s at full walk speed
constexpr float kBobLateral = 0.04f;   // meters of sideways sway
constexpr float kBobVertical = 0.03f;  // meters of vertical bounce
constexpr float kBobRollAmplitude = 0.01f; // radians

// Overlap test between two AABB centers/extents.
bool overlaps(const glm::vec3 &aCenter, const glm::vec3 &aHalf,
    const glm::vec3 &bMin, const glm::vec3 &bMax)
{
    const glm::vec3 aMin = aCenter - aHalf;
    const glm::vec3 aMax = aCenter + aHalf;
    return glm::all(glm::lessThan(aMin, bMax)) && glm::all(glm::greaterThan(aMax, bMin));
}
} // namespace

void CharacterSystem::step(Scene &scene, EntityId entity, const Hooks &hooks, float dt)
{
    CharacterController *cc = scene.find<CharacterController>(entity);
    if (!cc || !hooks.camera || !hooks.input)
        return;
    Transform &xf = scene.transform(entity);

    // ---- Look (mouse) -------------------------------------------------------
    const glm::vec2 md = hooks.input->mouseDelta() * cc->mouseLookSensitivity;
    hooks.camera->yaw += md.x;
    hooks.camera->pitch -= md.y;
    hooks.camera->clampPitch();

    // ---- Movement intent --------------------------------------------------
    const bool wantSprint = hooks.input->held(Action::Sprint);
    const bool wantCrouch = hooks.input->held(Action::Crouch);
    const bool wasGrounded = cc->grounded;

    float forwardAmt = (hooks.input->held(Action::MoveForward) ? 1.0f : 0.0f)
        - (hooks.input->held(Action::MoveBackward) ? 1.0f : 0.0f);
    float strafeAmt = (hooks.input->held(Action::MoveRight) ? 1.0f : 0.0f)
        - (hooks.input->held(Action::MoveLeft) ? 1.0f : 0.0f);
    if (hooks.input->keyboardWantedByImGui && !hooks.input->mouseCaptured()) {
        // Overlay is open & focused: don't walk while typing in a text field.
        forwardAmt = strafeAmt = 0.0f;
    }

    float targetSpeed = cc->walkSpeed;
    if (wantCrouch)
        targetSpeed = cc->crouchSpeed;
    else if (wantSprint && forwardAmt > 0.5f)
        targetSpeed = cc->sprintSpeed; // no sprint while strafing/backing

    glm::vec3 flatF = hooks.camera->forward();
    flatF.y = 0.0f;
    float fl = glm::length(flatF);
    flatF = fl > 1e-5f ? flatF / fl : glm::vec3(0.0f);
    glm::vec3 wishDir = flatF * forwardAmt + hooks.camera->right() * strafeAmt;
    if (glm::length(wishDir) > 1.0f)
        wishDir = glm::normalize(wishDir); // no diagonally-faster exploit

    float targetHeight = wantCrouch ? cc->crouchHeight : cc->height;

    // ---- Horizontal acceleration (ground vs air control) --------------------
    glm::vec3 horizTarget(wishDir * targetSpeed);
    float accel = cc->acceleration * (cc->grounded ? 1.0f : cc->airControl);
    glm::vec3 horiz(cc->velocity.x, 0.0f, cc->velocity.z);
    horiz = glm::mix(horiz, horizTarget, std::min(accel * dt, 1.0f));
    cc->velocity.x = horiz.x;
    cc->velocity.z = horiz.z;

    // Jump is edge-triggered; holding space doesn't auto-bounce.
    if (hooks.input->pressed(Action::Jump) && cc->grounded && !wantCrouch)
        cc->velocity.y = cc->jumpSpeed;

    cc->velocity.y += kGravity * dt;

    // ---- Integrate + resolve, one axis at a time ------------------------------
    // Axis-separated AABB resolution (X, then Z, then Y): the classic approach
    // that Quake-descended engines use. Y goes last so `grounded` reflects the
    // final resting state. No sub-stepping for tunneling: worst-case per-step
    // travel (~sprint 4.6m/s / 60Hz steps = 7.7cm) is under the thinnest wall
    // we'll build, and fixed timestep keeps that bound true.
    const glm::vec3 feet0 = xf.position; // feet origin
    float px = feet0.x, py = feet0.y, pz = feet0.z;

    // Player box is (radius x height/2 x radius), centered halfH above feet.
    // Recomputed per axis because a resolution on one axis changes the others.
    auto playerCenter = [&] {
        const float halfH = cc->currentHeight * 0.5f;
        return glm::vec3(px, py + halfH, pz);
    };
    auto playerHalf = [&] {
        return glm::vec3(cc->radius, cc->currentHeight * 0.5f, cc->radius);
    };

    // Resolve overlap along one axis, pushing out opposite to `dir` (the
    // movement direction on that axis). `dir == 0` means no motion along the
    // axis, so an overlap there is not this axis' blocker -- pushing out would
    // teleport the player to the far side of a large box (e.g. the floor while
    // walking across it).
    auto collideAxis = [&](int axis, float dir) -> bool {
        if (dir == 0.0f)
            return false;
        bool blocked = false;
        scene.view<AABBCollider>([&](EntityId, const AABBCollider &col) {
            if (!col.resolveCollisions)
                return;
            const glm::vec3 center = playerCenter();
            const glm::vec3 half = playerHalf();
            // Cheap reject before computing penetration.
            if (!overlaps(center, half, col.worldMin, col.worldMax))
                return;
            blocked = true;
            // Push fully out along the axis in the direction we were moving.
            const float pen = dir > 0.0f
                ? col.worldMin[axis] - (center[axis] + half[axis])
                : col.worldMax[axis] - (center[axis] - half[axis]);
            if (axis == 0)
                px += pen;
            else if (axis == 1)
                py += pen;
            else
                pz += pen;
        });
        return blocked;
    };

    px += cc->velocity.x * dt;
    if (collideAxis(0, cc->velocity.x))
        cc->velocity.x = 0.0f;
    pz += cc->velocity.z * dt;
    if (collideAxis(2, cc->velocity.z))
        cc->velocity.z = 0.0f;
    py += cc->velocity.y * dt;

    bool blockedY = collideAxis(1, cc->velocity.y);
    cc->grounded = blockedY; // we only get blocked downward while falling
    if (blockedY) {
        // Standing on something: stop falling (and cancel on upward push too,
        // e.g., bonking a ceiling edge with head-hits-floor cases).
        cc->velocity.y = 0.0f;
    } else if (wasGrounded && cc->velocity.y <= 0.0f) {
        // Walked off an edge this step: probe slightly down so tiny numerical
        // separations don't cancel grounded state on flat floors.
        py -= kGroundProbe;
        if (collideAxis(1, -kGroundProbe)) {
            cc->velocity.y = 0.0f;
        } else {
            py += kGroundProbe;
            cc->grounded = false; // genuinely airborne now
        }
    }

    xf.position = glm::vec3(px, py, pz);

    // ---- Crouch height smoothing -----------------------------------------
    cc->crouching = wantCrouch;
    cc->currentHeight = glm::mix(cc->currentHeight, targetHeight, std::min(dt * 10.0f, 1.0f));

    // ---- Head-bob ----------------------------------------------------------
    // Only while walking on ground; air/crouch damps it for readability.
    const float horizSpeed = glm::length(glm::vec2(cc->velocity.x, cc->velocity.z));
    if (cc->grounded && horizSpeed > 0.05f) {
        // Phase advances with speed so cadence feels tied to stride, not time.
        cc->bobPhase += horizSpeed * dt * kBobFrequency;
        float amp = glm::clamp(horizSpeed / cc->sprintSpeed, 0.35f, 1.0f);
        if (cc->crouching)
            amp *= 0.35f;
        // Two-beat pattern: double-frequency bounce + single sway (classic).
        cc->bobOffset.y = -std::abs(std::sin(cc->bobPhase)) * kBobVertical * amp;
        cc->bobOffset.x = std::cos(cc->bobPhase * 0.5f) * kBobLateral * amp;
        cc->bobOffset.z = 0.0f;
        cc->bobRoll = std::sin(cc->bobPhase) * kBobRollAmplitude * amp;
    } else {
        cc->bobOffset = glm::mix(cc->bobOffset, glm::vec3(0.0f), std::min(dt * 6.0f, 1.0f));
        cc->bobRoll = glm::mix(cc->bobRoll, 0.0f, std::min(dt * 6.0f, 1.0f));
    }

    // Camera eye = feet + eye height + bob (bob in local axis space here;
    // camera folds in roll itself -- see Camera::update).
    hooks.camera->position = xf.position + glm::vec3(0.0f, cc->eyeHeight, 0.0f) + cc->bobOffset;
    hooks.camera->bobOffset = cc->bobOffset;
    hooks.camera->bobRoll = cc->bobRoll;

    (void)targetSpeed; // (read above for sprint gate; silence unused warnings on some compilers)
}

} // namespace horror
