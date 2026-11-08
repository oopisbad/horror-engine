#include "game/Frontend.hpp"
#include "anim/Animator.hpp"
#include "anim/SkinDebug.hpp"
#include "asset/GltfAsset.hpp"
#include "asset/LevelBuilder.hpp"
#include "core/Application.hpp"
#include "core/Log.hpp"
#include "debug/FlightRecorder.hpp"
#include "render/Material.hpp"
#include "render/Mesh.hpp"
#include "scene/CharacterController.hpp"
#include "scene/Components.hpp"
#include "scene/Scene.hpp"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace horror;

// Top face of the corridor floor slab (local half-thickness 0.05, rotated flat).
constexpr float kFloorTopY = 0.05f;

// Distance along the view ray the flashlight beam is aimed at. Far enough that
// the hand offset costs no visible cone shift at room scale, close enough to
// stay well inside the geometric error of a parallax-free aim.
constexpr float kFlashlightAimDistance = 12.0f;

// ---- test-scene construction -------------------------------------------------
// Procedural placeholders only (no assets yet): a dark test corridor, some
// boxes to prove collision, two point lights, camera + flashlight. Every
// mesh instance built here lives in Scene's pool so RAII cleans up in order.
void buildTestScene(Application &app)
{
    Scene &scene = app.scene();

    // Mesh pool: primitives made once and shared by every instance.
    Mesh *cube = scene.addMesh(std::make_unique<Mesh>(Mesh::makeCube(0.5f, "cube")));
    // Half-extent 12 (24x24) so the slab reaches under the far wall at z=-10.5;
    // the old 16x16 floor ended at z=-8 and the player fell off the level just
    // before the corridor end. Mesh and collider share the same constant.
    constexpr float kFloorHalfExtent = 12.0f;
    Mesh *floor = scene.addMesh(std::make_unique<Mesh>(Mesh::makePlane(kFloorHalfExtent, 4, "floor")));
    Mesh *ball = scene.addMesh(std::make_unique<Mesh>(Mesh::makeSphere(0.3f, "ball")));

    // A handy material-factory lambda to keep call sites terse.
    auto mat = [&](const glm::vec3 &albedo, float rough = 0.85f) {
        Material *m = scene.createMaterial();
        m->albedo = albedo;
        m->roughness = rough;
        return m;
    };

    // Floor: XZ plane, normal +Y (the plane builder is authored in XY for
    // reuse as a wall later). -90 deg around X maps the plane's local +Z
    // normal to world +Y; +90 would point it down and back-face cull it.
    EntityId f = scene.create("floor");
    scene.add(f, MeshRenderer{ floor, mat({0.28f, 0.27f, 0.25f}, 0.95f), true, true });
    {
        Transform &t = scene.transform(f);
        t.position = { 0.0f, 0.0f, 0.0f };
        t.rotation = glm::quat(glm::vec3(glm::radians(-90.0f), 0.0f, 0.0f));
        // Half-extents are authored in the entity's local space, so the 0.05m
        // slab thickness is local Z (which the rotation maps to world Y).
        scene.add(f, AABBCollider{ glm::vec3(0.0f),
            glm::vec3(kFloorHalfExtent, kFloorHalfExtent, 0.05f), {}, {}, true });
    }

    // Corridor walls: long thin boxes; doubles as collision geometry.
    // The collider is authored in the entity's LOCAL space, so its half-extents
    // are the unit cube's half size (0.5) and the transform's scale turns them
    // into the world `half`. Passing `half` here would double-apply the scale
    // (collision boxes were scale² too small/thin -- walls 4 cm thick).
    constexpr glm::vec3 kUnitCubeHalf(0.5f);
    const auto wall = [&](const char *name, const glm::vec3 &pos, const glm::vec3 &half, const glm::vec3 &albedo) {
        EntityId e = scene.create(name);
        scene.add(e, MeshRenderer{ cube, mat(albedo, 0.9f), true, true });
        Transform &t = scene.transform(e);
        t.position = pos;
        t.scale = half * 2.0f; // cube is unit-space, so scale == full extents
        scene.add(e, AABBCollider{ glm::vec3(0.0f), kUnitCubeHalf, {}, {}, true });
        return e;
    };

    // Corridor along -Z, width ~3m: left, right, far end.
    wall("wall-left",  { -1.75f, 1.5f, -4.0f }, { 0.1f, 1.6f, 6.0f }, { 0.4f, 0.38f, 0.42f });
    wall("wall-right", {  1.75f, 1.5f, -4.0f }, { 0.1f, 1.6f, 6.0f }, { 0.4f, 0.38f, 0.42f });
    wall("wall-front", {  0.0f,  1.5f, -10.4f }, { 1.9f, 1.6f, 0.1f }, { 0.5f, 0.18f, 0.18f });
    // back wall behind spawn (blocks walking into the void)
    wall("wall-back",  {  0.0f,  1.5f,  3.0f }, { 1.9f, 1.6f, 0.1f }, { 0.45f, 0.30f, 0.50f });

    // A few scattered crates/cubes: prove flashlight shadows + collision.
    const auto crate = [&](const char *name, const glm::vec3 &pos, float scale, const glm::vec3 &albedo) {
        EntityId e = scene.create(name);
        scene.add(e, MeshRenderer{ cube, mat(albedo, 0.75f), true, true });
        scene.transform(e).position = pos;
        scene.transform(e).scale = glm::vec3(scale);
        // Local half of the unit cube; the transform scale supplies the size.
        scene.add(e, AABBCollider{ glm::vec3(0.0f), kUnitCubeHalf, {}, {}, true });
        return e;
    };
    crate("crate-1", { 0.9f, 0.25f, -3.2f }, 0.5f, { 0.45f, 0.32f, 0.20f });
    crate("crate-2", { 1.3f, 0.25f, -5.5f }, 0.5f, { 0.30f, 0.45f, 0.35f });
    crate("crate-3", { -1.2f, 0.5f, -7.2f }, 1.0f, { 0.55f, 0.40f, 0.25f });
    // Tall pillar in the flashlight beam: makes the PCF shadow on the floor
    // and far wall obvious at a glance when running the scene.
    wall("pillar", { -0.95f, 1.0f, -4.6f }, { 0.25f, 1.0f, 0.25f }, { 0.36f, 0.34f, 0.37f });

    // The red sphere -- "the red circle on the floor" from the bug reports.
    // 0.3 m radius at y=0.3 (resting on the slab). It used to be render-only,
    // so you walked straight through it; it is now solid (AABB, 0.6 m cube:
    // taller than the 0.35 m step offset, so you have to jump onto it) and it
    // carries a TriggerVolume that fires on entry.
    {
        EntityId e = scene.create("red-ball");
        scene.add(e, MeshRenderer{ ball, mat({ 0.7f, 0.1f, 0.1f }, 0.4f), true, true });
        scene.transform(e).position = { -0.8f, 0.3f, -1.5f };
        const glm::vec3 c = scene.transform(e).position;
        const glm::vec3 half = ball->maxBounds(); // 0.3; entity scale is 1
        // Collider half-extents are local, and the transform scale is 1, so
        // the mesh bounds are already the local half extents.
        scene.add(e, AABBCollider{ glm::vec3(0.0f), half, {}, {}, true });

        TriggerVolume trigger;
        trigger.tag = "red-ball";
        // Bigger than the ball so *touching* it counts (the player stops with
        // their body box against the face, where a strict AABB test would not
        // overlap), and taller so standing on top still counts.
        const glm::vec3 reach = half + glm::vec3(0.30f, 0.0f, 0.30f);
        trigger.worldMin = c - reach;
        trigger.worldMax = c + reach + glm::vec3(0.0f, 0.60f, 0.0f);
        scene.add(e, std::move(trigger));
    }

    // Point lights: warm key light in the corridor, red accent in the far end.
    {
        EntityId e = scene.create("light-warm");
        scene.transform(e).position = { 1.4f, 2.4f, -6.0f };
        scene.add(e, PointLight{ glm::vec3(1.0f, 0.85f, 0.6f), 16.0f, 0.25f, 9.0f, false });
    }
    {
        EntityId e = scene.create("light-red");
        scene.transform(e).position = { -1.0f, 1.0f, -9.6f };
        scene.add(e, PointLight{ glm::vec3(1.0f, 0.15f, 0.1f), 10.0f, 0.2f, 6.0f, false });
    }

    // Player: feet on the floor slab's top face so the spawn is not inside
    // collider geometry (the controller overwrites the camera position from
    // the feet every fixed step, so this is the only spawn that matters).
    EntityId player = scene.create("player");
    scene.add(player, CharacterController{});
    scene.transform(player).position = { 0.0f, kFloorTopY, 1.8f };
    app.camera().fovY = 75.0f;
    app.camera().position = { 0.0f, kFloorTopY + 1.62f, 1.8f };
    app.camera().yaw = 0.0f;   // looking down -Z into the corridor
    app.camera().pitch = -0.05f;

    // Flashlight entity: cone params come from the SpotLight component;
    // position/orientation are synced to the camera every fixed step below.
    EntityId flashlight = scene.create("flashlight");
    scene.add(flashlight, SpotLight{});
}

