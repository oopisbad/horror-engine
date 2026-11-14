#pragma once

#include "scene/Components.hpp"

namespace horror {

class LevelPhysics;

class Scene;
class Input;
class Camera;

// Fixed-timestep character *system* (runs inside the accumulator in
// Application, so `dt` is always exactly FixedStep here -- deterministic and
// stable). Named ...System to avoid colliding with the CharacterController
// *component* in Components.hpp.
//
// The player uses a feet-anchored AABB. The procedural corridor uses axis
// resolution against boxes. Imported levels sweep the same shape through a
// static triangle hierarchy and translating props.
class CharacterSystem {
public:
    struct Hooks {
        Camera *camera = nullptr; // camera to drive (yaw/pitch/bob/eye pos)
        Input *input = nullptr;
        bool logSteps = false;    // --debug-input: one trace line per fixed step
        LevelPhysics *levelPhysics = nullptr;
    };

    // One fixed step. Reads input, integrates + resolves the entity's world
    // transform in-place, updates the Camera eye position & bob state.
    static void step(Scene &scene, EntityId entity, const Hooks &hooks, float dt);
};

} // namespace horror
