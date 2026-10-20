#pragma once

#include "scene/Components.hpp"

namespace horror {

class Scene;
class Input;
class Camera;

// Fixed-timestep character *system* (runs inside the accumulator in
// Application, so `dt` is always exactly FixedStep here -- deterministic and
// stable). Named ...System to avoid colliding with the CharacterController
// *component* in Components.hpp.
//
// Movement model: capsule-feet approximation. The player body is an AABB
// standing on the transform's foot point; the camera is authored at feet +
// (eyeHeight + bob). Collisions are axis-resolved AABB sweeps against the
// scene's static AABBCollider boxes -- simple and stable for a corridor
// horror game; replace with a full physics library (Bullet/Jolt) before we
// need ragdolls, moving platforms, or fancy penetration resolution.
class CharacterSystem {
public:
    struct Hooks {
        Camera *camera = nullptr; // camera to drive (yaw/pitch/bob/eye pos)
        Input *input = nullptr;
        bool logSteps = false;    // --debug-input: one trace line per fixed step
    };

    // One fixed step. Reads input, integrates + resolves the entity's world
    // transform in-place, updates the Camera eye position & bob state.
    static void step(Scene &scene, EntityId entity, const Hooks &hooks, float dt);
};

} // namespace horror
