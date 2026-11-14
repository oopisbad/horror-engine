#pragma once

#include "render/Camera.hpp"
#include "render/RenderVolumes.hpp"
#include "scene/Components.hpp"

#include <glad/gl.h>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace horror {

struct Animator;
class Scene;
class Shader;
class Framebuffer;
class DebugDraw;
class Mesh;
class Texture;
struct Material;
struct MeshRenderer;

// One queued drawable for the frame. Built fresh per frame from the scene --
// cache-friendlier than iterating unordered_maps mid-draw, and the natural
// place to add sorting (opaque/near->far, then transparent) later.
struct RenderItem {
    const Mesh *mesh = nullptr;       // owning pool lives in Scene
    const Material *material = nullptr;
    glm::mat4 world{1.0f};
    glm::vec3 centerWorld{0.0f}; // center of conservative world bounds
    glm::vec3 worldExtent{0.0f};
    bool castsShadow = true;
    bool cameraVisible = false;
    bool dynamic = false;
    // Non-null for skinned meshes: packed joint matrices from the Animator.
    const std::vector<glm::mat4> *bones = nullptr;
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
    float bloom = 0.15f;
    int volumeSteps = 12;
    float grain = 0.014f;          // 0 = off
    bool temporalAA = true;
    bool screenReflections = true;
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
    uint32_t cachedShadowFaces = 0;
    uint32_t updatedShadowFaces = 0;
    uint32_t activeProbes = 0;
    uint32_t activeDecals = 0;
    uint32_t activeFogVolumes = 0;
    uint32_t temporalFrames = 0;
    bool screenReflections = false;
    // Last complete sample from the asynchronous GPU query ring, in milliseconds.
    std::array<double, 4> gpuMs{}; // shadow, geometry, effects, composite
    uint32_t gpuSamples = 0;
};

// Forward renderer with:
//   - flashlight spotlight with hardware percentage-closer filtering
//   - up to 8 point lights and 4 local spots, with a budget for cached maps
//   - one lit shader source pair compiled twice: static, and SKINNED for
//     GPU skinning through a bone-matrix UBO (uniform-driven materials)
//   - independent camera and flashlight frustum culling
//
// Structured for a deferred/post chain: the scene is drawn into a Framebuffer
// abstraction, so inserting a G-buffer pass or a post pass is a pass-count
// change, not a rewrite.
class ForwardRenderer {
public:
    explicit ForwardRenderer(uint32_t shadowMapSize = 2048);
    ~ForwardRenderer();

    // Full frame: build queues, render shadows and geometry, then composite.
    void render(Scene &scene, Camera &camera);

    // Hot-reload pass for all shaders; returns true if anything changed.
    bool tickShaderReloads();

    // --- debug toggles -------------------------------------------------------
    void setWireframe(bool on);
    bool wireframe() const { return wireframe_; }
    void setDebugDrawEnabled(bool on);
    bool debugDrawEnabled() const { return debugDrawEnabled_; }
    // Draw joint lines for skinned meshes (ImGui toggle).
    void setDrawBones(bool on) { drawBones_ = on; }
    bool drawBones() const { return drawBones_; }

    void setAmbient(const AmbientLight &a) { ambient_ = a; }
    const AmbientLight &ambient() const { return ambient_; }

    // Swapchain size (Application pushes this on framebuffer resize). The
    // shadow pass changes the GL viewport, so the scene pass must restore it.
    void setOutputSize(uint32_t width, uint32_t height);
    void setRenderScale(float scale);
    void setLocalShadowBudget(int spot, bool point) { spotShadowBudget_ = spot; pointShadows_ = point; }
    void resetHistory() { historyValid_ = false; }
    void setGpuProfiling(bool enabled) { profileGpu_ = enabled; }
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
    void shadowPass(const Camera &camera);
    void drawShadowMap(const glm::mat4 &vp, const Shader &rigid, const Shader &skinned,
        const glm::vec3 *point = nullptr, float range = 0.0f);
    void updateLocalShadows(const Camera &camera);
    void gatherEnvironment(Scene &scene, const Camera &camera);
    void bindEnvironment(const Shader &shader);
    void captureProbes(Scene &scene, const Camera &camera);
    void resolveTemporal(const Camera &camera);
    void traceReflections(const Camera &camera);
    GLuint sceneColor() const;
    void buildQueue(Scene &scene, const Camera &camera);
    void mainPass(Scene &scene, Camera &camera);
    void postPass(const Camera &camera);
    void effectsPass(const Camera &camera);
    void drawDebugData(Scene &scene, Camera &camera);
    void setupLightingUniforms(const Shader &shader, const Camera &camera);
    void bindLitUniforms(const Shader &shader, Camera &camera);
    void uploadBones(const std::vector<glm::mat4> &bones);
    void ensureOutputTarget();

