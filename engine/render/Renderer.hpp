#pragma once

#include "render/Camera.hpp"

#include <glad/gl.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace horror {

class Scene;
class Shader;
class Framebuffer;
class DebugDraw;
class Mesh;
struct Material;
struct MeshRenderer;

// One queued drawable for the frame. Built fresh per frame from the scene --
// cache-friendlier than iterating unordered_maps mid-draw, and the natural
// place to add sorting (opaque/near->far, then transparent) later.
struct RenderItem {
    const Mesh *mesh = nullptr;       // owning pool lives in Scene
    const Material *material = nullptr;
    glm::mat4 world{1.0f};
    glm::vec3 centerWorld{0.0f}; // bounding sphere center for culling
    float boundingRadius = 0.0f;
    bool castsShadow = true;
};

// Ambient occlusion isn't in scope yet; ambient is a flat term with a very
// low floor so pitch-black rooms are actually pitch-black except for what
// the flashlight hits.
struct AmbientLight {
    glm::vec3 color{0.008f, 0.010f, 0.014f}; // very low, slightly blue "night" tint
    float intensity = 1.0f;
};

// Post chain controls. The scene is rendered into an HDR target and composited
// with tonemapping, depth fog, vignette and film grain.
struct PostSettings {
    bool enabled = true;
    float exposure = 1.05f;
    float vignette = 0.85f;       // 0 = off
    float grain = 0.02f;          // 0 = off
    float fogDensity = 0.055f;    // exponential-squared fog (1/m)
    glm::vec3 fogColor{0.0012f, 0.0016f, 0.0026f}; // linear, near black
};

// Per-frame stats surfaced in the ImGui overlay + smoke tests.
struct RenderStats {
    uint32_t itemsSubmitted = 0;
    uint32_t itemsDrawn = 0;
    uint32_t itemsCulled = 0;
    uint32_t shadowDraws = 0;
    uint32_t triangles = 0;
    float shadowResolution = 1024.f;
};

// Forward renderer with:
//   - flashlight spotlight + 2x2-PCF shadow map (the only shadow caster)
//   - up to 8 point lights (no shadowing yet)
//   - one shared lit shader for everything (uniform-driven material params)
//   - frustum culling against the camera, at queue-build time
//
// Structured for a deferred/post chain: the scene is drawn into a Framebuffer
// abstraction, so inserting a G-buffer pass or a post pass is a pass-count
// change, not a rewrite.
class ForwardRenderer {
public:
    explicit ForwardRenderer(uint32_t shadowMapSize = 1024);
    ~ForwardRenderer();

    // Full frame: shadow pass -> cull/queue -> main pass -> debug draw.
    void render(Scene &scene, Camera &camera);

    // Hot-reload pass for all shaders; returns true if anything changed.
    bool tickShaderReloads();

    // --- debug toggles -------------------------------------------------------
    void setWireframe(bool on);
    bool wireframe() const { return wireframe_; }
    void setDebugDrawEnabled(bool on);
    bool debugDrawEnabled() const { return debugDrawEnabled_; }

    void setAmbient(const AmbientLight &a) { ambient_ = a; }
    const AmbientLight &ambient() const { return ambient_; }

    // Swapchain size (Application pushes this on framebuffer resize). The
    // shadow pass changes the GL viewport, so the scene pass must restore it.
    void setOutputSize(uint32_t width, uint32_t height);
    // Monotonic seconds for screen-space effects (film grain animation).
    void setTime(float seconds) { timeSeconds_ = seconds; }

    PostSettings &post() { return post_; }
    const PostSettings &post() const { return post_; }
    void setPostEnabled(bool on) { post_.enabled = on; }
    bool postEnabled() const { return post_.enabled; }

    const RenderStats &stats() const { return stats_; }
    DebugDraw &debugDraw();

    // flashlight cone widening while sprinting (gameplay flavor), etc.
    void tuneFlashlight(float innerDeg, float outerDeg, float range, float intensity);

private:
    void shadowPass(Scene &scene, const Camera &camera);
    void buildQueue(Scene &scene, const Camera &camera);
    void mainPass(Scene &scene, Camera &camera);
    void postPass();
    void drawDebugData(Scene &scene, Camera &camera);
    void setupLightingUniforms(const Shader &shader, const Camera &camera);
    void ensureOutputTarget();

    std::unique_ptr<Shader> litShader_;
    std::unique_ptr<Shader> debugShader_;
    std::unique_ptr<Shader> shadowShader_;
    std::unique_ptr<Shader> postShader_;

    std::unique_ptr<Framebuffer> shadowTarget_; // depth-only, flashlight view
    std::unique_ptr<Framebuffer> sceneTarget_;  // HDR color + depth, post input
    GLuint fullscreenVao_ = 0;                  // attribute-less post draw
    std::vector<RenderItem> queue_;

    uint32_t outputWidth_ = 0;
    uint32_t outputHeight_ = 0;
    float timeSeconds_ = 0.0f;
    PostSettings post_;

    AmbientLight ambient_;
    bool wireframe_ = false;
    bool debugDrawEnabled_ = true;

    std::unique_ptr<DebugDraw> debugDraw_;
    RenderStats stats_{};

    // Flashlight state, gathered from the SpotLight component each frame
    // (single flashlight assumption; gameplay may flicker these values).
    glm::vec3 flashlightColor_{1.0f, 0.96f, 0.88f};
    float flashlightIntensity_ = 8.0f;
    float flashlightInnerDeg_ = 18.0f;
    float flashlightOuterDeg_ = 24.0f;
    float flashlightRange_ = 45.0f;
    // World-space origin/direction gathered from the SpotLight entity.
    glm::vec3 flashlightPos_{0.0f};
    glm::vec3 flashlightDir_{0.0f, 0.0f, -1.0f};
    bool hasFlashlightTransform_ = false;
    glm::mat4 flashlightViewProj_{1.0f};
    float shadowBias_ = 0.0015f;

    Scene *sceneForUniforms_ = nullptr;
};

} // namespace horror
