#pragma once

#include "anim/Animator.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace horror {

class Mesh;
struct Material;

// Entity ids are dense, stable handles. No recycling yet (fine for the scene
// sizes a first pass has); the id's index doubles as the transform slot.
using EntityId = uint32_t;
static constexpr EntityId kInvalidEntity = 0xFFFFFFFFu;

// Every entity has exactly one Transform. Position is the local origin; the
// world matrix is derived on demand and cached against a generation counter
// so hierarchy edits mark descendants dirty without recomputing everything.
struct Transform {
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};

    EntityId parent = kInvalidEntity;
    std::vector<EntityId> children; // simple vector-children: fine at this scale

    glm::mat4 localMatrix() const;
    static glm::mat4 compose(const glm::mat4 &parentWorld, const Transform &child);
};

// Renderable: which mesh, which material params.
struct MeshRenderer {
    const Mesh *mesh = nullptr;             // non-owning; meshes are pooled elsewhere
    const Material *material = nullptr;     // non-owning; owned by Scene/materials pool
    bool castShadow = true;
    bool visible = true;
};

// Renderable whose vertices are deformed by the Animator on the same entity.
// The entity's transform acts as the model matrix on top of the skin.
struct SkinnedMeshRenderer {
    const Mesh *mesh = nullptr;
    const Material *material = nullptr;
    bool castShadow = true;
    bool visible = true;
};

// ---- level conventions (glTF node names) -----------------------------------

// SPAWN_ node: a marker where gameplay can place the player/AI.
struct SpawnPoint {
    std::string tag;                      // "player", "enemy", ...
    glm::vec3 forward{0.0f, 0.0f, -1.0f}; // world-space facing (node -Z)
};

// TRIGGER_ node: AABB volume (authored from the node's mesh bounds or extras).
struct TriggerVolume {
    std::string tag;
    glm::vec3 worldMin{0.0f};
    glm::vec3 worldMax{0.0f};
    bool once = false;
    bool fired = false;
};

// glTF node extras: a flat key/value string map (numbers/bools stringified).
struct EntityExtras {
    std::unordered_map<std::string, std::string> values;
};

// Point light: position comes from its transform; range/falloff below.
// Intensity is candela-ish (see SpotLight note); dozens for room-scale glow.
struct PointLight {
    glm::vec3 color{1.0f};
    float intensity = 10.0f;
    float radius = 0.35f;     // gizmo radius in debug draw
    float range = 8.0f;       // distance falloff bound
    bool castShadow = false;  // stub for later; only the flashlight casts now
};

// Spot light (the flashlight). Attached to the camera transform by gameplay;
// the shadow map is produced from this light's world orientation.
// KHR_lights_punctual spot lights import into this same component.
// Intensities are candela-ish: meter-scale scenes need tens, not units.
struct SpotLight {
    glm::vec3 color{1.0f};
    float intensity = 8.0f;
    // Half-angle of the inner/outer cone in degrees; smoothstep between them
    // gives a soft falloff edge rather than a hard cut.
    float innerConeDeg = 18.0f;
    float outerConeDeg = 24.0f;
    float range = 40.0f;
    bool castsShadows = true;

    // Runtime state the renderer manages for its shadow map; not authored data.
    uint32_t shadowMapSize = 1024;
};

// Axis-aligned box in local space (center offset + half-extents). World-space
// AABB is derived by transforming center and expanding by |scale|.
struct AABBCollider {
    glm::vec3 center{0.0f};
    glm::vec3 halfExtents{0.5f};

    // Expanded bounds after transform; the collision code lives in CController.
    glm::vec3 worldMin{0.0f};
    glm::vec3 worldMax{0.0f};
    bool resolveCollisions = true; // static geometry: player collides, boxes fall
};

// First-person character controller component (see CharacterController.cpp
// for the step logic). Position is at the character's *feet*, eye height is
// an authored offset above it; the camera is placed at feet + eye + bob.
struct CharacterController {
    float height = 1.75f;
    float crouchHeight = 1.05f;
    float eyeHeight = 1.62f;
    float radius = 0.28f;

    float walkSpeed = 2.4f;
    float sprintSpeed = 4.6f;
    float crouchSpeed = 1.2f;

    float acceleration = 22.0f;
    float airControl = 0.15f;
    float jumpSpeed = 5.5f;

    float mouseLookSensitivity = 0.0022f; // radians of yaw per pixel

    // Runtime state (not authored).
    glm::vec3 velocity{0.0f};
    bool grounded = false;
    bool crouching = false;
    float currentHeight = 1.75f;   // smoothly lerps toward target height
    float bobPhase = 0.0f;         // walk-cycle accumulator
    glm::vec3 bobOffset{0.0f};     // current head-bob offset for the camera
    float bobRoll = 0.0f;
};

} // namespace horror
