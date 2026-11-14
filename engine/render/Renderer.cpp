#include "render/Renderer.hpp"

#include "anim/Animator.hpp"
#include "core/Log.hpp"
#include "render/DebugDraw.hpp"
#include "render/Framebuffer.hpp"
#include "render/Material.hpp"
#include "render/Mesh.hpp"
#include "render/Shader.hpp"
#include "render/Texture.hpp"
#include "scene/Scene.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_access.hpp>

#include <algorithm>
#include <cmath>
#include <bit>
#include <functional>
#include <limits>
#include <stdexcept>

namespace horror {

ForwardRenderer::ForwardRenderer(uint32_t shadowMapSize)
{
    litShader_ = std::make_unique<Shader>("mesh.vert", "mesh.frag", "lit");
    litSkinnedShader_ = std::make_unique<Shader>("mesh.vert", "mesh.frag", "lit_skinned",
        std::vector<std::string>{ "SKINNED" });
    debugShader_ = std::make_unique<Shader>("debug.vert", "debug.frag", "debug");
    shadowShader_ = std::make_unique<Shader>("shadow_depth.vert", "shadow_depth.frag", "shadow");
    shadowSkinnedShader_ = std::make_unique<Shader>("shadow_depth.vert", "shadow_depth.frag",
        "shadow_skinned", std::vector<std::string>{ "SKINNED" });
    pointShadowShader_ = std::make_unique<Shader>("shadow_depth.vert", "point_shadow.frag", "point_shadow");
    pointShadowSkinnedShader_ = std::make_unique<Shader>("shadow_depth.vert", "point_shadow.frag", "point_shadow_skin",
        std::vector<std::string>{ "SKINNED" });
    temporalShader_ = std::make_unique<Shader>("post.vert", "temporal.frag", "temporal");
    reflectionShader_ = std::make_unique<Shader>("post.vert", "reflections.frag", "reflections");
    postShader_ = std::make_unique<Shader>("post.vert", "post.frag", "post");
    bloomShader_ = std::make_unique<Shader>("post.vert", "bloom.frag", "bloom");
    volumeShader_ = std::make_unique<Shader>("post.vert", "volume.frag", "volume");
    glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
    glCreateSamplers(1, &shadowSampler_);
    glSamplerParameteri(shadowSampler_, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glSamplerParameteri(shadowSampler_, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    glSamplerParameteri(shadowSampler_, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glSamplerParameteri(shadowSampler_, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glSamplerParameteri(shadowSampler_, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glSamplerParameteri(shadowSampler_, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // std140 array of 128 mat4 = 8 KiB; immutable storage + subdata updates.
    glCreateBuffers(1, &boneUbo_);
    glNamedBufferStorage(boneUbo_, kMaxBones * sizeof(glm::mat4), nullptr,
        GL_DYNAMIC_STORAGE_BIT);
    shadowTarget_ = std::make_unique<Framebuffer>(Framebuffer::Kind::DepthOnly,
        shadowMapSize, shadowMapSize);
    debugDraw_ = std::make_unique<DebugDraw>();
    // GL 4.5 core requires a bound VAO even for an attribute-less draw.
    glCreateVertexArrays(1, &fullscreenVao_);

    // One magenta texel bound for every material texture slot without a map.
    // Owned by the renderer (an engine-lifetime object created while the GL
    // context is current) instead of a function-local static, which used to be
    // destroyed after the context during static destruction.
    const uint8_t magenta[4] = { 255, 0, 255, 255 };
    placeholder_ = std::make_unique<Texture>(GL_RGBA8, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE,
        magenta,
        Texture::Params{ GL_NEAREST, GL_NEAREST, GL_REPEAT, GL_REPEAT, false });

    queue_.reserve(256);
    stats_.shadowResolution = static_cast<float>(shadowMapSize);
    HOR_LOG_DEBUG("forward renderer ready (shadow map {}x{})", shadowMapSize, shadowMapSize);
}

ForwardRenderer::~ForwardRenderer()
{
    glDeleteSamplers(1, &shadowSampler_);
    glDeleteTextures(1, &pointShadow_.cube);
    glDeleteFramebuffers(1, &pointShadow_.fbo);
    for (const auto &probe : probes_)
        glDeleteTextures(1, &probe.cube);
    for (auto &frame : gpuFrames_)
        if (frame.marks.front())
            glDeleteQueries(static_cast<GLsizei>(frame.marks.size()), frame.marks.data());
    if (boneUbo_)
        glDeleteBuffers(1, &boneUbo_);
    if (fullscreenVao_)
        glDeleteVertexArrays(1, &fullscreenVao_);
}

void ForwardRenderer::uploadBones(const std::vector<glm::mat4> &bones)
{
    if (!boneUbo_ || bones.empty())
        return;
    const size_t count = std::min(bones.size(), static_cast<size_t>(kMaxBones));
    if (bones.size() > static_cast<size_t>(kMaxBones))
        HOR_LOG_WARN_ONCE("skinned mesh has {} bones, GPU limit is {}", bones.size(), kMaxBones);
    glNamedBufferSubData(boneUbo_, 0, static_cast<GLsizeiptr>(count * sizeof(glm::mat4)), bones.data());
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, boneUbo_);
}

void ForwardRenderer::setOutputSize(uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0)
        return;

    if (!sceneTarget_ || outputWidth_ != width || outputHeight_ != height)
        resetHistory();
    outputWidth_ = width;
    outputHeight_ = height;
    const auto w = std::max(1u, static_cast<uint32_t>(width * renderScale_));
    const auto h = std::max(1u, static_cast<uint32_t>(height * renderScale_));
    if (sceneTarget_ && (sceneTarget_->width() != w || sceneTarget_->height() != h))
        resetHistory();
    if (!sceneTarget_)
        sceneTarget_ = std::make_unique<Framebuffer>(Framebuffer::Kind::Surface, w, h, true);
    else
        sceneTarget_->resize(w, h);
}

void ForwardRenderer::setRenderScale(float scale)
{
    renderScale_ = std::clamp(scale, 0.5f, 1.0f);
    setOutputSize(outputWidth_, outputHeight_);
}

void ForwardRenderer::ensureOutputTarget()
{
    // Called once per frame: if the app never pushed a size (e.g. a tool
    // embedding the renderer), fall back to the window's framebuffer size.
    if (sceneTarget_)
        return;
    setOutputSize(outputWidth_ ? outputWidth_ : 1, outputHeight_ ? outputHeight_ : 1);
}

DebugDraw &ForwardRenderer::debugDraw() { return *debugDraw_; }

bool ForwardRenderer::tickShaderReloads()
{
    bool any = false;
    any |= litShader_->checkHotReload();
    any |= litSkinnedShader_->checkHotReload();
    any |= debugShader_->checkHotReload();
    any |= shadowShader_->checkHotReload();
    any |= shadowSkinnedShader_->checkHotReload();
    any |= postShader_->checkHotReload();
    any |= bloomShader_->checkHotReload();
    any |= volumeShader_->checkHotReload();
    any |= pointShadowShader_->checkHotReload();
    any |= pointShadowSkinnedShader_->checkHotReload();
    any |= temporalShader_->checkHotReload();
    any |= reflectionShader_->checkHotReload();
    if (any) {
        resetHistory();
        for (auto &slot : spotShadows_) slot.valid = false;
        pointShadow_.valid.fill(false);
        for (auto &probe : probes_) probe.ready = false;
    }
    return any;
}

void ForwardRenderer::setWireframe(bool on) { wireframe_ = on; }
void ForwardRenderer::setDebugDrawEnabled(bool on) { debugDrawEnabled_ = on; }

void ForwardRenderer::tuneFlashlight(float innerDeg, float outerDeg, float range, float intensity)
{
    // Gameplay-side flavor (battery flicker etc.) replaces these direct calls
    // later; the hook exists so that swap doesn't touch render code first.
    flashlightInnerDeg_ = innerDeg;
    flashlightOuterDeg_ = outerDeg;
    flashlightRange_ = range;
    flashlightIntensity_ = intensity;
}

// ---- frustum ------------------------------------------------------------------
namespace {
// Gribb-Hartmann world-space plane extraction from the combined matrix.
struct Frustum {
    glm::vec4 planes[6];

    static Frustum fromViewProjection(const glm::mat4 &vp)
    {
        Frustum f;
        f.planes[0] = glm::row(vp, 3) + glm::row(vp, 0); // left
        f.planes[1] = glm::row(vp, 3) - glm::row(vp, 0); // right
        f.planes[2] = glm::row(vp, 3) + glm::row(vp, 1); // bottom
        f.planes[3] = glm::row(vp, 3) - glm::row(vp, 1); // top
        f.planes[4] = glm::row(vp, 3) + glm::row(vp, 2); // near
        f.planes[5] = glm::row(vp, 3) - glm::row(vp, 2); // far
        for (auto &p : f.planes)
            p /= glm::length(glm::vec3(p));
        return f;
    }

    bool intersectsBox(const glm::vec3 &center, const glm::vec3 &extent) const
    {
        for (const auto &p : planes) {
            if (glm::dot(glm::vec3(p), center) + p.w < -glm::dot(glm::abs(glm::vec3(p)), extent))
                return false;
        }
        return true;
    }
};
} // namespace

// ---- queue ------------------------------------------------------------------
void ForwardRenderer::buildQueue(Scene &scene, const Camera &camera)
{
    queue_.clear();
    stats_.itemsSubmitted = stats_.itemsDrawn = stats_.itemsCulled = 0;
    stats_.shadowDraws = 0;
    stats_.triangles = 0;

    const Frustum frustum = Frustum::fromViewProjection(camera.viewProjection());

    auto enqueue = [&](EntityId id, const Mesh *mesh, const Material *material,
                       bool castShadow, const std::vector<glm::mat4> *bones) {
        ++stats_.itemsSubmitted;
        if (!mesh)
            return;
        RenderItem item;
        item.mesh = mesh;
        item.material = material;
        item.world = scene.worldMatrix(id);
        item.castsShadow = castShadow;
        item.bones = bones;
        item.dynamic = bones != nullptr;
        for (auto parent = id; parent != kInvalidEntity && !item.dynamic; parent = scene.parentOf(parent))
            item.dynamic = scene.name(parent).find("PROP_") == 0;

        // Conservative world-space AABB from the transformed local extents.
        const glm::vec3 lmin = mesh->minBounds();
        const glm::vec3 lmax = mesh->maxBounds();
        const auto localCenter = (lmin + lmax) * 0.5f;
        const auto localHalf = (lmax - lmin) * 0.5f;
        const auto include = [&](const glm::mat4 &transform, glm::vec3 &low, glm::vec3 &high) {
            const auto center = glm::vec3(transform * glm::vec4(localCenter, 1.0f));
            const auto extent = glm::abs(glm::vec3(transform[0])) * localHalf.x
                + glm::abs(glm::vec3(transform[1])) * localHalf.y
                + glm::abs(glm::vec3(transform[2])) * localHalf.z;
            low = glm::min(low, center - extent);
            high = glm::max(high, center + extent);
        };
        glm::vec3 low(std::numeric_limits<float>::max()), high(-std::numeric_limits<float>::max());
        // Linear skin weights are nonnegative and normalized. The union of
        // all bone-transformed bounds encloses their convex combinations.
        if (bones)
            for (const auto &bone : *bones)
                include(item.world * bone, low, high);
        else
            include(item.world, low, high);
        item.centerWorld = (low + high) * 0.5f;
        item.worldExtent = (high - low) * 0.5f;
        item.cameraVisible = frustum.intersectsBox(item.centerWorld, item.worldExtent);
        if (!item.cameraVisible)
            ++stats_.itemsCulled;
        if (item.cameraVisible || item.castsShadow)
            queue_.push_back(item);
    };

    scene.view<MeshRenderer>([&](EntityId id, MeshRenderer &mr) {
        if (!mr.visible)
            return;
        enqueue(id, mr.mesh, mr.material, mr.castShadow, nullptr);
    });

    scene.view<SkinnedMeshRenderer>([&](EntityId id, SkinnedMeshRenderer &sr) {
        if (!sr.visible)
            return;
        // Only deform when the same entity has a populated Animator; without
        // one the mesh renders in its bind pose through the static path.
        Animator *anim = scene.find<Animator>(id);
        const std::vector<glm::mat4> *bones = nullptr;
        if (anim && !anim->boneMatrices.empty())
            bones = &anim->boneMatrices;
        enqueue(id, sr.mesh, sr.material, sr.castShadow, bones);
    });
}

// ---- lighting uniform upload ---------------------------------------------------
void ForwardRenderer::setupLightingUniforms(const Shader &shader, const Camera &camera)
{
    shader.setUniform("uAmbientColor", ambient_.color * ambient_.intensity);

    // Cache names once so per-frame light updates do not construct strings.
    static constexpr int kMaxPointLights = 8;
    static const auto pointUniforms = [] {
        std::array<std::array<std::string, 3>, kMaxPointLights> names;
        for (int i = 0; i < kMaxPointLights; ++i) {
            const std::string prefix = "uPointLights[" + std::to_string(i) + "]";
            names[i] = {prefix + ".position", prefix + ".color", prefix + ".range"};
        }
        return names;
    }();
    const auto uploaded = localPointCount_;
    int pointShadow = -1;
    for (size_t i = 0; i < uploaded; ++i) {
        const auto &names = pointUniforms[i];
        const auto &light = localPoints_[i];
        shader.setUniform(names[0].c_str(), light.position);
        shader.setUniform(names[1].c_str(), light.color);
        shader.setUniform(names[2].c_str(), light.range);
        if (light.shadow && !capturingProbe_) pointShadow = static_cast<int>(i);
    }
    shader.setUniform("uPointShadowIndex", pointShadow);
    glBindTextureUnit(16, pointShadow >= 0 ? pointShadow_.cube : 0);
    glBindSampler(16, shadowSampler_);
    // float nuance: the shader declares uPointLightCount as float (loops with
    // float(i) >= count); uploading an int to a float uniform is a silent GL
    // INVALID_OPERATION, so cast explicitly.
    shader.setUniform("uPointLightCount", static_cast<float>(uploaded));

    // Flashlight: origin/direction come from the SpotLight entity when one
    // exists (gameplay aims it); otherwise fall back to the camera basis so a
    // scene without a flashlight entity still renders sensibly.
    const glm::vec3 lightPos = hasFlashlightTransform_ ? flashlightPos_ : camera.position;
    const glm::vec3 lightDir = hasFlashlightTransform_ ? flashlightDir_ : camera.forward();
    shader.setUniform("uSpotLightPosition", lightPos);
    shader.setUniform("uSpotLightDirection", lightDir);
    shader.setUniform("uSpotLightColor", capturingProbe_ ? glm::vec3(0) : flashlightColor_ * flashlightIntensity_);
    shader.setUniform("uSpotLightInnerCos", std::cos(glm::radians(flashlightInnerDeg_)));
    shader.setUniform("uSpotLightOuterCos", std::cos(glm::radians(flashlightOuterDeg_)));
    shader.setUniform("uSpotLightRange", flashlightRange_);

    shader.setUniform("uLightViewProj", flashlightViewProj_);
    shader.setUniform("uShadowBias", shadowBias_);
    shader.setUniform("uShadows", flashlightShadows_ && !capturingProbe_ ? 1 : 0);
    shader.setUniform("uShadowMap", 15);
    shader.setUniform("uLocalSpotCount", static_cast<int>(localSpotCount_));
    for (size_t i = 0; i < spotShadows_.size(); ++i) {
        const auto &slot = spotShadows_[i];
        const auto matrix = "uLocalShadowVP[" + std::to_string(i) + "]";
        shader.setUniform(matrix.c_str(), slot.vp);
        glBindTextureUnit(13 + static_cast<GLuint>(i), slot.map ? slot.map->depth() : 0);
        glBindSampler(13 + static_cast<GLuint>(i), shadowSampler_);
    }
    for (size_t i = 0; i < localSpotCount_; ++i) {
        const auto &spot = localSpots_[i];
        const std::string prefix = "uLocalSpots[" + std::to_string(i) + "]";
        shader.setUniform((prefix + ".position").c_str(), spot.position);
        shader.setUniform((prefix + ".direction").c_str(), spot.direction);
        shader.setUniform((prefix + ".color").c_str(), spot.color);
        shader.setUniform((prefix + ".coneRange").c_str(), spot.coneAndRange);
        shader.setUniform((prefix + ".shadowSlot").c_str(), capturingProbe_ ? -1 : spot.shadow);
    }
}

void ForwardRenderer::render(Scene &scene, Camera &camera)
{
    buildQueue(scene, camera);
    gatherEnvironment(scene, camera);

    // ---- flashlight light state (from the SpotLight component, if any) ----
    flashlightInnerDeg_ = 18.0f;
    flashlightOuterDeg_ = 24.0f;
    flashlightRange_ = 45.0f;
    flashlightIntensity_ = 8.0f;
    flashlightColor_ = glm::vec3(1.0f, 0.96f, 0.88f);
    hasFlashlightTransform_ = false;
    flashlightShadows_ = true;
    localSpotCount_ = 0;
    scene.view<SpotLight>([&](EntityId id, const SpotLight &sl) {
        const glm::mat4 &world = scene.worldMatrix(id);
        const glm::vec3 dir = -glm::vec3(world[2]);
        const float len = glm::length(dir);
        if (len <= 1e-5f)
            return;
        if (scene.name(id) == "flashlight") {
            flashlightInnerDeg_ = sl.innerConeDeg;
            flashlightOuterDeg_ = sl.outerConeDeg;
            flashlightRange_ = sl.range;
            flashlightIntensity_ = sl.intensity;
            flashlightColor_ = sl.color;
            flashlightShadows_ = sl.castsShadows;
            flashlightPos_ = glm::vec3(world[3]);
            flashlightDir_ = dir / len;
            hasFlashlightTransform_ = true;
        } else if (localSpotCount_ < localSpots_.size()) {
            localSpots_[localSpotCount_++] = {id, glm::vec3(world[3]), dir/len,
                sl.color*sl.intensity, {std::cos(glm::radians(sl.outerConeDeg)),
                    std::cos(glm::radians(sl.innerConeDeg)), sl.range}, sl.castsShadows};
        }
    });

    ensureOutputTarget();
    drawViewProj_ = camera.viewProjection();
    rasterOffset_ = glm::vec2(0.0f);
    temporalActive_ = post_.enabled && post_.temporalAA && !wireframe_ && !debugDrawEnabled_ && !drawBones_;
    if (!temporalActive_) {
        resetHistory();
        stats_.temporalFrames = 0;
    }
    if (temporalActive_) {
        const auto halton = [](unsigned index, unsigned base) {
            float result = 0, place = 1;
            for (; index; index /= base) {
                place /= static_cast<float>(base);
                result += static_cast<float>(index % base) * place;
            }
            return result;
        };
        const auto sample = jitterFrame_++ % 8 + 1;
        rasterOffset_ = glm::vec2(0.5f - halton(sample, 2), 0.5f - halton(sample, 3))
            / glm::vec2(sceneTarget_->width(), sceneTarget_->height());
        // In this projection clip.w is -view.z, so [2] shifts NDC in the opposite direction.
        auto projection = camera.projection();
        projection[2][0] -= 2.0f * rasterOffset_.x;
        projection[2][1] -= 2.0f * rasterOffset_.y;
        drawViewProj_ = projection * camera.view();
    }
    stats_.cachedShadowFaces = stats_.updatedShadowFaces = 0;
    GpuFrame *timing = nullptr;
    if (profileGpu_) {
        auto &slot = gpuFrames_[gpuFrame_++ % gpuFrames_.size()];
        if (!slot.marks.front())
            glCreateQueries(GL_TIMESTAMP, static_cast<GLsizei>(slot.marks.size()), slot.marks.data());
        if (slot.pending) {
            GLint available = 0;
            glGetQueryObjectiv(slot.marks.back(), GL_QUERY_RESULT_AVAILABLE, &available);
            if (available) {
                GLuint64 previous = 0;
                glGetQueryObjectui64v(slot.marks.front(), GL_QUERY_RESULT, &previous);
                for (size_t i = 0; i < stats_.gpuMs.size(); ++i) {
                    GLuint64 next = 0;
                    glGetQueryObjectui64v(slot.marks[i + 1], GL_QUERY_RESULT, &next);
                    stats_.gpuMs[i] = static_cast<double>(next - previous) * 1e-6;
                    previous = next;
                }
                ++stats_.gpuSamples;
                slot.pending = false;
            }
        }
        if (!slot.pending)
            timing = &slot;
    }
    const auto mark = [&](size_t i) {
        if (timing)
            glQueryCounter(timing->marks[i], GL_TIMESTAMP);
    };

    mark(0);
    if (flashlightShadows_)
        shadowPass(camera);
    updateLocalShadows(camera);
    mark(1);
    captureProbes(scene, camera);
    glBindTextureUnit(15, shadowTarget_->depth());
    glBindSampler(15, shadowSampler_);
    mainPass(scene, camera);
    mark(2);
    stats_.screenReflections = false;
    if (post_.enabled) {
        if (temporalActive_) resolveTemporal(camera);
        traceReflections(camera);
        effectsPass(camera);
    }
    mark(3);
    postPass(camera);
    mark(4);
    for (GLuint unit : {13u, 14u, 15u, 16u}) {
        glBindSampler(unit, 0);
        glBindTextureUnit(unit, 0);
    }
    if (timing)
        timing->pending = true;
}

void ForwardRenderer::shadowPass(const Camera &camera)
{
    // Shadow camera: the flashlight's own cone, originating at the light
    // (not at the eye), so the hand offset produces real parallax shadows.
    const float fov = 2.0f * flashlightOuterDeg_;
    const glm::vec3 lightPos = hasFlashlightTransform_ ? flashlightPos_ : camera.position;
    const glm::vec3 lightDir = hasFlashlightTransform_ ? flashlightDir_ : camera.forward();
    // lookAt degenerates when the view direction is parallel to up.
    const glm::vec3 up = std::abs(lightDir.y) > 0.98f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
    glm::mat4 lightView = glm::lookAt(lightPos, lightPos + lightDir, up);
    glm::mat4 lightProj = glm::perspective(glm::radians(fov), 1.0f, 0.1f, flashlightRange_);
    flashlightViewProj_ = lightProj * lightView;

    shadowTarget_->bind();
    drawShadowMap(flashlightViewProj_, *shadowShader_, *shadowSkinnedShader_);
}

void ForwardRenderer::drawShadowMap(const glm::mat4 &vp, const Shader &rigid, const Shader &skinned,
    const glm::vec3 *point, float range)
{
    const Frustum lightFrustum = Frustum::fromViewProjection(vp);
    // State is set per pass instead of inherited: postPass disables depth
    // testing and the shadow pass runs before mainPass re-enables it, so
    // without this the depth map was cleared and never written (no shadows).
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glClear(GL_DEPTH_BUFFER_BIT);

    const Material *shadowMaterial = nullptr;
    const auto drawItem = [&](const RenderItem &item, const Shader &shader) {
        if (!item.castsShadow || !lightFrustum.intersectsBox(item.centerWorld, item.worldExtent))
            return;
        if (item.bones)
            uploadBones(*item.bones);
        if (!item.material)
            return;
        if (shadowMaterial != item.material) {
            shadowMaterial = item.material;
            if (shadowMaterial->doubleSided) glDisable(GL_CULL_FACE);
            else glEnable(GL_CULL_FACE);
            shader.setUniform("uAlphaCutoff", shadowMaterial->alphaCutoff);
            shader.setUniform("uOpacity", shadowMaterial->opacity);
            shader.setUniform("uHasBaseColorTex", shadowMaterial->baseColorTex ? 1 : 0);
            if (shadowMaterial->baseColorTex)
                shadowMaterial->baseColorTex->bind(3);
        }
        glFrontFace(glm::determinant(glm::mat3(item.world)) < 0.0f ? GL_CW : GL_CCW);
        shader.setUniform("uModel", item.world);
        item.mesh->bind();
        glDrawElements(GL_TRIANGLES, item.mesh->indexCount(), item.mesh->indexType(), nullptr);
        ++stats_.shadowDraws;
    };

    // Static casters first with the static program.
    const auto bind = [&](const Shader &shader) {
        shader.bind();
        shader.setUniform("uLightViewProj", vp);
        if (point) {
            shader.setUniform("uLightPosition", *point);
            shader.setUniform("uLightRange", range);
        }
    };
    bind(rigid);
    for (const RenderItem &item : queue_)
        if (!item.bones)
            drawItem(item, rigid);

    // Deformed casters need the SKINNED variant (its bone UBO lookup must
    // match the lit mesh exactly).
    if (std::any_of(queue_.begin(), queue_.end(), [](const RenderItem &item) { return item.bones && item.castsShadow; })) {
        shadowMaterial = nullptr;
        bind(skinned);
        for (const RenderItem &item : queue_)
            if (item.bones)
                drawItem(item, skinned);
    }

    glFrontFace(GL_CCW);
    Framebuffer::bindDefault();
}

namespace {
constexpr glm::vec3 cubeDirections[] = {{1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1}};
constexpr glm::vec3 cubeUps[] = {{0,-1,0}, {0,-1,0}, {0,0,1}, {0,0,-1}, {0,-1,0}, {0,-1,0}};
}

void ForwardRenderer::updateLocalShadows(const Camera &camera)
{
    // An unchanged light and unchanged casters inside its frustum reuse the
    // map, even when the player moves. Hash poses only for intersecting meshes.
    const auto signature = [&](const glm::mat4 &vp, EntityId light) {
        uint64_t hash = 14695981039346656037ull;
        const auto add = [&](uint64_t value) { hash = (hash ^ value) * 1099511628211ull; };
        const auto addMatrix = [&](const glm::mat4 &matrix) {
            for (glm::length_t column = 0; column < 4; ++column)
                for (glm::length_t row = 0; row < 4; ++row)
                    add(std::bit_cast<uint32_t>(matrix[column][row]));
        };
        add(light);
        addMatrix(vp);
        const auto frustum = Frustum::fromViewProjection(vp);
        for (const auto &item : queue_) {
            if (!item.castsShadow || !item.material || !frustum.intersectsBox(item.centerWorld, item.worldExtent))
                continue;
            add(item.mesh->handle());
            addMatrix(item.world);
            add(item.material->doubleSided);
            add(std::bit_cast<uint32_t>(item.material->alphaCutoff));
            if (item.material->alphaCutoff >= 0) {
                add(std::bit_cast<uint32_t>(item.material->opacity));
                add(item.material->baseColorTex ? item.material->baseColorTex->handle() : 0);
            }
            if (item.bones)
                for (const auto &bone : *item.bones) addMatrix(bone);
        }
        return hash;
    };

    std::array<LocalSpot *, 4> candidates{};
    size_t candidateCount = 0;
    for (size_t i = 0; i < localSpotCount_; ++i)
        if (localSpots_[i].castsShadow && localSpots_[i].coneAndRange.z > 0.08f)
            candidates[candidateCount++] = &localSpots_[i];
    const auto score = [&](const LocalSpot *light) {
        const auto offset = light->position - camera.position;
        const float distance2 = glm::dot(offset, offset);
        const bool resident = std::any_of(spotShadows_.begin(), spotShadows_.end(),
            [&](const SpotShadow &slot) { return slot.valid && slot.id == light->id; });
        return distance2 * (resident ? 0.84f : 1.0f);
    };
    std::sort(candidates.begin(), candidates.begin() + candidateCount,
        [&](const LocalSpot *a, const LocalSpot *b) { return score(a) < score(b); });
    const auto count = std::min({candidateCount, spotShadows_.size(), static_cast<size_t>(std::max(0, spotShadowBudget_))});
    std::array<int, 2> slots{-1, -1};
    std::array<bool, 2> occupied{};
    for (size_t i = 0; i < count; ++i) {
        for (size_t j = 0; j < spotShadows_.size(); ++j) {
            if (spotShadows_[j].valid && spotShadows_[j].id == candidates[i]->id) {
                slots[i] = static_cast<int>(j);
                occupied[j] = true;
                break;
            }
        }
    }
    for (size_t i = 0; i < count; ++i) {
        if (slots[i] < 0) {
            slots[i] = static_cast<int>(std::find(occupied.begin(), occupied.end(), false) - occupied.begin());
            occupied[slots[i]] = true;
        }
        auto &light = *candidates[i];
        light.shadow = slots[i];
        auto &slot = spotShadows_[light.shadow];
        const auto up = std::abs(light.direction.y) > 0.98f ? glm::vec3(0,0,1) : glm::vec3(0,1,0);
        const float fov = std::clamp(2 * std::acos(std::clamp(light.coneAndRange.x, -1.0f, 1.0f)), 0.05f, 3.10f);
        const auto vp = glm::perspective(fov, 1.0f, 0.06f, light.coneAndRange.z)
            * glm::lookAt(light.position, light.position + light.direction, up);
        const auto key = signature(vp, light.id);
        if (slot.valid && slot.signature == key) {
            ++stats_.cachedShadowFaces;
            continue;
        }
        if (!slot.map)
            slot.map = std::make_unique<Framebuffer>(Framebuffer::Kind::DepthOnly, 1024, 1024);
        slot.map->bind();
        drawShadowMap(vp, *shadowShader_, *shadowSkinnedShader_);
        slot.vp = vp;
        slot.id = light.id;
        slot.signature = key;
        slot.valid = true;
        ++stats_.updatedShadowFaces;
    }

    if (!pointShadows_)
        return;
    auto end = localPoints_.begin() + localPointCount_;
    auto light = std::find_if(localPoints_.begin(), end,
        [](const LocalPoint &p) { return p.castsShadow && p.range > 0.08f; });
    if (light == end)
        return;
    constexpr GLsizei size = 384;
    if (!pointShadow_.cube) {
        glCreateTextures(GL_TEXTURE_CUBE_MAP, 1, &pointShadow_.cube);
        glTextureStorage2D(pointShadow_.cube, 1, GL_DEPTH_COMPONENT24, size, size);
        glCreateFramebuffers(1, &pointShadow_.fbo);
        glNamedFramebufferDrawBuffer(pointShadow_.fbo, GL_NONE);
        glNamedFramebufferReadBuffer(pointShadow_.fbo, GL_NONE);
        glNamedFramebufferTextureLayer(pointShadow_.fbo, GL_DEPTH_ATTACHMENT, pointShadow_.cube, 0, 0);
        if (glCheckNamedFramebufferStatus(pointShadow_.fbo, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            throw std::runtime_error("Point-light shadow framebuffer incomplete");
    }
    light->shadow = true;
    const auto projection = glm::perspective(glm::half_pi<float>(), 1.0f, 0.06f, light->range);
    for (size_t face = 0; face < 6; ++face) {
        const auto vp = projection * glm::lookAt(light->position,
            light->position + cubeDirections[face], cubeUps[face]);
        const auto key = signature(vp, light->id);
        if (pointShadow_.valid[face] && pointShadow_.signatures[face] == key) {
            ++stats_.cachedShadowFaces;
            continue;
        }
        glNamedFramebufferTextureLayer(pointShadow_.fbo, GL_DEPTH_ATTACHMENT, pointShadow_.cube, 0, static_cast<GLint>(face));
        glBindFramebuffer(GL_FRAMEBUFFER, pointShadow_.fbo);
        glViewport(0, 0, size, size);
        drawShadowMap(vp, *pointShadowShader_, *pointShadowSkinnedShader_, &light->position, light->range);
        pointShadow_.valid[face] = true;
        pointShadow_.signatures[face] = key;
        ++stats_.updatedShadowFaces;
    }
}

void ForwardRenderer::bindLitUniforms(const Shader &shader, Camera &camera)
{
    shader.setUniform("uViewProj", drawViewProj_);
    shader.setUniform("uCameraPos", camera.position);
    setupLightingUniforms(shader, camera);
    bindEnvironment(shader);
}

void ForwardRenderer::mainPass(Scene &scene, Camera &camera)
{
    // Scene renders into the HDR target; postPass() composites it to the
    // swapchain. Explicit viewport: the shadow pass left it at the shadow-map
    // size, which would otherwise clip the main image.
    auto &target = capturingProbe_ ? *probeTarget_ : *sceneTarget_;
    target.bind();
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glDepthMask(GL_TRUE);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (!capturingProbe_) {
        constexpr GLfloat empty[] = {0.5f, 0.5f, 1.0f, 0.0f};
        glClearBufferfv(GL_COLOR, 1, empty);
    }
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);

    if (wireframe_)
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);

    // One shared lit shader (compiled static + SKINNED variants) draws
    // everything; Material::shader is only a hint for future multi-shader
    // materials, so a material without one still draws.
    auto drawItems = [&](const Shader &shader, bool skinnedPass) {
        shader.setUniform("uBaseColorTex", 3);
        shader.setUniform("uNormalTex", 4);
        shader.setUniform("uMetallicRoughnessTex", 5);
        shader.setUniform("uDetailTex", 6);
        shader.setUniform("uEmissiveMap", 8);
        shader.setUniform("uOcclusionMap", 9);
        const Material *boundMaterial = nullptr;
        const Mesh *boundMesh = nullptr;
        for (const RenderItem &item : queue_) {
            if (!item.cameraVisible || !item.material || (capturingProbe_ && item.dynamic)
                || (item.bones != nullptr) != skinnedPass)
                continue;
            if (skinnedPass)
                uploadBones(*item.bones);
            shader.setUniform("uModel", item.world);
            shader.setUniform("uReactive", item.dynamic ? 1 : 0);
            const auto worldBasis = glm::mat3(item.world);
            const float det = glm::determinant(worldBasis);
            if (std::abs(det) < 1e-10f)
                continue;
            glFrontFace(det < 0.0f ? GL_CW : GL_CCW);
            shader.setUniform("uNormalMatrix", glm::transpose(glm::inverse(worldBasis)));
            shader.setUniform("uTangentSign", det < 0.0f ? -1.0f : 1.0f);
            if (item.material != boundMaterial) {
                boundMaterial = item.material;
                shader.setUniform("uAlbedo", boundMaterial->albedo);
                shader.setUniform("uEmissive", boundMaterial->emissiveColor * boundMaterial->emissiveStrength);
                shader.setUniform("uRoughness", boundMaterial->roughness);
                shader.setUniform("uMetallic", boundMaterial->metallic);
                shader.setUniform("uAlphaCutoff", boundMaterial->alphaCutoff);
                shader.setUniform("uOpacity", boundMaterial->opacity);
                shader.setUniform("uNormalScale", boundMaterial->normalScale);
                shader.setUniform("uOcclusionStrength", boundMaterial->occlusionStrength);
                shader.setUniform("uUnlit", boundMaterial->unlit ? 1 : 0);
                shader.setUniform("uWetness", boundMaterial->wetness);
                shader.setUniform("uLightmapStrength", boundMaterial->lightmap ? boundMaterial->lightmapStrength : 0.0f);
                glBindTextureUnit(10, boundMaterial->lightmap ? boundMaterial->lightmap->handle() : 0);
                if (boundMaterial->doubleSided) glDisable(GL_CULL_FACE);
                else glEnable(GL_CULL_FACE);
                const auto bindMap = [&](const Texture *texture, GLuint unit, const char *flag) {
                    (texture ? texture : placeholder_.get())->bind(unit);
                    shader.setUniform(flag, texture ? 1 : 0);
                };
                bindMap(boundMaterial->baseColorTex, 3, "uHasBaseColorTex");
                bindMap(boundMaterial->normalTex, 4, "uHasNormalTex");
                bindMap(boundMaterial->emissiveTex, 8, "uHasEmissiveMap");
                bindMap(boundMaterial->occlusionTex, 9, "uHasOcclusionMap");
                bindMap(boundMaterial->metallicRoughnessTex, 5, "uHasMetallicRoughnessTex");
                (boundMaterial->detailTex ? boundMaterial->detailTex : placeholder_.get())->bind(6);
                shader.setUniform("uDetailScale", boundMaterial->detailTex ? boundMaterial->detailScale : 0.0f);
            }
            if (item.mesh != boundMesh) {
                boundMesh = item.mesh;
                boundMesh->bind();
            }
            glDrawElements(GL_TRIANGLES, item.mesh->indexCount(), item.mesh->indexType(), nullptr);
            stats_.triangles += item.mesh->indexCount() / 3;
            ++stats_.itemsDrawn;
        }
    };

    litShader_->bind();
    bindLitUniforms(*litShader_, camera);
    drawItems(*litShader_, false);

    if (std::any_of(queue_.begin(), queue_.end(), [](const RenderItem &item) { return item.cameraVisible && item.bones; })) {
        litSkinnedShader_->bind();
        bindLitUniforms(*litSkinnedShader_, camera);
        drawItems(*litSkinnedShader_, true);
    }

    glFrontFace(GL_CCW);
    glEnable(GL_CULL_FACE);
    if (wireframe_)
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

    // Debug draw lines + gizmos overlay, drawn even in wireframe mode.
    if (!capturingProbe_ && (debugDrawEnabled_ || drawBones_))
        drawDebugData(scene, camera);


}

void ForwardRenderer::effectsPass(const Camera &camera)
{
    if (post_.bloom <= 0.0f && (post_.volumeSteps <= 0 || (!flashlightShadows_ && fogCount_ == 0)))
        return;

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glBindVertexArray(fullscreenVao_);
    const auto target = [&](std::unique_ptr<Framebuffer> &fb, Framebuffer::Kind kind,
                            uint32_t divisor) {
        const uint32_t w = std::max(1u, (sceneTarget_->width() + divisor - 1) / divisor);
        const uint32_t h = std::max(1u, (sceneTarget_->height() + divisor - 1) / divisor);
        if (!fb)
            fb = std::make_unique<Framebuffer>(kind, w, h, true);
        else
            fb->resize(w, h);
        fb->bind();
    };

    if (post_.volumeSteps > 0 && (flashlightShadows_ || fogCount_ != 0)) {
        // Alpha stores linear distance for the depth-aware upsampler.
        target(volumeTarget_, Framebuffer::Kind::ColorAlpha, 4);
        volumeShader_->bind();
        glBindTextureUnit(0, sceneTarget_->depth());
        volumeShader_->setUniform("uInvViewProj", glm::inverse(camera.viewProjection()));
        volumeShader_->setUniform("uRasterOffset", rasterOffset_);
        volumeShader_->setUniform("uLightViewProj", flashlightViewProj_);
        volumeShader_->setUniform("uEye", camera.position);
        volumeShader_->setUniform("uLightPos", hasFlashlightTransform_ ? flashlightPos_ : camera.position);
        volumeShader_->setUniform("uLightDir", hasFlashlightTransform_ ? flashlightDir_ : camera.forward());
        volumeShader_->setUniform("uLightColor", flashlightColor_ * flashlightIntensity_);
        volumeShader_->setUniform("uCone", glm::vec2(std::cos(glm::radians(flashlightOuterDeg_)),
            std::cos(glm::radians(flashlightInnerDeg_))));
        volumeShader_->setUniform("uRange", flashlightRange_);
        volumeShader_->setUniform("uSteps", post_.volumeSteps);
        volumeShader_->setUniform("uFlashlightShadows", flashlightShadows_ ? 1 : 0);
        volumeShader_->setUniform("uFogCount", static_cast<int>(fogCount_));
        for (size_t i = 0; i < fogCount_; ++i) {
            const auto name = "uFog[" + std::to_string(i) + "]";
            volumeShader_->setUniform((name + ".worldToBox").c_str(), fogVolumes_[i].worldToBox);
            volumeShader_->setUniform((name + ".colorDensity").c_str(), glm::vec4(fogVolumes_[i].color, fogVolumes_[i].density));
        }
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }

    if (post_.bloom > 0.0f) {
        bloomShader_->bind();
        GLuint source = sceneColor();
        for (size_t i = 0; i < bloomTargets_.size(); ++i) {
            // Quarter resolution first, then progressively wider bloom.
            target(bloomTargets_[i], Framebuffer::Kind::ColorOnly, 4u << i);
            glBindTextureUnit(0, source);
            bloomShader_->setUniform("uMode", i == 0 ? 0 : 1);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            source = bloomTargets_[i]->colorAttachment();
        }
        glEnable(GL_BLEND);
        glBlendEquation(GL_FUNC_ADD);
        glBlendFunc(GL_ONE, GL_ONE);
        bloomShader_->setUniform("uMode", 2);
        for (size_t i = bloomTargets_.size() - 1; i > 0; --i) {
            bloomTargets_[i - 1]->bind();
            glBindTextureUnit(0, bloomTargets_[i]->colorAttachment());
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
        glDisable(GL_BLEND);
    }
    glBindVertexArray(0);
}

void ForwardRenderer::postPass(const Camera &camera)
{
    if (!sceneTarget_)
        return;

    // No post: show the HDR scene directly (blit handles format differences).
    if (!post_.enabled) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, sceneTarget_->fbo());
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glBlitFramebuffer(0, 0, static_cast<GLint>(sceneTarget_->width()),
            static_cast<GLint>(sceneTarget_->height()), 0, 0,
            static_cast<GLint>(outputWidth_), static_cast<GLint>(outputHeight_),
            GL_COLOR_BUFFER_BIT, GL_LINEAR);
        Framebuffer::bindDefault();
        glViewport(0, 0, static_cast<GLint>(outputWidth_), static_cast<GLint>(outputHeight_));
        return;
    }

    Framebuffer::bindDefault();
    glViewport(0, 0, static_cast<GLint>(outputWidth_), static_cast<GLint>(outputHeight_));
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    postShader_->bind();
    glBindTextureUnit(0, sceneColor());
    glBindTextureUnit(12, reflectionsReady_ ? reflectionTarget_->colorAttachment() : 0);
    postShader_->setUniform("uHasReflections", reflectionsReady_ ? 1 : 0);
    glBindTextureUnit(1, sceneTarget_->depth());
    postShader_->setUniform("uScene", 0);
    postShader_->setUniform("uDepth", 1);
    postShader_->setUniform("uRasterOffset", rasterOffset_);
    postShader_->setUniform("uNear", camera.nearPlane);
    postShader_->setUniform("uFar", camera.farPlane);
    postShader_->setUniform("uRayScale", glm::vec2(1.0f / camera.projection()[0][0],
        1.0f / camera.projection()[1][1]));
    const bool bloomEnabled = post_.bloom > 0.0f && bloomTargets_[0];
    const bool volumeEnabled = post_.volumeSteps > 0 && (flashlightShadows_ || fogCount_ != 0) && volumeTarget_;
    glBindTextureUnit(2, bloomEnabled ? bloomTargets_[0]->colorAttachment() : 0);
    glBindTextureUnit(7, volumeEnabled ? volumeTarget_->colorAttachment() : 0);
    postShader_->setUniform("uBloomStrength", bloomEnabled ? post_.bloom : 0.0f);
    postShader_->setUniform("uHasVolume", volumeEnabled ? 1 : 0);
    postShader_->setUniform("uExposure", post_.exposure);
    postShader_->setUniform("uTime", timeSeconds_);
    postShader_->setUniform("uVignette", post_.vignette);
    postShader_->setUniform("uGrain", post_.grain);
    postShader_->setUniform("uFogColor", post_.fogColor);
    postShader_->setUniform("uFogDensity", post_.fogDensity);

    glBindVertexArray(fullscreenVao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    glBindTextureUnit(0, 0);
    glBindTextureUnit(1, 0);
}


void ForwardRenderer::drawDebugData(Scene &scene, Camera &camera)
{
    if (debugDrawEnabled_) {
        // Lights: small sphere at point-light positions (yellow) + flashlight
        // cone tip (magenta) so a missing light is obvious on the overlay.
        scene.view<PointLight>([&](EntityId id, const PointLight &pl) {
            debugDraw_->addSphere(
                glm::vec3(scene.worldMatrix(id) * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f)),
                pl.radius, glm::vec3(1.0f, 0.85f, 0.1f));
            debugDraw_->addAxes(glm::vec3(scene.worldMatrix(id) * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f)), 0.35f);
        });
        scene.view<SpotLight>([&](EntityId id, const SpotLight &) {
            const glm::vec3 pos = glm::vec3(scene.worldMatrix(id) * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
            debugDraw_->addSphere(pos, 0.1f, glm::vec3(1.0f, 0.2f, 1.0f));
        });
        // Static collider outlines (green); shape matches gameplay collision
        // exactly because both derive from the same AABBCollider data.
        scene.view<AABBCollider>([&](EntityId id, const AABBCollider &col) {
            debugDraw_->addBox(col.worldMin, col.worldMax, glm::vec3(0.2f, 1.0f, 0.4f));
        });
        // Trigger volumes (blue, brighter while the player is inside): makes it
        // obvious what "the red circle" is and whether it fired.
        scene.view<TriggerVolume>([&](EntityId, const TriggerVolume &trigger) {
            const glm::vec3 color = trigger.playerInside ? glm::vec3(0.3f, 0.7f, 1.0f)
                                                        : glm::vec3(0.15f, 0.3f, 0.8f);
            debugDraw_->addBox(trigger.worldMin, trigger.worldMax, color);
        });
    }

    // Skeleton overlay (cyan bones, yellow joints). Joints come from the
    // Animator's last evaluated pose, in mesh-node space, so the entity's world
    // matrix places them exactly where the skinned vertices ended up.
    //
    // Drawn in x-ray (overlay) mode on purpose: a depth-tested skeleton is
    // mostly hidden inside the mesh, so the visible fragments look detached.
    // Visible-through-the-body, the whole rig can be compared to the skin at a
    // glance -- which is the question this overlay exists to answer.
    debugDraw_->setOverlayMode(drawBones_);
    if (drawBones_) {
        scene.view<Animator>([&](EntityId id, const Animator &anim) {
            if (!anim.skeleton || anim.jointWorldPositions.empty())
                return;
            const Skeleton &sk = *anim.skeleton;
            const glm::mat4 &world = scene.worldMatrix(id);
            const glm::vec3 boneColor(0.2f, 0.9f, 1.0f);
            const glm::vec3 jointColor(1.0f, 0.9f, 0.2f);
            for (size_t j = 0; j < anim.jointWorldPositions.size(); ++j) {
                const glm::vec3 p = glm::vec3(world * glm::vec4(anim.jointWorldPositions[j], 1.0f));
                debugDraw_->addSphere(p, 0.02f, jointColor, 6);
                const int parentJoint = sk.jointParents[j];
                if (parentJoint >= 0 && static_cast<size_t>(parentJoint) < anim.jointWorldPositions.size()) {
                    const glm::vec3 pp = glm::vec3(
                        world * glm::vec4(anim.jointWorldPositions[static_cast<size_t>(parentJoint)], 1.0f));
                    debugDraw_->addLine(p, pp, boneColor);
                }
            }
        });
    }
    debugDraw_->setOverlayMode(false);

    debugDraw_->flush(camera.viewProjection());
}

} // namespace horror