// World-space AABB of every rest-pose mesh under a level instance, taken from
// the scene's evaluated hierarchy so node transforms (CesiumMan's Z-up
// conversion nodes), the root yaw and the root scale (Fox: 0.012) all apply.
// Returns false when the instance has no renderable meshes.
bool levelWorldBounds(Scene &scene, const LevelInstance &level,
    glm::vec3 &outMin, glm::vec3 &outMax)
{
    outMin = glm::vec3(std::numeric_limits<float>::max());
    outMax = glm::vec3(std::numeric_limits<float>::lowest());
    bool any = false;
    const auto accumulate = [&](EntityId id, const Mesh *mesh) {
        if (!mesh)
            return;
        const glm::mat4 world = scene.worldMatrix(id);
        const glm::vec3 mn = mesh->minBounds();
        const glm::vec3 mx = mesh->maxBounds();
        for (int corner = 0; corner < 8; ++corner) {
            const glm::vec3 local((corner & 1) ? mx.x : mn.x,
                (corner & 2) ? mx.y : mn.y,
                (corner & 4) ? mx.z : mn.z);
            const glm::vec3 p = glm::vec3(world * glm::vec4(local, 1.0f));
            outMin = glm::min(outMin, p);
            outMax = glm::max(outMax, p);
            any = true;
        }
    };
    for (EntityId id : level.meshEntities) {
        if (const MeshRenderer *mr = scene.find<MeshRenderer>(id))
            accumulate(id, mr->mesh);
        else if (const SkinnedMeshRenderer *sr = scene.find<SkinnedMeshRenderer>(id))
            accumulate(id, sr->mesh);
    }
    return any;
}

// Sampling density for animation-aware placement and its self-test. Kept in
// one constant so the check and the snap measure exactly the same poses.
constexpr int levelSkinSamples = 24;

// Lowest world Y of a level across its *animated* poses, not just the rest
// pose. A walk clip lowers the planted foot ~1 cm below the bind pose (the
// hips bob and the ankle rotates), so snapping from the rest pose lets the
// feet poke through the floor during the cycle. Skinned meshes are CPU-skinned
// at a handful of clip times here; static meshes use their rest-pose corners.
float levelLowestAnimatedY(Scene &scene, const LevelInstance &level, float &restY)
{
    restY = 0.0f;
    glm::vec3 mn(0.0f), mx(0.0f);
    if (!levelWorldBounds(scene, level, mn, mx))
        return 0.0f;
    restY = mn.y;
    float lowest = mn.y;
    for (EntityId id : level.meshEntities) {
        if (!scene.find<SkinnedMeshRenderer>(id))
            continue;
        // Dense enough to catch the deepest foot dip between keyframes; only
        // runs at spawn, so 24 poses of a few thousand vertices is free.
        const std::vector<SkinSample> samples = sampleSkin(scene, id, levelSkinSamples);
        for (const SkinSample &s : samples)
            lowest = std::min(lowest, s.lowestY);
    }
    return lowest;
}

// Feet on the floor: offsets the level ROOT (in world/parent space) so the
// lowest vertex sits exactly on `floorY` -- rest pose AND every sampled pose
// of the playing clip. An authored spawn height alone is not enough: it
// assumes each model's origin is at its feet, which is exactly the assumption
// that sank both samples once the root transform was lost (NOTES bug 20).
// CesiumMan's mesh lives under Z-up conversion nodes and the Fox is authored in
// centimeters with a 0.012 root scale, so the evaluated hierarchy is the only
// reliable source for the offset.
float snapLevelToFloor(Scene &scene, LevelInstance &level, float floorY)
{
    if (!scene.valid(level.root)) {
        HOR_LOG_WARN("floor snap: level instance has no root");
        return 0.0f;
    }
    scene.updateWorldMatrices();
    glm::vec3 mn(0.0f), mx(0.0f);
    if (!levelWorldBounds(scene, level, mn, mx)) {
        HOR_LOG_WARN("floor snap: level '{}' has no mesh bounds", scene.name(level.root));
        return 0.0f;
    }
    float restY = mn.y;
    const float lowest = levelLowestAnimatedY(scene, level, restY);
    const float lift = floorY - lowest;
    scene.transform(level.root).position.y += lift;
    scene.updateWorldMatrices();
    HOR_LOG_INFO(
        "level '{}': rest bbox y [{:.3f}, {:.3f}] x [{:.3f}, {:.3f}] animated lowest {:.3f} "
        "(rest {:.3f}) -> lifted {:.3f} m onto floor {:.3f}",
        scene.name(level.root), mn.y, mx.y, mn.x, mx.x, lowest, restY, lift, floorY);
    return lift;
}

// Loads a glTF scene, starts a clip on every Animator it created and puts the
// rest-pose feet on the corridor floor. `preferredClip` falls back to the
// first clip when not present.
void spawnGltfLevel(Application &app, const char *path, const glm::vec3 &position,
    float yawDegrees, LevelInstance &instance, const char *preferredClip = nullptr,
    float scale = 1.0f, float floorY = kFloorTopY)
{
    auto model = app.assets().loadModel(path);
    if (!model || !model->valid()) {
        HOR_LOG_WARN("test model '{}' unavailable -- skipping", path);
        return;
    }

    Transform transform;
    transform.position = position;
    transform.rotation = glm::angleAxis(glm::radians(yawDegrees), glm::vec3(0, 1, 0));
    transform.scale = glm::vec3(scale);
    instance = LevelBuilder::instantiate(app.scene(), model, transform);

    app.scene().view<Animator>([&](EntityId, Animator &animator) {
        if (animator.currentClip >= 0 || !animator.clips || animator.clips->empty())
            return;
        int index = preferredClip ? animator.findClip(preferredClip) : 0;
        if (index < 0)
            index = 0;
        animator.playIndex(index, 0.0f, true);
    });

    // Placement is bounds-driven: the authored spawn height is only a hint.
    snapLevelToFloor(app.scene(), instance, floorY);
}

// Puts the flashlight on the camera: origin at a hand offset, beam aimed at the
// crosshair (see the comment at the call site). Shared by the fixed step and
// the self-test so the test exercises the real aim math.
void syncFlashlight(Application &app, EntityId flashlight)
{
    Camera &cam = app.camera();
    const glm::vec3 lightOrigin = cam.position + cam.right() * 0.12f - cam.up() * 0.10f;
    Transform &light = app.scene().transform(flashlight);
    light.position = lightOrigin;
    // Aim the *beam* at the crosshair instead of parallel to the eye axis: the
    // hand offset would otherwise push the bright spot sideways by
    // atan(0.12 / distance) -- invisible down a corridor, ~20 degrees off on a
    // wall 30 cm away ("the flashlight gets off-centred near walls").
    const glm::vec3 aimPoint = cam.position + cam.forward() * kFlashlightAimDistance;
    light.rotation = glm::quatLookAt(glm::normalize(aimPoint - lightOrigin), cam.up());
}

// ---- gameplay systems --------------------------------------------------------

