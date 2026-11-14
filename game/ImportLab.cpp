#include "game/ImportLab.hpp"

#include "asset/LevelBuilder.hpp"
#include "core/Application.hpp"
#include "debug/FlightRecorder.hpp"
#include "scene/CharacterController.hpp"

#include <cmath>

namespace horror {

bool checkImportLab(Application &app, LevelInstance &level, EntityId player)
{
    auto &physics = *level.physics;
    auto &scene = app.scene();
    auto &body = *scene.find<CharacterController>(player);
    auto &input = app.input();
    CharacterSystem::Hooks hooks{&app.camera(), &input, false, &physics};
    constexpr float dt = 1.0f/60.0f;
    bool success = true;
    const auto check = [&](bool result, const char *label) {
        success &= result;
        HOR_LOG_INFO("import lab {}: {}", label, result ? "PASS" : "FAIL");
    };
    const auto simulate = [&](int frames, bool forward, bool crouch) {
        input.setScriptedKey(GLFW_KEY_W, forward);
        input.setScriptedKey(GLFW_KEY_LEFT_CONTROL, crouch);
        for (int i = 0; i < frames; ++i)
            CharacterSystem::step(scene,player,hooks,dt);
    };
    const auto place = [&](glm::vec3 feet) {
        FlightRecorder::get().noteTeleport("import lab check");
        scene.transform(player).position = feet;
        body = CharacterController{};
        app.camera().yaw = 0;
        app.camera().pitch = 0;
    };

    check(!physics.geometry.empty() && physics.props.size() == 2
        && level.spawnPoints.size() >= 2 && !level.lights.empty() && !level.triggers.empty(),
        "named nodes and physics bake");
    place({-1.8f,0.02f,-2.4f});
    simulate(12,false,false);
    simulate(110,true,false);
    auto feet = scene.transform(player).position;
    HOR_LOG_INFO("import lab stairs position {:.3f}, {:.3f}, {:.3f}", feet.x,feet.y,feet.z);
    check(feet.z < -5.45f && feet.y > 0.70f && body.grounded, "stair ascent");
    simulate(142,true,false);
    feet = scene.transform(player).position;
    HOR_LOG_INFO("import lab ramp position {:.3f}, {:.3f}, {:.3f}", feet.x,feet.y,feet.z);
    check(feet.z < -9.55f && feet.y < 0.09f, "ramp descent");

    place({0,0.02f,-9.0f});
    simulate(75,true,false);
    check(scene.transform(player).position.z > -9.98f, "standing blocks at low beam");
    simulate(35,true,true);
    simulate(15,false,false);
    feet = scene.transform(player).position;
    HOR_LOG_INFO("import lab crouch: z {:.3f}, height {:.3f}, held under ceiling {}",
        feet.z, body.currentHeight, body.crouching);
    check(feet.z < -9.98f && body.crouching && body.currentHeight < 1.2f,
        "crouch and stand clearance");
    simulate(70,true,true);
    simulate(40,false,false);
    check(scene.transform(player).position.z < -10.85f && body.currentHeight > 1.65f,
        "standing after leaving beam");

    const auto thinWall = physics.sweep({2.0f,0.8f,-13.0f},{0.22f,0.7f,0.22f},{0,0,-3.6f});
    check(thinWall.time > 0.1f && thinWall.time < 0.8f && thinWall.normal.z > 0.9f,
        "fast sweep against thin wall");
    if (!physics.props.empty()) {
        const auto spawn = physics.props.front().spawn;
        for (int i = 0; i < 45; ++i)
            physics.stepProps(scene,dt,{100,100,100},{0.25f,0.9f,0.25f});
        const auto rested = physics.props.front().center;
        check(rested.y < spawn.y-0.1f && rested.y >= physics.props.front().half.y,
            "prop falls and rests");
        physics.push(physics.props.front().entity,{0,0,-2});
        for (int i = 0; i < 24; ++i)
            physics.stepProps(scene,dt,{100,100,100},{0.25f,0.9f,0.25f});
        check(physics.props.front().center.z < rested.z-0.07f, "prop push");
        physics.reset(scene);
        check(glm::length(physics.props.front().center-spawn) < 0.001f, "prop reset");
    }
    simulate(0,false,false);
    return success;
}

} // namespace horror
