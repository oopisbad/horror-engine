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
    postShader_ = std::make_unique<Shader>("post.vert", "post.frag", "post");

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
    if (outputWidth_ == width && outputHeight_ == height && sceneTarget_)
        return;
    outputWidth_ = width;
    outputHeight_ = height;
    if (!sceneTarget_)
        sceneTarget_ = std::make_unique<Framebuffer>(Framebuffer::Kind::ColorDepth,
            width, height, true);
    else
        sceneTarget_->resize(width, height);
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

    bool intersectsSphere(const glm::vec3 &center, float radius) const
    {
        for (const auto &p : planes) {
            if (glm::dot(glm::vec3(p), center) + p.w < -radius)
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

        // Bounding sphere from local AABB: center transformed directly, radius
        // as the farthest corner distance. Coarser than 6-DoF OBB, plenty for
        // the primitive sizes in play (and scale-invariant). Skinned meshes
        // use the rest-pose bounds (poses are near the rest volume here).
        const glm::vec3 lmin = mesh->minBounds();
        const glm::vec3 lmax = mesh->maxBounds();
        const glm::vec3 center = glm::vec3(item.world * glm::vec4((lmin + lmax) * 0.5f, 1.0f));
        const glm::vec3 corner = glm::vec3(item.world * glm::vec4(lmax, 1.0f));
        item.centerWorld = center;
        item.boundingRadius = glm::length(corner - center);

        if (frustum.intersectsSphere(center, item.boundingRadius))
            queue_.push_back(item);
        else
            ++stats_.itemsCulled;
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

    // Point lights walk the scene's store directly; up to kMaxPointLights
    // (compile-time constant in mesh.frag -- keep the two in sync).
    static constexpr int kMaxPointLights = 8;
    int uploaded = 0;
    sceneForUniforms_->view<PointLight>([&](EntityId id, const PointLight &pl) {
        if (uploaded >= kMaxPointLights)
            return;
        std::string prefix = "uPointLights[" + std::to_string(uploaded) + "]";
        const glm::vec3 pos = glm::vec3(sceneForUniforms_->worldMatrix(id) * glm::vec4(0, 0, 0, 1));
        shader.setUniform((prefix + ".position").c_str(), pos);
        shader.setUniform((prefix + ".color").c_str(), pl.color * pl.intensity);
        shader.setUniform((prefix + ".range").c_str(), pl.range);
        ++uploaded;
    });
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
    shader.setUniform("uSpotLightColor", flashlightColor_ * flashlightIntensity_);
    shader.setUniform("uSpotLightInnerCos", std::cos(glm::radians(flashlightInnerDeg_)));
    shader.setUniform("uSpotLightOuterCos", std::cos(glm::radians(flashlightOuterDeg_)));
    shader.setUniform("uSpotLightRange", flashlightRange_);

    shader.setUniform("uLightViewProj", flashlightViewProj_);
    shader.setUniform("uShadowBias", shadowBias_);
    glActiveTexture(GL_TEXTURE0 + 15);
    glBindTextureUnit(15, shadowTarget_->depth());
    shader.setUniform("uShadowMap", 15);
}

void ForwardRenderer::render(Scene &scene, Camera &camera)
{
    sceneForUniforms_ = &scene;
    buildQueue(scene, camera);

    // ---- flashlight light state (from the SpotLight component, if any) ----
    flashlightInnerDeg_ = 18.0f;
    flashlightOuterDeg_ = 24.0f;
    flashlightRange_ = 45.0f;
    flashlightIntensity_ = 8.0f;
    flashlightColor_ = glm::vec3(1.0f, 0.96f, 0.88f);
    hasFlashlightTransform_ = false;
    scene.view<SpotLight>([&](EntityId id, const SpotLight &sl) {
        flashlightInnerDeg_ = sl.innerConeDeg;
        flashlightOuterDeg_ = sl.outerConeDeg;
        flashlightRange_ = sl.range;
        flashlightIntensity_ = sl.intensity;
        flashlightColor_ = sl.color;
        // World matrix: translation = light origin, -Z column = light axis
        // (matches how the game builds the entity's rotation from the camera).
        const glm::mat4 &world = scene.worldMatrix(id);
        const glm::vec3 dir = -glm::vec3(world[2]);
        const float len = glm::length(dir);
        if (len > 1e-5f) {
            flashlightPos_ = glm::vec3(world[3]);
            flashlightDir_ = dir / len;
            hasFlashlightTransform_ = true;
        }
    });

    ensureOutputTarget();
    // Shadow pass renders only from the flashlight.
    shadowPass(scene, camera);
    mainPass(scene, camera);
    postPass();
}

void ForwardRenderer::shadowPass(Scene &scene, const Camera &camera)
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

    auto drawItem = [&](const RenderItem &item) {
        if (!item.castsShadow || !item.mesh)
            return;
        if (item.bones)
            uploadBones(*item.bones);
        shadowShader_->setUniform("uModel", item.world);
        item.mesh->bind();
        glDrawElements(GL_TRIANGLES, item.mesh->indexCount(), GL_UNSIGNED_INT, nullptr);
        ++stats_.shadowDraws;
    };

    // Static casters first with the static program.
    shadowShader_->bind();
    shadowShader_->setUniform("uLightViewProj", flashlightViewProj_);
    for (const RenderItem &item : queue_)
        if (!item.bones)
            drawItem(item);

    // Deformed casters need the SKINNED variant (its bone UBO lookup must
    // match the lit mesh exactly).
    shadowSkinnedShader_->bind();
    shadowSkinnedShader_->setUniform("uLightViewProj", flashlightViewProj_);
    for (const RenderItem &item : queue_)
        if (item.bones)
            drawItem(item);

    Framebuffer::bindDefault();
}