// Fires when the player's body box enters a TriggerVolume. Edge-triggered on
// entry (so standing inside does not spam) and cheap: one AABB test per
// trigger per fixed step.
void updateTriggers(Application &app, EntityId player)
{
    Scene &scene = app.scene();
    const CharacterController *cc = scene.find<CharacterController>(player);
    if (!cc)
        return;
    const glm::vec3 feet = scene.transform(player).position;
    const glm::vec3 half(cc->radius, cc->currentHeight * 0.5f, cc->radius);
    const glm::vec3 center = feet + glm::vec3(0.0f, half.y, 0.0f);
    scene.view<TriggerVolume>([&](EntityId, TriggerVolume &trigger) {
        const bool inside = glm::all(glm::lessThan(center - half, trigger.worldMax))
            && glm::all(glm::greaterThan(center + half, trigger.worldMin));
        if (inside && !trigger.playerInside) {
            ++trigger.enterCount;
            trigger.playerInside = true;
            trigger.fired = true;
            HOR_LOG_INFO("[trigger] entered '{}' ({}x)", trigger.tag, trigger.enterCount);
        } else if (!inside && trigger.playerInside) {
            trigger.playerInside = false;
            HOR_LOG_DEBUG("[trigger] left '{}'", trigger.tag);
        }
    });
}

// One patrol-able level root: the sample clips are in-place (no root motion),
// so walking is only visible if gameplay translates the root. The path is a
// straight line along the model's own facing, oscillating +/- `amplitude`;
// phase 0 is the authored position, so enabling patrol never teleports it.
struct PatrolUnit {
    EntityId root = 0;
    glm::vec3 basePos{0.0f};
    glm::vec3 dir{0.0f, 0.0f, -1.0f};
    float baseYaw = 0.0f;
    float amplitude = 0.5f;
    float speed = 0.5f; // m/s along the path
    float phase = 0.0f;
};

void updatePatrol(Scene &scene, std::vector<PatrolUnit> &units, float dt, bool enabled)
{
    if (!enabled || dt <= 0.0f)
        return;
    for (PatrolUnit &unit : units) {
        if (!scene.valid(unit.root))
            continue;
        unit.phase += (unit.speed / std::max(unit.amplitude, 0.05f)) * dt;
        const float offset = unit.amplitude * std::sin(unit.phase);
        // Travel reverses at the extremes, where the speed is ~0: flip the
        // facing there so the model turns around instead of moonwalking.
        const float travel = std::cos(unit.phase) >= 0.0f ? 1.0f : -1.0f;
        Transform &t = scene.transform(unit.root);
        t.position = unit.basePos + unit.dir * offset;
        t.rotation = glm::angleAxis(unit.baseYaw + (travel > 0.0f ? 0.0f : glm::pi<float>()),
            glm::vec3(0.0f, 1.0f, 0.0f));
    }
}

// --debug-skin: CPU-skin every vertex of a model and compare the foot/head
// vertices against the joint positions the bone debug overlay draws. Prints
// the numeric offset per animation time; this is the data that decides whether
// the mesh and the skeleton really agree (they do not, if the offset swings).
void debugSkinReport(Application &app, const LevelInstance &level, const char *label, int samples)
{
    app.scene().updateWorldMatrices();
    bool any = false;
    for (EntityId id : level.meshEntities) {
        if (!app.scene().find<SkinnedMeshRenderer>(id))
            continue;
        any = true;
        printSkinReport(app.scene(), std::string(label) + ":" + app.scene().name(id), id, samples);
    }
    if (!any)
        HOR_LOG_WARN("--debug-skin: level '{}' has no skinned meshes", label);
}

// ---- scripted input / self-test ---------------------------------------------
// Movement is debugged with data, not by eye: the harness drives the real
// fixed step with synthetic key/mouse state and asserts on positions.

int keyFromName(std::string_view name)
{
    if (name == "W") return GLFW_KEY_W;
    if (name == "A") return GLFW_KEY_A;
    if (name == "S") return GLFW_KEY_S;
    if (name == "D") return GLFW_KEY_D;
    if (name == "SPACE") return GLFW_KEY_SPACE;
    if (name == "SHIFT") return GLFW_KEY_LEFT_SHIFT;
    if (name == "CTRL" || name == "CONTROL") return GLFW_KEY_LEFT_CONTROL;
    if (name == "C") return GLFW_KEY_C;
    return GLFW_KEY_UNKNOWN;
}

// Every key the harness is allowed to drive; cleared each frame unless held.
const std::vector<int> &scriptableKeys()
{
    static const std::vector<int> keys = {
        GLFW_KEY_W, GLFW_KEY_A, GLFW_KEY_S, GLFW_KEY_D,
        GLFW_KEY_SPACE, GLFW_KEY_LEFT_SHIFT, GLFW_KEY_C, GLFW_KEY_LEFT_CONTROL
    };
    return keys;
}

// One simulated frame = roll input edges, apply scripted key state (what
// glfwPollEvents would deliver), then run exactly one fixed step.
struct InputHarness {
    Application &app;
    EntityId player = 0;
    CharacterSystem::Hooks hooks;
    float dt = 1.0f / 60.0f;
    std::vector<int> held;

    void set(std::initializer_list<int> keys) { held.assign(keys.begin(), keys.end()); }
    [[nodiscard]] bool down(int key) const
    {
        return std::find(held.begin(), held.end(), key) != held.end();
    }

    void step(const glm::vec2 &mouse = glm::vec2(0.0f))
    {
        Input &in = app.input();
        in.update();
        for (int key : scriptableKeys())
            in.setScriptedKey(key, down(key));
        if (mouse.x != 0.0f || mouse.y != 0.0f)
            in.injectMouseDelta(mouse);
        CharacterSystem::step(app.scene(), player, hooks, dt);
    }

    void releaseAll()
    {
        held.clear();
        for (int key : scriptableKeys())
            app.input().setScriptedKey(key, false);
    }
};

// Puts the character back on its feet with no momentum and a level camera.
// This is a teleport: the recorder is told so an expected jump never shows up
// as an anomaly in a dump (and the row is marked TELEPORT instead).
void resetCharacter(Application &app, EntityId player, const glm::vec3 &feet)
{
    Scene &scene = app.scene();
    FlightRecorder::get().noteTeleport("resetCharacter");
    noteCameraWriter("resetCharacter");
    notePlayerWriter("resetCharacter");
    scene.transform(player).position = feet;
    if (CharacterController *cc = scene.find<CharacterController>(player)) {
        cc->velocity = glm::vec3(0.0f);
        cc->lastPush = glm::vec3(0.0f);
        cc->jumpBuffer = cc->coyoteTime = 0.0f;
        cc->grounded = false;
        cc->crouching = false;
        cc->currentHeight = cc->height;
        cc->bobPhase = 0.0f;
        cc->bobOffset = glm::vec3(0.0f);
        cc->bobRoll = 0.0f;
        cc->bobAmount = 0.0f;
        cc->bobDriving = false;
    }
    Camera &cam = app.camera();
    cam.position = feet + glm::vec3(0.0f, 1.62f, 0.0f);
    cam.yaw = 0.0f;
    cam.pitch = 0.0f;
    cam.bobOffset = glm::vec3(0.0f);
    cam.bobRoll = 0.0f;
}

// --script "hold W 60; wait 30; mouse 120 0; hold S+SHIFT 60"
void runScript(Application &app, EntityId player, float dt, const std::string &script,
    bool verbose)
{
    InputHarness h{ app, player, {}, dt };
    h.hooks.camera = &app.camera();
    h.hooks.input = &app.input();
    h.hooks.logSteps = verbose;

    const glm::vec3 start = app.scene().transform(player).position;
    std::istringstream lines(script);
    std::string command;
    while (std::getline(lines, command, ';')) {
        std::istringstream words(command);
        std::string verb;
        if (!(words >> verb))
            continue;
        if (verb == "hold") {
            std::string keys;
            int steps = 60;
            if (!(words >> keys)) {
                HOR_LOG_WARN("script: 'hold' needs a key");
                continue;
            }
            if (!(words >> steps))
                steps = 60;
            std::istringstream keyList(keys);
            std::string keyName;
            std::vector<int> pressed;
            while (std::getline(keyList, keyName, '+')) {
                const int key = keyFromName(keyName);
                if (key == GLFW_KEY_UNKNOWN)
                    HOR_LOG_WARN("script: unknown key '{}'", keyName);
                else {
                    h.held.push_back(key);
                    pressed.push_back(key);
                }
            }
            for (int i = 0; i < steps; ++i)
                h.step();
            for (int key : pressed)
                std::erase(h.held, key); // hold = press for N steps only
        } else if (verb == "release") {
            std::string keys;
            if (words >> keys) {
                std::istringstream keyList(keys);
                std::string keyName;
                while (std::getline(keyList, keyName, '+'))
                    std::erase(h.held, keyFromName(keyName));
            }
        } else if (verb == "wait") {
            int steps = 1;
            words >> steps;
            for (int i = 0; i < steps; ++i)
                h.step();
        } else if (verb == "mouse") {
            float dx = 0.0f, dy = 0.0f;
            words >> dx >> dy;
            h.step({ dx, dy });
        } else {
            HOR_LOG_WARN("script: unknown command '{}'", verb);
        }
    }
    h.releaseAll();
    const glm::vec3 end = app.scene().transform(player).position;
    HOR_LOG_INFO("script done: player ({:.3f},{:.3f},{:.3f}) -> ({:.3f},{:.3f},{:.3f})",
        start.x, start.y, start.z, end.x, end.y, end.z);
}

