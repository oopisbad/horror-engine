#include "anim/Animator.hpp"
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

    // A ball demonstrating the sphere builder (also a collider).
    {
        EntityId e = scene.create("ball");
        scene.add(e, MeshRenderer{ ball, mat({ 0.7f, 0.1f, 0.1f }, 0.4f), true, true });
        scene.transform(e).position = { -0.8f, 0.3f, -1.5f };
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

// Feet on the floor: offsets the level ROOT (in world/parent space) so the
// lowest rest-pose vertex sits exactly on `floorY`. An authored spawn height
// alone is not enough -- it assumes each model's origin is at its feet, which
// is exactly the assumption that sank both samples once the root transform was
// lost (NOTES bug 20). CesiumMan's mesh lives under Z-up conversion nodes and
// the Fox is authored in centimeters with a 0.012 root scale, so the evaluated
// hierarchy is the only reliable source for the offset.
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
    const float lift = floorY - mn.y;
    scene.transform(level.root).position.y += lift;
    scene.updateWorldMatrices();
    HOR_LOG_INFO(
        "level '{}': rest bbox y [{:.3f}, {:.3f}] x [{:.3f}, {:.3f}] -> lifted {:.3f} m onto floor {:.3f}",
        scene.name(level.root), mn.y, mx.y, mn.x, mx.x, lift, floorY);
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
    return GLFW_KEY_UNKNOWN;
}

// Every key the harness is allowed to drive; cleared each frame unless held.
const std::vector<int> &scriptableKeys()
{
    static const std::vector<int> keys = {
        GLFW_KEY_W, GLFW_KEY_A, GLFW_KEY_S, GLFW_KEY_D,
        GLFW_KEY_SPACE, GLFW_KEY_LEFT_SHIFT, GLFW_KEY_LEFT_CONTROL
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
int runSelfTest(Application &app, EntityId player, float dt,
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
        { "crouch W", { GLFW_KEY_W, GLFW_KEY_LEFT_CONTROL }, { 0.0f, 0.0f, -1.0f }, 1.2f },
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

    // Test models: the rest-pose lowest vertex must sit on the floor slab's
    // top face. This is the regression check for the sunk-models bug: spawning
    // at the slab height ignored each model's own origin/axis/scale.
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
        const float error = mn.y - kFloorTopY;
        std::ostringstream detail;
        detail << "lowest_y=" << mn.y << " floor=" << kFloorTopY
               << " (error " << error * 1000.0f << " mm) top_y=" << mx.y;
        report(name, std::abs(error) < 5e-4f, detail.str());
    };
    reportModelFloor("CesiumMan floor", cesiumMan);
    reportModelFloor("Fox floor", fox);

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

    // Tiny arg parser: the smoke-test/CICapture flags matter more than full
    // CLI polish at this stage.
    Application::Config cfg;
    int frameLimit = 0;
    int screenshotFrame = 29;
    std::string screenshotPath;
    std::string script;      // --script: scripted input before the main loop
    bool selftest = false;   // --selftest: scripted movement/mouse assertions
    bool testModels = false; // --test-models: load CesiumMan/Fox sample assets
    bool debugInput = false; // --debug-input: per-step/per-frame input trace
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto value = [&]() -> const char * { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--w")
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
    if (testModels || selftest) {
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
    CharacterSystem::Hooks hooks{ &app.camera(), &app.input(), debugInput };

    // Scripted runs happen before the render loop; the scene's static colliders
    // need world bounds by then, which Application only refreshes at loop start.
    app.scene().updateWorldMatrices();
    app.scene().refreshColliderBounds();
    if (selftest) {
        const int failures = runSelfTest(app, player, cfg.fixedStepSeconds, cesiumMan, fox);
        return failures == 0 ? 0 : 1;
    }
    if (!script.empty())
        runScript(app, player, cfg.fixedStepSeconds, script, debugInput);

    app.onFixedStep = [&](float dt) {
        CharacterSystem::step(app.scene(), player, hooks, dt);

        // Flashlight rides the camera: same position + light offset, same
        // orientation basis. (The renderer aims it at the camera basis too;
        // keeping the transform in sync means the light can be re-parented
        // to, say, a held prop later without renderer changes.)
        // Hand-held offset: slightly right of and below the eye. The renderer
        // reads this entity's world matrix as the light origin/axis.
        app.scene().transform(flashlight).position = app.camera().position
            + app.camera().right() * 0.12f - app.camera().up() * 0.10f;
        // Quat from yaw around +Y then pitch around +X, so its local -Z matches
        // Camera::forward() exactly.
        app.scene().transform(flashlight).rotation =
            glm::angleAxis(app.camera().yaw, glm::vec3(0, 1, 0))
            * glm::angleAxis(app.camera().pitch, glm::vec3(1, 0, 0));
    };

    // Variable-rate hook: nothing here yet (camera smoothing lives in the
    // controller); documented extension point for gameplay animations.
    // Screenshot capture is deferred ~30 frames so startup (shadow map init,
    // ImGui font bake) has definitely settled before the image is read back.
    app.onFrameUpdate = [&app, player, flashlight, &screenshotPath, screenshotFrame, debugInput](float frameDt) {
        (void)player;
        (void)flashlight;
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