    std::unique_ptr<Shader> litShader_;
    std::unique_ptr<Shader> litSkinnedShader_;   // same source, SKINNED define
    std::unique_ptr<Shader> debugShader_;
    std::unique_ptr<Shader> shadowShader_;
    std::unique_ptr<Shader> shadowSkinnedShader_;
    std::unique_ptr<Shader> postShader_;
    std::unique_ptr<Shader> bloomShader_;
    std::unique_ptr<Shader> volumeShader_;
    std::unique_ptr<Shader> pointShadowShader_, pointShadowSkinnedShader_;
    std::unique_ptr<Shader> temporalShader_, reflectionShader_;

    // GPU skinning: bone matrices for the current draw. std140 mat4[128].
    static constexpr int kMaxBones = 128;
    GLuint boneUbo_ = 0;

    std::unique_ptr<Framebuffer> shadowTarget_; // depth-only, flashlight view
    std::unique_ptr<Framebuffer> sceneTarget_;  // HDR color + depth, post input
    std::unique_ptr<Framebuffer> volumeTarget_;
    std::unique_ptr<Framebuffer> probeTarget_, reflectionTarget_;
    std::array<std::unique_ptr<Framebuffer>, 2> history_;
    bool historyValid_ = false;
    bool temporalActive_ = false;
    bool reflectionsReady_ = false;
    size_t historyWrite_ = 0;
    uint32_t jitterFrame_ = 0;
    glm::mat4 drawViewProj_{1.0f}, previousViewProj_{1.0f};
    glm::vec2 rasterOffset_{0.0f};
    glm::vec3 previousEye_{0}, previousForward_{0,0,-1};
    glm::vec3 previousLampPosition_{0}, previousLampDirection_{0,0,-1};
    glm::vec3 previousLampColor_{0};
    float previousTime_ = 0;
    bool capturingProbe_ = false;
    std::array<std::unique_ptr<Framebuffer>, 4> bloomTargets_;
    GLuint shadowSampler_ = 0;
    float renderScale_ = 1.0f;
    bool profileGpu_ = false;
    struct GpuFrame {
        std::array<GLuint, 5> marks{};
        bool pending = false;
    };
    std::array<GpuFrame, 12> gpuFrames_{};
    size_t gpuFrame_ = 0;
    // 1x1 magenta fallback bound for material slots without a texture. Owned
    // here so its GL name dies with the renderer (context still current).
    std::unique_ptr<Texture> placeholder_;
    GLuint fullscreenVao_ = 0;                  // attribute-less post draw
    std::vector<RenderItem> queue_;

    uint32_t outputWidth_ = 0;
    uint32_t outputHeight_ = 0;
    float timeSeconds_ = 0.0f;
    PostSettings post_;

    AmbientLight ambient_;
    bool wireframe_ = false;
    bool debugDrawEnabled_ = false;
    bool drawBones_ = false;

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
    float shadowBias_ = 0.00038f;
    struct LocalSpot {
        EntityId id;
        glm::vec3 position, direction, color;
        glm::vec3 coneAndRange;
        bool castsShadow;
        int shadow = -1;
    };
    struct LocalPoint {
        EntityId id;
        glm::vec3 position, color;
        float range;
        bool castsShadow;
        bool shadow = false;
    };
    struct SpotShadow {
        std::unique_ptr<Framebuffer> map;
        glm::mat4 vp{1.0f};
        EntityId id = kInvalidEntity;
        uint64_t signature = 0;
        bool valid = false;
    };
    struct PointShadow {
        GLuint cube = 0, fbo = 0;
        std::array<uint64_t, 6> signatures{};
        std::array<bool, 6> valid{};
    };
    struct Probe {
        GLuint cube = 0;
        EntityId id = kInvalidEntity;
        glm::mat4 worldToBox{1.0f};
        glm::vec3 origin{0.0f};
        float strength = 0;
        bool ready = false;
    };
    struct FogInstance { glm::mat4 worldToBox; glm::vec3 color; float density; };
    struct DecalInstance { glm::mat4 worldToBox; SurfaceDecal decal; };
    std::array<LocalPoint, 8> localPoints_{};
    std::array<SpotShadow, 2> spotShadows_{};
    PointShadow pointShadow_;
    std::array<Probe, 2> probes_{};
    std::array<FogInstance, 4> fogVolumes_{};
    std::array<DecalInstance, 12> decals_{};
    size_t localPointCount_ = 0, probeCount_ = 0, fogCount_ = 0, decalCount_ = 0;
    int spotShadowBudget_ = 2;
    bool pointShadows_ = true;
    std::array<LocalSpot, 4> localSpots_{};
    size_t localSpotCount_ = 0;
    bool flashlightShadows_ = true;


};

} // namespace horror
