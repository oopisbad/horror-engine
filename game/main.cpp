#include "anim/Animator.hpp"
#include "asset/GltfAsset.hpp"
#include "asset/LevelBuilder.hpp"
#include "core/Application.hpp"
#include "core/Log.hpp"
#include "render/Material.hpp"
#include "render/Mesh.hpp"
#include "scene/CharacterController.hpp"
#include "scene/Components.hpp"
#include "scene/Scene.hpp"

#include <glm/gtc/quaternion.hpp>

#include <cstdlib>
#include <cstring>
#include <string>

namespace {

using namespace horror;

// ---- test-scene construction -------------------------------------------------
// Procedural placeholders only (no assets yet): a dark test corridor, some
// boxes to prove collision, two point lights, camera + flashlight. Every
// mesh instance built here lives in Scene's pool so RAII cleans up in order.
void buildTestScene(Application &app)
{
    Scene &scene = app.scene();

    // Mesh pool: primitives made once and shared by every instance.
    Mesh *cube = scene.addMesh(std::make_unique<Mesh>(Mesh::makeCube(0.5f, "cube")));
    Mesh *floor = scene.addMesh(std::make_unique<Mesh>(Mesh::makePlane(8.0f, 4, "floor")));
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
        scene.add(f, AABBCollider{ glm::vec3(0.0f), glm::vec3(8.0f, 8.0f, 0.05f), {}, {}, true });
    }

    // Corridor walls: long thin boxes; doubles as collision geometry.
    const auto wall = [&](const char *name, const glm::vec3 &pos, const glm::vec3 &half, const glm::vec3 &albedo) {
        EntityId e = scene.create(name);
        scene.add(e, MeshRenderer{ cube, mat(albedo, 0.9f), true, true });
        Transform &t = scene.transform(e);
        t.position = pos;
        t.scale = half * 2.0f; // cube is unit-space, so scale == full extents
        scene.add(e, AABBCollider{ glm::vec3(0.0f), half, {}, {}, true });
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
        scene.add(e, AABBCollider{ glm::vec3(0.0f), glm::vec3(scale * 0.5f), {}, {}, true });
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

    // Player: feet at (0,0,0.0); the controller ticks it every fixed step.
    EntityId player = scene.create("player");
    scene.add(player, CharacterController{});
    app.camera().fovY = 75.0f;
    app.camera().position = { 0.0f, 1.62f, 1.8f };
    app.camera().yaw = 0.0f;   // looking down -Z into the corridor
    app.camera().pitch = -0.05f;

    // Flashlight entity: cone params come from the SpotLight component;
    // position/orientation are synced to the camera every fixed step below.
    EntityId flashlight = scene.create("flashlight");
    scene.add(flashlight, SpotLight{});
}

// Loads a glTF scene and starts a clip on every Animator it created.
// `preferredClip` falls back to the first clip when not present.
void spawnGltfLevel(Application &app, const char *path, const glm::vec3 &position,
    float yawDegrees, LevelInstance &instance, const char *preferredClip = nullptr,
    float scale = 1.0f)
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
    }

    Application app{ cfg };
    app.setFrameLimit(frameLimit);

    buildTestScene(app);

    // Step 2 test content: Khronos sample models (skinned, animated) in the
    // corridor. Held in main's scope for the whole run: the level keeps the
    // asset (meshes/materials/textures) alive.
    LevelInstance cesiumMan;
    LevelInstance fox;
    spawnGltfLevel(app, "test/CesiumMan.glb", { 0.0f, 0.05f, -5.5f }, 0.0f, cesiumMan);
    // The Fox asset is authored in centimeters (mesh bounds ~80 units tall),
    // so its level root carries the scale. CesiumMan is already in meters.
    spawnGltfLevel(app, "test/Fox.glb", { -1.1f, 0.05f, -8.2f }, 25.0f, fox, "Walk", 0.012f);

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
    CharacterSystem::Hooks hooks{ &app.camera(), &app.input() };
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
    app.onFrameUpdate = [&app, player, flashlight, &screenshotPath, screenshotFrame](float) {
        (void)player;
        (void)flashlight;
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