// Scripted movement matrix + model placement checks. Returns the number of
// failed checks.
int runSelfTest(Application &app, EntityId player, float dt, EntityId flashlight,
    const LevelInstance &cesiumMan, const LevelInstance &fox)
{
    Scene &scene = app.scene();
    Camera &cam = app.camera();
    InputHarness h{ app, player, {}, dt };
    h.hooks.camera = &cam;
    h.hooks.input = &app.input();

    // Private platform far from the corridor: flat, wall-free, one second of
    // travel in every direction. Destroyed before the game starts rendering.
    EntityId pad = scene.create("selftest-pad");
    scene.transform(pad).position = { 100.0f, -0.05f, 100.0f };
    scene.add(pad, AABBCollider{ glm::vec3(0.0f), glm::vec3(16.0f, 0.05f, 16.0f), {}, {}, true });
    // 0.2 m ledge (top at y=0.2) for the step-offset case and a 6 cm thick
    // wall for the tunneling check.
    // Placed well beyond the straight-line walk distance of the cases above so
    // they stay unobstructed.
    EntityId ledge = scene.create("selftest-ledge");
    scene.transform(ledge).position = { 100.0f, 0.1f, 92.0f };
    scene.add(ledge, AABBCollider{ glm::vec3(0.0f), glm::vec3(2.0f, 0.1f, 1.0f), {}, {}, true });
    EntityId thinWall = scene.create("selftest-thin-wall");
    scene.transform(thinWall).position = { 100.0f, 1.0f, 86.0f };
    scene.add(thinWall, AABBCollider{ glm::vec3(0.0f), glm::vec3(2.0f, 1.0f, 0.03f), {}, {}, true });
    scene.updateWorldMatrices();
    scene.refreshColliderBounds();

    const glm::vec3 padSpawn{ 100.0f, 0.0f, 100.0f };
    constexpr int kSteps = 60; // one second at the fixed step
    constexpr float kJumpLimit = 0.2f; // no single step may move further than this

    int checked = 0;
    int failed = 0;
    auto report = [&](const char *name, bool pass, const std::string &detail) {
        ++checked;
        if (!pass)
            ++failed;
        HOR_LOG_INFO("[selftest] {:<16} {}  {}", name, pass ? "PASS" : "FAIL", detail);
    };

    struct MoveCase {
        const char *name;
        std::vector<int> keys;
        glm::vec3 dir;
        float speed;
    };
    const glm::vec3 diagonal = glm::normalize(glm::vec3(1.0f, 0.0f, -1.0f));
    const std::vector<MoveCase> cases = {
        { "walk W", { GLFW_KEY_W }, { 0.0f, 0.0f, -1.0f }, 2.4f },
        { "walk S", { GLFW_KEY_S }, { 0.0f, 0.0f, 1.0f }, 2.4f },
        { "walk A", { GLFW_KEY_A }, { -1.0f, 0.0f, 0.0f }, 2.4f },
        { "walk D", { GLFW_KEY_D }, { 1.0f, 0.0f, 0.0f }, 2.4f },
        { "diagonal W+D", { GLFW_KEY_W, GLFW_KEY_D }, diagonal, 2.4f },
        { "sprint W", { GLFW_KEY_W, GLFW_KEY_LEFT_SHIFT }, { 0.0f, 0.0f, -1.0f }, 4.6f },
        { "crouch W", { GLFW_KEY_W, GLFW_KEY_C }, { 0.0f, 0.0f, -1.0f }, 1.2f },
    };

    for (const MoveCase &c : cases) {
        resetCharacter(app, player, padSpawn);
        h.held = c.keys;
        glm::vec3 prev = padSpawn;
        float maxStep = 0.0f;
        for (int i = 0; i < kSteps; ++i) {
            h.step();
            const glm::vec3 p = scene.transform(player).position;
            maxStep = std::max(maxStep, glm::length(p - prev));
            prev = p;
        }
        const glm::vec3 disp = scene.transform(player).position - padSpawn;
        const glm::vec3 flat(disp.x, 0.0f, disp.z);
        const float along = glm::dot(flat, c.dir);
        const float lateral = glm::length(flat - c.dir * along);
        const float expected = c.speed * kSteps * dt; // acceleration ramp costs ~5%
        const CharacterController *cc = scene.find<CharacterController>(player);
        const bool pass = along > expected * 0.85f && along < expected * 1.10f
            && lateral < 0.02f && maxStep < kJumpLimit && std::abs(disp.y) < 0.01f
            && cc && cc->grounded;
        std::ostringstream detail;
        detail << "along=" << along << " expected~" << expected << " lateral=" << lateral
               << " maxStep=" << maxStep << " grounded=" << (cc && cc->grounded);
        report(c.name, pass, detail.str());
    }

    // Idle: this is the regression check for the WASD teleport (standing on the
    // floor used to be resolved as a horizontal collision against a 16 m box).
    {
        resetCharacter(app, player, padSpawn);
        h.held.clear();
        float maxStep = 0.0f;
        glm::vec3 prev = padSpawn;
        for (int i = 0; i < kSteps; ++i) {
            h.step();
            const glm::vec3 p = scene.transform(player).position;
            maxStep = std::max(maxStep, glm::length(p - prev));
            prev = p;
        }
        const glm::vec3 end = scene.transform(player).position;
        const CharacterController *cc = scene.find<CharacterController>(player);
        std::ostringstream detail;
        detail << "drift=" << glm::length(end - padSpawn) << " maxStep=" << maxStep
               << " grounded=" << (cc && cc->grounded);
        report("idle", glm::length(end - padSpawn) < 1e-3f && maxStep < 1e-3f && cc
                && cc->grounded, detail.str());
    }

    // Jump: settle, press for one step, then simulate 1.5 s of air time.
    {
        resetCharacter(app, player, padSpawn);
        h.held.clear();
        for (int i = 0; i < 5; ++i)
            h.step();
        h.held = { GLFW_KEY_SPACE };
        h.step();
        h.held.clear();
        float peak = 0.0f;
        float maxStep = 0.0f;
        glm::vec3 prev = scene.transform(player).position;
        for (int i = 0; i < 90; ++i) {
            h.step();
            const glm::vec3 p = scene.transform(player).position;
            peak = std::max(peak, p.y);
            maxStep = std::max(maxStep, glm::length(p - prev));
            prev = p;
        }
        const CharacterController *cc = scene.find<CharacterController>(player);
        const glm::vec3 end = scene.transform(player).position;
        std::ostringstream detail;
        detail << "peak=" << peak << " landed_y=" << end.y
               << " maxStep=" << maxStep << " grounded=" << (cc && cc->grounded);
        report("jump", peak > 0.8f && peak < 1.6f && maxStep < kJumpLimit
                && std::abs(end.y - padSpawn.y) < 0.01f && cc && cc->grounded,
            detail.str());
    }

    // A complete tap between physics steps must survive render-only frames.
    {
        resetCharacter(app, player, padSpawn);
        h.releaseAll();
        for (int i = 0; i < 5; ++i) h.step();
        app.input().setScriptedKey(GLFW_KEY_SPACE, true);
        app.input().setScriptedKey(GLFW_KEY_SPACE, false);
        app.input().update();
        app.input().update();
        h.step();
        auto *cc = scene.find<CharacterController>(player);
        report("short jump tap", cc && cc->velocity.y > 0.0f, "tap preserved across render-only frames");
        bool bounced = false;
        for (int i = 0; i < 120; ++i) { h.step(); if (i > 70 && cc->velocity.y > 0.0f) bounced = true; }
        report("jump consumed once", !bounced && cc->grounded, "no repeated jump from one tap");
    }
    // Corridor: walk straight into the far wall; must stop at the face and not
    // tunnel through or slide off-axis.
    {
        const glm::vec3 corridorSpawn{ 0.0f, kFloorTopY, 0.0f };
        resetCharacter(app, player, corridorSpawn);
        h.held = { GLFW_KEY_W };
        float maxStep = 0.0f;
        glm::vec3 prev = corridorSpawn;
        for (int i = 0; i < 400; ++i) {
            h.step();
            const glm::vec3 p = scene.transform(player).position;
            maxStep = std::max(maxStep, glm::length(p - prev));
            prev = p;
        }
        const glm::vec3 end = scene.transform(player).position;
        const CharacterController *cc = scene.find<CharacterController>(player);
        std::ostringstream detail;
        detail << "end=(" << end.x << "," << end.y << "," << end.z << ")"
               << " maxStep=" << maxStep << " grounded=" << (cc && cc->grounded);
        report("wall stop", end.z < -9.95f && end.z > -10.1f && std::abs(end.x) < 0.01f
                && maxStep < kJumpLimit && cc && cc->grounded, detail.str());
    }

    // Step offset: a 0.2 m ledge has to be walked onto, not treated as a wall.
    {
        const glm::vec3 approach{ 100.0f, 0.0f, 95.0f };
        resetCharacter(app, player, approach);
        h.held = { GLFW_KEY_W };
        float maxStep = 0.0f;
        glm::vec3 prev = approach;
        for (int i = 0; i < kSteps; ++i) {
            h.step();
            const glm::vec3 p = scene.transform(player).position;
            maxStep = std::max(maxStep, glm::length(p - prev));
            prev = p;
        }
        const glm::vec3 end = scene.transform(player).position;
        const CharacterController *cc = scene.find<CharacterController>(player);
        std::ostringstream detail;
        detail << "end=(" << end.x << "," << end.y << "," << end.z << ")"
               << " maxStep=" << maxStep << " grounded=" << (cc && cc->grounded);
        report("step up 0.2m", std::abs(end.y - 0.2f) < 0.01f && end.z < 93.2f
                && maxStep < 0.35f && cc && cc->grounded, detail.str());
    }

    // Sub-step / sweep: sprint into a 6 cm wall; the body must stop at it.
    {
        const glm::vec3 runUp{ 100.0f, 0.0f, 89.0f };
        resetCharacter(app, player, runUp);
        h.held = { GLFW_KEY_W, GLFW_KEY_LEFT_SHIFT };
        float maxStep = 0.0f;
        glm::vec3 prev = runUp;
        for (int i = 0; i < 200; ++i) {
            h.step();
            const glm::vec3 p = scene.transform(player).position;
            maxStep = std::max(maxStep, glm::length(p - prev));
            prev = p;
        }
        const glm::vec3 end = scene.transform(player).position;
        const CharacterController *cc = scene.find<CharacterController>(player);
        std::ostringstream detail;
        detail << "end=(" << end.x << "," << end.y << "," << end.z << ")"
               << " (wall face 86.03, stop ~86.31) maxStep=" << maxStep
               << " grounded=" << (cc && cc->grounded);
        report("thin wall", end.z > 86.30f && end.z < 86.6f && std::abs(end.y) < 0.01f
                && maxStep < kJumpLimit && cc && cc->grounded, detail.str());
    }

    // Mouse convention: right turns right, up looks up (and invert Y flips it).
    constexpr float kMouseX = 120.0f;
    auto mouseReport = [&](const char *name, bool pass, const glm::vec3 &f, float yaw,
                            float pitch) {
        std::ostringstream detail;
        detail << "forward=(" << f.x << "," << f.y << "," << f.z << ") yaw=" << yaw
               << " pitch=" << pitch;
        report(name, pass, detail.str());
    };
    {
        resetCharacter(app, player, padSpawn);
        h.held.clear();
        h.step({ kMouseX, 0.0f });
        mouseReport("mouse right", cam.forward().x > 0.15f, cam.forward(), cam.yaw, cam.pitch);

        resetCharacter(app, player, padSpawn);
        h.step({ 0.0f, -kMouseX });
        mouseReport("mouse up", cam.forward().y > 0.15f, cam.forward(), cam.yaw, cam.pitch);

        if (CharacterController *cc = scene.find<CharacterController>(player)) {
            cc->invertY = true;
            resetCharacter(app, player, padSpawn);
            h.step({ 0.0f, -kMouseX });
            mouseReport("invert Y", cam.pitch < -0.1f, cam.forward(), cam.yaw, cam.pitch);
            cc->invertY = false;
        }
    }

    // Key edges: update() must snapshot the *previous* frame, not the state
    // this frame's events just wrote (jump, F1, P, O, V and Esc were dead).
    {
        Input &in = app.input();
        in.setScriptedKey(GLFW_KEY_F1, false);
        in.update();                              // frame boundary (no event yet)
        in.setScriptedKey(GLFW_KEY_F1, true);     // press event arrives
        const bool pressEdge = in.pressed(Action::ToggleDebugUI) && in.held(Action::ToggleDebugUI);
        in.update();                              // next frame, key still down
        const bool heldOnly = in.held(Action::ToggleDebugUI) && !in.pressed(Action::ToggleDebugUI);
        in.update();                              // frame boundary, still down
        in.setScriptedKey(GLFW_KEY_F1, false);    // release event arrives
        const bool releaseEdge = in.released(Action::ToggleDebugUI);
        report("key edges", pressEdge && heldOnly && releaseEdge,
            "pressed/held/released = " + std::to_string(pressEdge) + "/"
                + std::to_string(heldOnly) + "/" + std::to_string(releaseEdge));
    }

    // Landing flinch: the camera eye is feet + eye height + head bob, so the
    // only thing that can jump on landing is the bob. It used to restart from
    // a damped-to-zero offset and put up to kBobVertical (3 cm) into a single
    // step the instant a jump landed while walking. Measured here: instant
    // restart = 15.4 mm of extra camera motion on the landing step, ramped
    // amplitude = 2.0 mm (and 5.6 mm at worst over the whole jump, which is
    // just the normal walk bob at 2.4 m/s). Threshold 10 mm separates them.
    {
        // Run it at the red ball, i.e. the spot from the bug report ("jump
        // while standing on the circle and land"). The ball is solid now, so
        // this spawn point sits inside its collider and walks out through it
        // (only entering a face is blocked); the flinch measured here is the
        // head bob restarting, not the prop.
        resetCharacter(app, player, { -0.8f, kFloorTopY, -1.3f });
        h.held = { GLFW_KEY_W };
        for (int i = 0; i < 30; ++i) // settle into a walk; bob is driven
            h.step();
        h.held = { GLFW_KEY_W, GLFW_KEY_SPACE };
        h.step(); // jump
        h.held = { GLFW_KEY_W };

        float maxExtra = 0.0f;
        float atLanding = 0.0f;
        float prevCamY = cam.position.y;
        float prevFeetY = scene.transform(player).position.y;
        bool airborne = true;
        for (int i = 0; i < 90; ++i) {
            h.step();
            const CharacterController *cc = scene.find<CharacterController>(player);
            const float camY = cam.position.y;
            const float feetY = scene.transform(player).position.y;
            const float extra = std::abs((camY - prevCamY) - (feetY - prevFeetY));
            maxExtra = std::max(maxExtra, extra);
            if (airborne && cc && cc->grounded) {
                atLanding = extra; // the landing step itself
                airborne = false;
            }
            prevCamY = camY;
            prevFeetY = feetY;
        }
        std::ostringstream detail;
        detail << "maxCameraExtraPerStep=" << maxExtra * 1000.0f << " mm landingStep="
               << atLanding * 1000.0f << " mm";
        report("landing bob", maxExtra < 0.010f && atLanding < 0.010f, detail.str());
        h.held.clear();
    }

    // Trigger: the red ball used to be inert (no collider, no trigger), so
    // "walking over the red circle does nothing". It must now fire exactly one
    // enter event while standing on it, not one per step.
    {
        EntityId ballTrigger = kInvalidEntity;
        scene.view<TriggerVolume>([&](EntityId id, TriggerVolume &t) {
            if (t.tag == "red-ball")
                ballTrigger = id;
        });
        if (ballTrigger == kInvalidEntity) {
            report("red-ball trigger", false, "no TriggerVolume tagged red-ball");
        } else {
            TriggerVolume *tv = scene.find<TriggerVolume>(ballTrigger);
            tv->enterCount = 0;
            tv->playerInside = false;
            tv->fired = false;
            resetCharacter(app, player, { -0.8f, kFloorTopY, 0.2f });
            h.held = { GLFW_KEY_W };
            for (int i = 0; i < 38; ++i) { // walk onto the ball (z ~ -1.3)
                h.step();
                updateTriggers(app, player);
            }
            h.held.clear();
            for (int i = 0; i < 20; ++i) { // stand on it: no re-fire
                h.step();
                updateTriggers(app, player);
            }
            const glm::vec3 end = scene.transform(player).position;
            std::ostringstream detail;
            detail << "end=(" << end.x << "," << end.y << "," << end.z << ")"
                   << " enters=" << tv->enterCount << " inside=" << (tv->playerInside ? 1 : 0)
                   << " volume x[" << tv->worldMin.x << "," << tv->worldMax.x << "]";
            // The ball is solid now: walking into it must stop at its face
            // (near face z=-1.2, body radius 0.28 -> stop at ~-0.92), not pass
            // through to the far side.
            report("red-ball trigger",
                tv->enterCount == 1 && tv->playerInside && end.z > -1.05f, detail.str());
            h.held.clear();
        }
    }

    // Look spike: a warp-sized mouse delta inside one fixed step must not move
    // the camera. This is the reproduced "camera snaps to a fixed orientation"
    // -- an F1 capture change delivered a 205 px cursor warp as one delta,
    // which turned the view by 0.451 rad (~26 deg) in both axes. The
    // controller rejects any step that would turn more than 0.35 rad, and the
    // recorder counts the rejection.
    {
        resetCharacter(app, player, padSpawn);
        h.held.clear();
        const int rejectsBefore = FlightRecorder::get().anomalyCount();
        const float yaw0 = cam.yaw;
        const float pitch0 = cam.pitch;
        h.step({ 900.0f, 900.0f }); // ~2 rad at the default sensitivity
        const float dy = std::abs(cam.yaw - yaw0);
        const float dp = std::abs(cam.pitch - pitch0);
        const int rejects = FlightRecorder::get().anomalyCount() - rejectsBefore;
        // ...and ordinary movement still works immediately afterwards.
        h.step({ 10.0f, 0.0f });
        const float dy2 = std::abs(cam.yaw - yaw0);
        std::ostringstream detail;
        detail << "spikeYaw=" << dy << " spikePitch=" << dp << " rejected=" << rejects
               << " normalTurn=" << dy2;
        report("look spike", dy < 1e-4f && dp < 1e-4f && rejects >= 1 && dy2 > 0.01f,
            detail.str());
    }

    // Flashlight aim: the beam must point where the player looks (within a
    // degree or two) while the origin keeps the hand offset. Parallel-aiming
    // from an offset origin made the bright spot slide ~20 degrees sideways on
    // a wall 30 cm away.
    {
        float worstAngle = 0.0f;
        float originOffset = 0.0f;
        for (float yawDeg : { 0.0f, 45.0f, -120.0f }) {
            resetCharacter(app, player, padSpawn);
            cam.yaw = glm::radians(yawDeg);
            cam.pitch = -0.2f;
            h.held.clear();
            for (int i = 0; i < 2; ++i) {
                h.step();
                syncFlashlight(app, flashlight);
                scene.updateWorldMatrices();
            }
            const glm::mat4 &lightWorld = scene.worldMatrix(flashlight);
            const glm::vec3 beam = -glm::normalize(glm::vec3(lightWorld[2]));
            worstAngle = std::max(worstAngle,
                std::acos(glm::clamp(glm::dot(beam, cam.forward()), -1.0f, 1.0f)));
            originOffset = glm::length(glm::vec3(lightWorld[3]) - cam.position);
        }
        std::ostringstream detail;
        detail << "beam-vs-view=" << glm::degrees(worstAngle) << " deg handOffset="
               << originOffset << " m";
        report("flashlight aim", worstAngle < glm::radians(2.0f)
                && std::abs(originOffset - 0.156f) < 0.02f,
            detail.str());
    }

    // Crouch: the eye has to come down with the body. It used to be a fixed
    // 1.62 m, so crouching only shrank the collider and slowed the walk while
    // the camera never moved ("crouch doesn't work").
    {
        resetCharacter(app, player, padSpawn);
        h.held.clear();
        for (int i = 0; i < 5; ++i)
            h.step();
        const float eyeStanding = cam.position.y - scene.transform(player).position.y;
        h.held = { GLFW_KEY_C };
        float maxStep = 0.0f;
        float prevY = cam.position.y;
        for (int i = 0; i < 60; ++i) {
            h.step();
            maxStep = std::max(maxStep, std::abs(cam.position.y - prevY));
            prevY = cam.position.y;
        }
        const float eyeCrouched = cam.position.y - scene.transform(player).position.y;
        h.held.clear();
        for (int i = 0; i < 90; ++i)
            h.step(); // stand back up
        const float eyeBack = cam.position.y - scene.transform(player).position.y;
        const CharacterController *cc = scene.find<CharacterController>(player);
        std::ostringstream detail;
        detail << "eye standing=" << eyeStanding << " crouched=" << eyeCrouched
               << " back=" << eyeBack << " maxStep=" << maxStep * 1000.0f << " mm cursor="
               << (cc ? cc->crouching : false);
        report("crouch eye",
            eyeCrouched < eyeStanding - 0.5f && std::abs(eyeBack - eyeStanding) < 0.01f
                && maxStep < 0.15f,
            detail.str());
    }

    // Mouse warp guard: a cursor sample can never reach the camera right after
    // a capture/focus change, nor as a single warp-sized event. This is the
    // "camera snaps to a fixed orientation" regression -- a 1000+ px warp is
    // ~60 degrees of yaw and pins the pitch at its +/-89 clamp.
    {
        Input &in = app.input();
        const int warpsBefore = in.warpEvents();
        in.setMouseCaptured(false);
        in.setMouseCaptured(true); // re-baseline: 3 samples + 8 frames swallowed
        // The capture warp and the position event that follows it (the pair
        // that used to slip a 205 px delta through).
        in.simulateCursorMove({ 960.0f, 540.0f });
        in.simulateCursorMove({ 962.0f, 541.0f });
        in.simulateCursorMove({ 963.0f, 541.0f }); // fills the 3-sample window
        // Let the frame window expire. update() re-arms it whenever the window
        // focus changes, which can happen underneath a test run, so stop when
        // it is actually clear rather than after a fixed count.
        for (int i = 0; i < 200 && in.resyncFramesLeft() > 0; ++i)
            in.update();
        const int framesLeft = in.resyncFramesLeft();
        const glm::vec2 afterResync = in.consumeMouseDelta();
        in.simulateCursorMove({ 973.0f, 546.0f }); // +10/+5 px: real motion
        const glm::vec2 real = in.consumeMouseDelta();
        in.simulateCursorMove({ 2500.0f, -800.0f }); // 1500 px jump: warp
        const glm::vec2 warp = in.consumeMouseDelta();
        const bool pass = glm::length(afterResync) < 1e-4f
            && std::abs(real.x - 10.0f) < 1e-3f && std::abs(real.y - 5.0f) < 1e-3f
            && glm::length(warp) < 1e-4f && in.warpEvents() > warpsBefore;
        std::ostringstream detail;
        detail << "resyncDelta=" << glm::length(afterResync) << " realDelta=(" << real.x
               << "," << real.y << ") warpDelta=" << glm::length(warp)
               << " warpsRejected=" << in.warpEvents() << " resyncFramesLeft=" << framesLeft;
        report("mouse warp guard", pass, detail.str());
    }

    // Re-baseline counter: extra cursor samples must not drive the sample
    // window negative. resyncSamples_ is decremented only while > 0, so a
    // burst of samples and a run past the 8-frame window leave it at exactly
    // 0; look input must still work afterwards. This is the regression guard
    // for the counter underflowing (which would keep swallowing look forever
    // if only the frame window were consulted).
    {
        Input &in = app.input();
        in.setMouseCaptured(false);
        in.setMouseCaptured(true); // re-baseline: sample + frame windows re-arm
        const glm::vec2 start{ 400.0f, 300.0f };
        in.simulateCursorMove(start);
        for (int i = 1; i <= 40; ++i) // far more samples than the 3-sample window
            in.simulateCursorMove(start + glm::vec2(static_cast<float>(i), 0.0f));
        const int samplesAfterBurst = in.resyncSamplesLeft();
        // One sample per frame, well past the frame window, so an unguarded
        // decrement would keep running on every extra event.
        for (int i = 0; i < 24; ++i) {
            in.simulateCursorMove(start + glm::vec2(60.0f + static_cast<float>(i), 0.0f));
            in.update();
        }
        // update() re-arms the window if the real window focus changes under
        // the test run; drain it the same way the warp-guard check does.
        for (int i = 0; i < 200 && in.resyncFramesLeft() > 0; ++i) {
            in.simulateCursorMove(start + glm::vec2(100.0f + static_cast<float>(i), 0.0f));
            in.update();
        }
        const int samplesLeft = in.resyncSamplesLeft();
        const int framesLeft = in.resyncFramesLeft();
        in.consumeMouseDelta();
        in.simulateCursorMove(in.mousePos() + glm::vec2(10.0f, 6.0f));
        const glm::vec2 resumed = in.consumeMouseDelta();
        std::ostringstream detail;
        detail << "afterBurst=" << samplesAfterBurst << " samplesLeft=" << samplesLeft
               << " framesLeft=" << framesLeft << " resumed=(" << resumed.x << ","
               << resumed.y << ")";
        report("resync counter",
            samplesAfterBurst == 0 && samplesLeft == 0 && framesLeft == 0
                && std::abs(resumed.x - 10.0f) < 1e-3f
                && std::abs(resumed.y - 6.0f) < 1e-3f,
            detail.str());
    }

    // Test models: the lowest vertex must sit on the floor slab's top face --
    // rest pose *and* while the clip plays. Regression check for the
    // sunk-models bug (placement ignored each model's origin/axis/scale) and
    // for the walk clip dropping the planted foot below the bind pose.
    auto reportModelFloor = [&](const char *name, const LevelInstance &level) {
        if (!scene.valid(level.root)) {
            report(name, false, "model not instantiated");
            return;
        }
        scene.updateWorldMatrices();
        glm::vec3 mn(0.0f), mx(0.0f);
        if (!levelWorldBounds(scene, level, mn, mx)) {
            report(name, false, "no mesh bounds");
            return;
        }
        float restY = mn.y;
        const float lowest = levelLowestAnimatedY(scene, level, restY);
        const float error = lowest - kFloorTopY;
        std::ostringstream detail;
        detail << "lowest_y=" << lowest << " (rest " << restY << ") floor=" << kFloorTopY
               << " error=" << error * 1000.0f << " mm top_y=" << mx.y;
        report(name, std::abs(error) < 1e-3f, detail.str());
    };

    // The skeleton the debug overlay draws must live in the same space as the
    // skinned vertices: CPU-skin the foot/head vertex with the matrices the
    // renderer uploads and compare against the joint position the overlay
    // draws. A wrong joint space (the mesh node's inverse not applied) showed
    // up as 1.85 m of head offset on CesiumMan.
    auto reportModelSkin = [&](const char *name, const LevelInstance &level) {
        if (!scene.valid(level.root)) {
            report(name, false, "model not instantiated");
            return;
        }
        scene.updateWorldMatrices();
        float maxFoot = 0.0f, maxHead = 0.0f, minLow = std::numeric_limits<float>::max();
        int samples = 0;
        int worstOutside = 0;
        for (EntityId id : level.meshEntities) {
            if (!scene.find<SkinnedMeshRenderer>(id))
                continue;
            // Same density the placement used, so "no foot below the floor at
            // any sampled pose" actually covers the deepest part of the cycle.
            for (const SkinSample &s : sampleSkin(scene, id, levelSkinSamples)) {
                maxFoot = std::max(maxFoot, s.footOffset);
                maxHead = std::max(maxHead, s.headOffset);
                minLow = std::min(minLow, s.lowestY);
                worstOutside = std::max(worstOutside, s.jointsOutside);
                ++samples;
            }
        }
        if (samples == 0) {
            report(name, false, "no skinned samples");
            return;
        }
        std::ostringstream detail;
        detail << "samples=" << samples << " maxFootOffset=" << maxFoot
               << " maxHeadOffset=" << maxHead << " minLowestY=" << minLow
               << " jointsOutside=" << worstOutside << " (floor " << kFloorTopY << ")";
        report(name,
            maxFoot < 0.30f && maxHead < 0.60f && minLow > kFloorTopY - 1e-3f
                && worstOutside == 0,
            detail.str());
    };
    reportModelFloor("CesiumMan floor", cesiumMan);
    reportModelFloor("Fox floor", fox);
    reportModelSkin("CesiumMan skin", cesiumMan);
    reportModelSkin("Fox skin", fox);

    scene.destroy(ledge);
    scene.destroy(thinWall);
    scene.destroy(pad);
    h.releaseAll();
    resetCharacter(app, player, { 0.0f, kFloorTopY, 1.8f });
    scene.updateWorldMatrices();
    scene.refreshColliderBounds();

    HOR_LOG_INFO("[selftest] {} of {} checks passed", checked - failed, checked);
    return failed;
}

} // namespace