void ForwardRenderer::bindLitUniforms(const Shader &shader, Camera &camera)
{
    shader.setUniform("uViewProj", camera.viewProjection());
    shader.setUniform("uCameraPos", camera.position);
    setupLightingUniforms(shader, camera);
}

void ForwardRenderer::mainPass(Scene &scene, Camera &camera)
{
    // Scene renders into the HDR target; postPass() composites it to the
    // swapchain. Explicit viewport: the shadow pass left it at the shadow-map
    // size, which would otherwise clip the main image.
    sceneTarget_->bind();
    glViewport(0, 0, static_cast<GLint>(sceneTarget_->width()),
        static_cast<GLint>(sceneTarget_->height()));
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
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
        for (const RenderItem &item : queue_) {
            if (!item.material || (item.bones != nullptr) != skinnedPass)
                continue;
            if (skinnedPass)
                uploadBones(*item.bones);
            shader.setUniform("uModel", item.world);
            shader.setUniform("uAlbedo", item.material->albedo);
            shader.setUniform("uEmissive", item.material->emissiveStrength);
            shader.setUniform("uRoughness", item.material->roughness);
            shader.setUniform("uMetallic", item.material->metallic);
            if (item.material->baseColorTex) {
                item.material->baseColorTex->bind(3);
                shader.setUniform("uHasBaseColorTex", 1);
            } else {
                placeholder_->bind(3);
                shader.setUniform("uHasBaseColorTex", 0);
            }
            shader.setUniform("uBaseColorTex", 3);

            // Unit 4: tangent-space normal map (linear data).
            if (item.material->normalTex) {
                item.material->normalTex->bind(4);
                shader.setUniform("uHasNormalTex", 1);
            } else {
                placeholder_->bind(4);
                shader.setUniform("uHasNormalTex", 0);
            }
            shader.setUniform("uNormalTex", 4);

            // Unit 5: glTF metallic-roughness (G=roughness, B=metallic).
            if (item.material->metallicRoughnessTex) {
                item.material->metallicRoughnessTex->bind(5);
                shader.setUniform("uHasMetallicRoughnessTex", 1);
            } else {
                placeholder_->bind(5);
                shader.setUniform("uHasMetallicRoughnessTex", 0);
            }
            shader.setUniform("uMetallicRoughnessTex", 5);

            item.mesh->bind();
            glDrawElements(GL_TRIANGLES, item.mesh->indexCount(), GL_UNSIGNED_INT, nullptr);
            stats_.triangles += item.mesh->indexCount() / 3;
            ++stats_.itemsDrawn;
        }
    };

    litShader_->bind();
    bindLitUniforms(*litShader_, camera);
    drawItems(*litShader_, false);

    litSkinnedShader_->bind();
    bindLitUniforms(*litSkinnedShader_, camera);
    drawItems(*litSkinnedShader_, true);

    if (wireframe_)
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

    // Debug draw lines + gizmos overlay, drawn even in wireframe mode.
    if (debugDrawEnabled_)
        drawDebugData(scene, camera);

    // Unbind texture unit 15 (shadow map) so the next frame's shader binds
    // don't fight with a lingering dependency on the depth attachment.
    glBindTextureUnit(15, 0);
}

void ForwardRenderer::postPass()
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
    glBindTextureUnit(0, sceneTarget_->colorAttachment(0));
    glBindTextureUnit(1, sceneTarget_->depth());
    postShader_->setUniform("uScene", 0);
    postShader_->setUniform("uDepth", 1);
    postShader_->setUniform("uNear", 0.05f);
    postShader_->setUniform("uFar", 200.0f);
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

    // Skeleton overlay (cyan bones, yellow joints). Joints come from the
    // Animator's last evaluated pose, in model space, so the entity's world
    // matrix places them exactly where the skinned vertices ended up.
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

    debugDraw_->flush(camera.viewProjection());
}

} // namespace horror
