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
// Tolerance for "was outside this face before the axis move?" tests. Keeps
// resting contact (feet exactly on a floor top) a valid block without letting
// deep pre-existing overlap through.
constexpr float kContactEps = 1e-4f;
// Largest ledge the controller walks up without jumping (curbs, stair treads).
constexpr float kStepHeight = 0.35f;
// Collision sub-step budget: no collision pass may move the body further than
// this, so a fast fall/run can never cross a collider between two overlap
// tests. 8 cm is above the fixed-step walk stride (4 cm) and keeps the usual
// step to a single pass, but bounds sprint (7.7 cm) and high-speed falls.
constexpr float kMaxSubStep = 0.08f;
constexpr int kMaxSubSteps = 8;
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
    // Deltas are accumulated by Input and *consumed* here, so every pixel is
    // applied exactly once per fixed step no matter whether a frame ran zero,
    // one or several steps (reading a per-frame delta in the fixed step made
    // look speed depend on the frame rate and double-applied it on catch-up).
    const glm::vec2 rawMouse = hooks.input->consumeMouseDelta();
    const glm::vec2 md = rawMouse * cc->mouseLookSensitivity;
    // Screen convention (GLFW: +y is down the screen):
    //   mouse right -> turn right, mouse up -> look up.
    // Camera::forward() is (-sin yaw, *, -cos yaw), so *increasing* yaw turns
    // left; turning right therefore subtracts. Pitch is up-positive.
    hooks.camera->yaw -= md.x;
    hooks.camera->pitch += cc->invertY ? md.y : -md.y;
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
    glm::vec3 pos = feet0;

    // Player box is (radius x height/2 x radius), centered halfH above feet.
    // Recomputed per collision because a resolution on one axis changes the
    // others.
    auto playerCenter = [&] {
        const float halfH = cc->currentHeight * 0.5f;
        return glm::vec3(pos.x, pos.y + halfH, pos.z);
    };
    auto playerHalf = [&] {
        return glm::vec3(cc->radius, cc->currentHeight * 0.5f, cc->radius);
    };

    glm::vec3 pushTotal(0.0f);

    // True if the body (at its current pos) overlaps any solid collider.
    auto overlapsAny = [&]() -> bool {
        bool hit = false;
        scene.view<AABBCollider>([&](EntityId, const AABBCollider &col) {
            if (hit || !col.resolveCollisions)
                return;
            if (overlaps(playerCenter(), playerHalf(), col.worldMin, col.worldMax))
                hit = true;
        });
        return hit;
    };

    // Swept, axis-separated resolution.
    //
    // Only a face the player *newly crossed* on this axis may block: before
    // applying the move we test whether the body was already overlapping the
    // collider on this axis. That test is what keeps standing-on-the-floor
    // from teleporting: while walking, the body overlaps the floor slab on Y
    // but was already inside the slab's X/Z extent, so X/Z resolution ignores
    // it (the old code pushed out of the nearest face instead, which is ~8 m
    // away for the 16x16 floor -- the "WASD teleports me" bug).
    //
    // The push-out target is gathered across every blocking collider and the
    // extreme one applied once: with several stacked surfaces the result must
    // be order-independent (landing on the highest of two steps, not on
    // whichever entity the unordered_map happened to visit last).
    auto collideAxis = [&](int axis, float delta) -> bool {
        if (delta == 0.0f)
            return false;
        const glm::vec3 center = playerCenter();
        const glm::vec3 half = playerHalf();
        const float offset = center[axis] - pos[axis]; // 0 on X/Z, half height on Y
        const float before = center[axis] - delta;    // pre-move centre on this axis
        bool blocked = false;
        float best = pos[axis];
        scene.view<AABBCollider>([&](EntityId, const AABBCollider &col) {
            if (!col.resolveCollisions)
                return;
            // Cheap reject before computing penetration.
            if (!overlaps(center, half, col.worldMin, col.worldMax))
                return;
            float target;
            if (delta > 0.0f) {
                if (before + half[axis] > col.worldMin[axis] + kContactEps)
                    return; // already inside this extent: not this axis' blocker
                target = col.worldMin[axis] - half[axis] - offset;
                best = blocked ? std::min(best, target) : target;
            } else {
                if (before - half[axis] < col.worldMax[axis] - kContactEps)
                    return;
                target = col.worldMax[axis] + half[axis] - offset;
                best = blocked ? std::max(best, target) : target;
            }
            blocked = true;
        });
        if (blocked) {
            pushTotal[axis] += best - pos[axis];
            pos[axis] = best;
        }
        return blocked;
    };

    // Horizontal move with step-offset: when a grounded body is blocked by a
    // low ledge, retry the same move one step-height up, then settle back down
    // onto the surface. Falls back to the plain blocked position (pressed
    // against the face) if there is no headroom or no surface to land on, so
    // real walls still stop the player.
    auto tryHorizontal = [&](int axis, float delta) -> bool {
        if (delta == 0.0f)
            return false;
        const glm::vec3 start = pos;
        pos[axis] += delta;
        if (!collideAxis(axis, delta))
            return false;
        // Where the plain resolution left us (pressed against the face). Keep
        // this for every failed step-up path so the body rests on the wall
        // instead of a sub-step short of it.
        const glm::vec3 blockedPos = pos;
        if (!cc->grounded) {
            return true; // no mid-air step-ups
        }

        pos = start;
        pos.y += kStepHeight;
        if (overlapsAny()) { // too low a ceiling to lift the body
            pos = blockedPos;
            return true;
        }
        pos[axis] += delta;
        if (collideAxis(axis, delta)) { // still walled in at step height
            pos = blockedPos;
            return true;
        }
        const float raised = pos.y;
        const float drop = kStepHeight + kGroundProbe;
        pos.y -= drop;
        if (collideAxis(1, -drop))
            return false; // stepped up and settled on the ledge
        pos.y = raised; // nothing to stand on: don't levitate
        pos = blockedPos;
        return true;
    };

    // ---- Sub-step integration -------------------------------------------------
    // The whole move is split so no collision pass advances the body further
    // than kMaxSubStep. Worth it for fast falls and for authored colliders
    // thinner than one fixed-step stride; normal walking still runs one pass.
    const float travel = glm::length(cc->velocity) * dt;
    const int substeps = std::clamp(
        static_cast<int>(std::ceil(travel / kMaxSubStep)), 1, kMaxSubSteps);
    const float h = dt / static_cast<float>(substeps);

    bool groundedThisStep = false;
    for (int sub = 0; sub < substeps; ++sub) {
        const float dx = cc->velocity.x * h;
        const float dy = cc->velocity.y * h;
        const float dz = cc->velocity.z * h;

        if (tryHorizontal(0, dx))
            cc->velocity.x = 0.0f;
        if (tryHorizontal(2, dz))
            cc->velocity.z = 0.0f;

        pos.y += dy;
        if (collideAxis(1, dy)) {
            // Ground only when we were moving down; a ceiling hit must not
            // make us think we can jump.
            if (dy <= 0.0f)
                groundedThisStep = true;
            cc->velocity.y = 0.0f;
        }
    }

    cc->grounded = groundedThisStep;
    if (!groundedThisStep && wasGrounded && cc->velocity.y <= 0.0f) {
        // Walked off an edge this step: probe slightly down so tiny numerical
        // separations don't cancel grounded state on flat floors.
        const float beforeProbe = pos.y;
        pos.y -= kGroundProbe;
        if (collideAxis(1, -kGroundProbe)) {
            cc->velocity.y = 0.0f;
            cc->grounded = true;
        } else {
            pos.y = beforeProbe;
        }
    }

    xf.position = pos;
    cc->lastPush = pushTotal;

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

    if (hooks.logSteps) {
        HOR_LOG_INFO(
            "[step] pos=({:.3f},{:.3f},{:.3f}) vel=({:.3f},{:.3f},{:.3f}) "
            "grounded={} in=(f{:+.0f} s{:+.0f} sprint={} crouch={}) "
            "mouse=({:+.1f},{:+.1f}) yaw={:.3f} pitch={:.3f} "
            "push=({:+.4f},{:+.4f},{:+.4f})",
            pos.x, pos.y, pos.z, cc->velocity.x, cc->velocity.y, cc->velocity.z,
            cc->grounded, forwardAmt, strafeAmt, wantSprint, wantCrouch,
            rawMouse.x, rawMouse.y, hooks.camera->yaw, hooks.camera->pitch,
            pushTotal.x, pushTotal.y, pushTotal.z);
    }

    (void)targetSpeed; // (read above for sprint gate; silence unused warnings on some compilers)
}

} // namespace horror