int main(int argc, char **argv)
{
    using namespace horror;

    for (int i = 1; i < argc; ++i) if (std::string_view(argv[i]) == "--lobby-test") return runLobbyTests();

    // Tiny arg parser: the smoke-test/CICapture flags matter more than full
    // CLI polish at this stage.
    Application::Config cfg;
    int frameLimit = 0;
    int screenshotFrame = 29;
    std::string screenshotPath;
    std::string script;      // --script: scripted input before the main loop
    bool quickplay = false;
    bool frontendTest = false;
    bool selftest = false;   // --selftest: scripted movement/mouse assertions
    bool testModels = false; // --test-models: load CesiumMan/Fox sample assets
    bool debugInput = false; // --debug-input: per-step/per-frame input trace
    bool debugSkin = false;  // --debug-skin: CPU-skin and compare against bones
    bool debugBones = false; // --debug-bones: show the skeleton overlay at start
    bool startPatrol = false; // --patrol: walk the sample models immediately
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto value = [&]() -> const char * { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--play") quickplay = true;
        else if (a == "--frontend-test") frontendTest = true;
        else if (a == "--w")
            cfg.windowWidth = static_cast<uint32_t>(std::atoi(value()));
        else if (a == "--h")
            cfg.windowHeight = static_cast<uint32_t>(std::atoi(value()));
        else if (a == "--vsync")
            cfg.vsync = std::strcmp(value(), "off") != 0;
        else if (a == "--fullscreen")
            cfg.fullscreen = true;
        else if (a == "--frames")
            frameLimit = std::atoi(value());
        else if (a == "--screenshot")
            screenshotPath = value();
        else if (a == "--shot-frame")
            screenshotFrame = std::atoi(value());
        else if (a == "--shadow")
            cfg.shadowMapSize = static_cast<uint32_t>(std::atoi(value()));
        else if (a == "--ui")
            cfg.windowTitle = value();
        else if (a == "--script")
            script = value();
        else if (a == "--selftest")
            selftest = true;
        else if (a == "--test-models")
            testModels = true;
        else if (a == "--debug-input")
            debugInput = true;
        else if (a == "--debug-skin")
            debugSkin = true;
        else if (a == "--debug-bones")
            debugBones = true;
        else if (a == "--patrol")
            startPatrol = true;
        else if (a == "--wireframe")
            cfg.wireframe = true;
    }

    Application app{ cfg };
    app.setFrameLimit(frameLimit);

    buildTestScene(app);

    // Step 2 test content: Khronos sample models (skinned, animated). Held in
    // main's scope for the whole run: the level keeps the asset
    // (meshes/materials/textures) alive. Off by default so the base scene is
    // the procedural corridor + crates only; --selftest needs them for the
    // floor-snap assertions.
    LevelInstance cesiumMan;
    LevelInstance fox;
    if (testModels || selftest || debugSkin) {
        // Flanking the corridor just ahead of the spawn: inside the flashlight
        // cone and unobstructed (the far end is hidden behind crate-3/pillar),
        // while the centre line between them stays walkable. The y in these
        // positions is only a hint -- spawnGltfLevel lifts each root from its
        // measured rest-pose bounds so the feet touch the floor.
        spawnGltfLevel(app, "test/CesiumMan.glb", { -1.0f, 0.0f, -2.8f }, 180.0f,
            cesiumMan);
        // The Fox asset is authored in centimeters (mesh bounds ~80 units tall),
        // so its level root carries the scale. CesiumMan is already in meters.
        spawnGltfLevel(app, "test/Fox.glb", { 1.1f, 0.0f, -2.4f }, 165.0f, fox,
            "Walk", 0.012f);
    }

    // Patrol data for the sample models. Their clips are in-place by design
    // (no root-motion channel: see NOTES), so 'M' translates the level root
    // along the model's facing to make the walk readable in the world.
    std::vector<PatrolUnit> patrolUnits;
    bool patrolEnabled = startPatrol;
    {
        const LevelInstance *levels[] = { &cesiumMan, &fox };
        const float speeds[] = { 0.6f, 0.45f };
        for (size_t i = 0; i < 2; ++i) {
            if (!app.scene().valid(levels[i]->root))
                continue;
            PatrolUnit unit;
            unit.root = levels[i]->root;
            const Transform &t = app.scene().transform(unit.root);
            unit.basePos = t.position;
            unit.baseYaw = glm::eulerAngles(t.rotation).y;
            unit.dir = { -std::sin(unit.baseYaw), 0.0f, -std::cos(unit.baseYaw) };
            unit.speed = speeds[i];
            patrolUnits.push_back(unit);
        }
    }

    // --- fixed-step gameplay ------------------------------------------------
    EntityId player = 0; // first created entity is the player (see builder above)
    EntityId flashlight = 0;
    {
        // Find by name; index bookkeeping stays out of gameplay logic.
        Scene &s = app.scene();
        for (size_t i = 0; i < s.entityNames().size(); ++i) {
            if (s.entityNames()[i] == "player")
                player = static_cast<EntityId>(i);
            if (s.entityNames()[i] == "flashlight")
                flashlight = static_cast<EntityId>(i);
        }
    }
    if (debugSkin) {
        // Data first: measure the two paths (skinned vertices from the UBO
        // matrices vs. the joint positions the skeleton overlay draws) across
        // the whole clip before touching anything.
        debugSkinReport(app, cesiumMan, "CesiumMan", 9);
        debugSkinReport(app, fox, "Fox", 9);
    }

    // The skeleton overlay is an ImGui toggle; expose it from the command line
    // too so a single run shows mesh + bones at the same animation time.
    if (debugBones)
        app.renderer().setDrawBones(true);
    if (cfg.wireframe)
        app.renderer().setWireframe(true);

    CharacterSystem::Hooks hooks{ &app.camera(), &app.input(), debugInput };

    // Scripted runs happen before the render loop; the scene's static colliders
    // need world bounds by then, which Application only refreshes at loop start.
    app.scene().updateWorldMatrices();
    app.scene().refreshColliderBounds();
    if (selftest) {
        const int failures = runSelfTest(app, player, cfg.fixedStepSeconds, flashlight,
            cesiumMan, fox);
        return failures == 0 ? 0 : 1;
    }


    const glm::vec3 initialSpawn = app.scene().transform(player).position;
    Frontend frontend(app, player, [&] {
        resetCharacter(app, player, initialSpawn);
        syncFlashlight(app, flashlight);
    });
    if (quickplay || !script.empty()) frontend.start();
    if (!script.empty()) runScript(app, player, cfg.fixedStepSeconds, script, debugInput);
    app.onDrawUI = [&] { frontend.draw(); };
    app.onEscape = [&] { frontend.escape(); };
    app.onFixedStep = [&](float dt) {
        frontend.session.advance();
        CharacterSystem::step(app.scene(), frontend.session.localEntity(), hooks, dt);
        updatePatrol(app.scene(), patrolUnits, dt, patrolEnabled);
        updateTriggers(app, player);

        // Flashlight rides the camera: same position + light offset, same
        // orientation basis. (The renderer aims it at the camera basis too;
        // keeping the transform in sync means the light can be re-parented
        // to, say, a held prop later without renderer changes.)
        // Flashlight rides the camera: hand offset for the origin, beam aimed
        // at the crosshair. The renderer reads this entity's world matrix as
        // the light origin/axis.
        syncFlashlight(app, flashlight);
    };

    // Variable-rate hook: nothing here yet (camera smoothing lives in the
    // controller); documented extension point for gameplay animations.
    // Screenshot capture is deferred ~30 frames so startup (shadow map init,
    // ImGui font bake) has definitely settled before the image is read back.
    app.onFrameUpdate = [&](float frameDt) {
        if (frontendTest) {
            auto require = [&](bool ok) { if (!ok) { HOR_LOG_ERROR("frontend test failed at frame {}", app.framesRun()); std::exit(2); } };
            const int f = app.framesRun();
            if (f == 0) require(!app.simulationEnabled && !app.input().mouseCaptured());
            if (f == 2) { frontend.start(); require(frontend.session.players().size() == 1 && app.input().mouseCaptured()); }
            if (f == 5) { frontend.escape(); require(!app.simulationEnabled && !app.input().mouseCaptured()); }
            if (f == 8) { frontend.resume(); require(app.simulationEnabled); }
            if (f == 12) { frontend.leave(); require(frontend.session.players().empty() && !app.simulationEnabled); }
            if (f == 15) { frontend.start(); require(frontend.session.tick() == 0 && frontend.session.players().size() == 1); }
            if (f == 18) frontend.leave();
            if (f == 20) { frontend.requestQuit(); require(frontend.confirmingQuit()); }
            if (f == 25) { frontend.escape(); require(!frontend.confirmingQuit() && !app.simulationEnabled); }
            if (f == 28) { HOR_LOG_INFO("frontend lifecycle and quit cancel PASS"); app.quit(); }
        }
        (void)player;
        (void)flashlight;

        // Key handling that changes game state lives here, not in the fixed
        // step: several fixed steps can run per frame, and an edge action
        // would then toggle once per step.
        if (app.simulationEnabled && app.input().pressed(Action::TogglePatrol)) {
            patrolEnabled = !patrolEnabled;
            noteHotkey(kHotkeyPatrol);
            HOR_LOG_INFO("patrol {}", patrolEnabled ? "on" : "off");
        }
        if (debugInput) {
            // Mouse is consumed by the fixed step, so this shows what is still
            // pending plus the resulting orientation for this frame.
            const glm::vec2 pending = app.input().mouseDelta();
            HOR_LOG_INFO("[frame] {} dt={:.4f} pendingMouse=({:+.1f},{:+.1f}) yaw={:.4f} pitch={:.4f}",
                app.framesRun(), frameDt, pending.x, pending.y,
                app.camera().yaw, app.camera().pitch);
        }
        if (!screenshotPath.empty() && app.framesRun() == screenshotFrame)
            app.requestScreenshot(screenshotPath);
        if (app.framesRun() == 30) {
            const auto &rs = app.renderer().stats();
            HOR_LOG_INFO("stats: submitted {} drawn {} culled {} tri {} shadow {}",
                rs.itemsSubmitted, rs.itemsDrawn, rs.itemsCulled, rs.triangles, rs.shadowDraws);
            HOR_LOG_INFO("camera: pos ({:.3f},{:.3f},{:.3f}) yaw {:.3f} pitch {:.3f}",
                app.camera().position.x, app.camera().position.y, app.camera().position.z,
                app.camera().yaw, app.camera().pitch);
            size_t animators = 0;
            app.scene().view<Animator>([&](EntityId, const Animator &) { ++animators; });
            HOR_LOG_INFO("scene: {} entities, {} animators, {} asset models alive",
                app.scene().entityCount(), animators, app.assets().liveCount());
        }
    };

    int exitCode = app.run();
    HOR_LOG_INFO("exiting (code {}) after {} frames", exitCode, app.framesRun());
    return exitCode;
}
