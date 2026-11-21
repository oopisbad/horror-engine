#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "asset/GltfAsset.hpp"
#include "asset/LevelBuilder.hpp"
#include "render/Renderer.hpp"
#include "scene/Scene.hpp"
#include <glm/gtc/type_ptr.hpp>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <windows.h>

using namespace horror;
#define API extern "C" __declspec(dllexport)
namespace {
HGLRC context;
std::string error;
struct Current {
    HDC dc = wglGetCurrentDC();
    HGLRC previous = wglGetCurrentContext();
    Current()
    {
        if (!dc || !previous)
            throw std::runtime_error("Solid viewport context unavailable");
        if (!context)
            context = wglCreateContext(dc);
        if (!context || !wglMakeCurrent(dc, context))
            throw std::runtime_error("Cannot activate lighting context");
    }
    ~Current()
    {
        if (glad_glFlush)
            glFlush();
        wglMakeCurrent(dc, previous);
    }
};
GLADapiproc load(const char* name)
{
    PROC proc = wglGetProcAddress(name);
    if (reinterpret_cast<uintptr_t>(proc) <= 3 || proc == reinterpret_cast<PROC>(-1))
        proc = GetProcAddress(GetModuleHandleW(L"opengl32.dll"), name);
    return reinterpret_cast<GLADapiproc>(proc);
}
struct Preview {
    Scene scene;
    LevelInstance level;
    std::unordered_map<std::string, EntityId> objects;

    explicit Preview(const char* path)
    {
        auto model = std::make_shared<GltfModel>(path);
        if (!model->valid())
            throw std::runtime_error(model->error());
        level = LevelBuilder::instantiate(scene, model);
        scene.view<EntityExtras>([&](EntityId id, const EntityExtras& extras) {
            auto it = extras.values.find("editor_id");
            if (it != extras.values.end())
                objects.emplace(it->second, id);
        });
        // The editor camera does not carry the player's flashlight.
        auto id = scene.create("flashlight");
        SpotLight off;
        off.intensity = 0;
        off.castsShadows = false;
        scene.add(id, off);
    }
};
std::unique_ptr<Preview> preview;
std::unique_ptr<ForwardRenderer> renderer;
}
API const char* lit_load(const char* path)
{
    try {
        Current guard;
        if (!gladLoadGL(load) || !GLAD_GL_VERSION_4_5)
            throw std::runtime_error("Lit viewport requires OpenGL 4.5");
        if (!renderer) {
            renderer = std::make_unique<ForwardRenderer>(1024);
            renderer->post().temporalAA = false;
            renderer->post().grain = 0;
        }
        auto next = std::make_unique<Preview>(path);
        renderer->resetSceneCaches();
        preview = std::move(next);
        return nullptr;
    } catch (const std::exception& e) {
        error = e.what();
        return error.c_str();
    }
}
API void lit_transform(const char* key, const float* p, const float* q, const float* s)
{
    if (!preview)
        return;
    auto it = preview->objects.find(key);
    if (it == preview->objects.end())
        return;
    auto& t = preview->scene.transform(it->second);
    t.position = glm::make_vec3(p);
    t.rotation = glm::make_quat(q);
    t.scale = glm::make_vec3(s);
    renderer->invalidateProbes();
}
API const char* lit_draw(int width, int height, const float* eye, const float* target)
{
    try {
        Current guard;
        if (!preview)
            throw std::runtime_error("No lit scene loaded");
        Camera camera;
        camera.position = glm::make_vec3(eye);
        auto delta = glm::make_vec3(target) - camera.position;
        float distance = glm::length(delta);
        auto dir = delta / distance;
        camera.yaw = std::atan2(-dir.x, -dir.z);
        camera.pitch = std::asin(glm::clamp(dir.y, -1.f, 1.f));
        camera.fovY = 48;
        camera.nearPlane = std::max(.015f, distance * .0005f);
        camera.farPlane = std::max(2000.f, distance * 8);
        camera.update(float(width) / height);
        preview->scene.updateWorldMatrices();
        renderer->setOutputSize(width, height);
        renderer->render(preview->scene, camera);
        return nullptr;
    } catch (const std::exception& e) {
        error = e.what();
        return error.c_str();
    }
}
API void lit_close()
{
    if (!context)
        return;
    try {
        Current guard;
        preview.reset();
        renderer.reset();
    } catch (...) {
        return;
    }
    wglDeleteContext(context);
    context = nullptr;
}
