#include "render/Renderer.hpp"
#include "render/Framebuffer.hpp"
#include "render/Shader.hpp"
#include "scene/Scene.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace horror {
namespace {
bool invertBox(const glm::mat4 &world, glm::mat4 &inverse)
{
    const float determinant = glm::determinant(glm::mat3(world));
    if (!std::isfinite(determinant) || std::abs(determinant) < 1e-8f)
        return false;
    inverse = glm::inverse(world);
    return true;
}

template<class Component, size_t Capacity>
auto nearest(Scene &scene, glm::vec3 eye)
{
    std::array<std::pair<float, EntityId>, Capacity> result;
    result.fill({INFINITY, kInvalidEntity});
    scene.view<Component>([&](EntityId id, const Component &) {
        const auto offset = glm::vec3(scene.worldMatrix(id)[3]) - eye;
        const auto candidate = std::pair{glm::dot(offset, offset), id};
        const auto insert = std::lower_bound(result.begin(), result.end(), candidate);
        if (insert != result.end()) {
            std::move_backward(insert, result.end()-1, result.end());
            *insert = candidate;
        }
    });
    return result;
}
} // namespace

void ForwardRenderer::gatherEnvironment(Scene &scene, const Camera &camera)
{
    localPointCount_ = fogCount_ = decalCount_ = probeCount_ = 0;
    for (const auto &[distance, id] : nearest<PointLight, 8>(scene, camera.position)) {
        if (id == kInvalidEntity)
            break;
        const auto &point = *scene.find<PointLight>(id);
        localPoints_[localPointCount_++] = {id, glm::vec3(scene.worldMatrix(id)[3]),
            point.color * point.intensity, point.range, point.castShadow};
    }

    for (const auto &[distance, id] : nearest<FogVolume, 4>(scene, camera.position)) {
        if (id == kInvalidEntity) break;
        const auto &fog = *scene.find<FogVolume>(id);
        glm::mat4 inverse;
        if (fog.density > 0 && invertBox(scene.worldMatrix(id), inverse))
            fogVolumes_[fogCount_++] = {inverse, fog.color, fog.density};
    }
    for (const auto &[distance, id] : nearest<SurfaceDecal, 12>(scene, camera.position)) {
        if (id == kInvalidEntity) break;
        const auto &decal = *scene.find<SurfaceDecal>(id);
        glm::mat4 inverse;
        if (decal.strength > 0 && invertBox(scene.worldMatrix(id), inverse))
            decals_[decalCount_++] = {inverse, decal};
    }

    size_t write = 0;
    for (const auto &[distance, id] : nearest<ReflectionProbe, 2>(scene, camera.position)) {
        if (id == kInvalidEntity) break;
        const auto &authored = *scene.find<ReflectionProbe>(id);
        const auto &world = scene.worldMatrix(id);
        glm::mat4 inverse;
        if (authored.strength <= 0 || !invertBox(world, inverse))
            continue;
        auto found = std::find_if(probes_.begin() + write, probes_.end(),
            [id](const Probe &probe) { return probe.id == id; });
        if (found != probes_.end())
            std::swap(probes_[write], *found);
        auto &slot = probes_[write++];
        const auto origin = glm::vec3(world[3]);
        if (slot.id != id || slot.origin != origin)
            slot.ready = false;
        slot.id = id;
        slot.origin = origin;
        slot.worldToBox = inverse;
        slot.strength = authored.strength;
    }
    probeCount_ = write;
    stats_.activeProbes = static_cast<uint32_t>(probeCount_);
    stats_.activeFogVolumes = static_cast<uint32_t>(fogCount_);
    stats_.activeDecals = static_cast<uint32_t>(decalCount_);
}

void ForwardRenderer::bindEnvironment(const Shader &shader)
{
    shader.setUniform("uProbeCount", capturingProbe_ ? 0 : static_cast<int>(probeCount_));
    for (size_t i = 0; i < probes_.size(); ++i) {
        const auto &probe = probes_[i];
        const auto prefix = "uProbes[" + std::to_string(i) + "]";
        const bool enabled = !capturingProbe_ && i < probeCount_ && probe.ready;
        shader.setUniform((prefix + ".worldToBox").c_str(), probe.worldToBox);
        shader.setUniform((prefix + ".originStrength").c_str(), glm::vec4(probe.origin, enabled ? probe.strength : 0.0f));
        glBindTextureUnit(17 + static_cast<GLuint>(i), enabled ? probe.cube : 0);
    }

    shader.setUniform("uDecalCount", static_cast<int>(decalCount_));
    for (size_t i = 0; i < decalCount_; ++i) {
        const auto &decal = decals_[i];
        const auto prefix = "uDecals[" + std::to_string(i) + "]";
        shader.setUniform((prefix + ".worldToBox").c_str(), decal.worldToBox);
        shader.setUniform((prefix + ".tintStrength").c_str(), glm::vec4(decal.decal.tint, decal.decal.strength));
        shader.setUniform((prefix + ".patternWetness").c_str(),
            glm::vec2(static_cast<float>(decal.decal.pattern), decal.decal.wetness));
    }
}

void ForwardRenderer::captureProbes(Scene &scene, const Camera &camera)
{
    if (probeCount_ == 0 || wireframe_)
        return;
    bool pending = false;
    for (size_t i = 0; i < probeCount_; ++i) pending |= !probes_[i].ready;
    if (!pending)
        return;

    constexpr GLsizei size = 128;
    constexpr GLsizei levels = 8;
    constexpr glm::vec3 directions[] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    constexpr glm::vec3 ups[] = {{0,-1,0},{0,-1,0},{0,0,1},{0,0,-1},{0,-1,0},{0,-1,0}};
    const auto projection = glm::perspective(glm::half_pi<float>(), 1.0f, 0.05f, 80.0f);
    if (!probeTarget_)
        probeTarget_ = std::make_unique<Framebuffer>(Framebuffer::Kind::ColorDepth, size, size, true);
    const auto saved = stats_;
    const auto mainView = drawViewProj_;
    // Capture static lighting without the player flashlight or animated props.
    capturingProbe_ = true;
    for (size_t i = 0; i < probeCount_; ++i) {
        auto &probe = probes_[i];
        if (probe.ready)
            continue;
        if (!probe.cube) {
            glCreateTextures(GL_TEXTURE_CUBE_MAP, 1, &probe.cube);
            glTextureStorage2D(probe.cube, levels, GL_R11F_G11F_B10F, size, size);
            glTextureParameteri(probe.cube, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glTextureParameteri(probe.cube, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTextureParameteri(probe.cube, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTextureParameteri(probe.cube, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTextureParameteri(probe.cube, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        }
        Camera view;
        view.position = probe.origin;
        for (GLint face = 0; face < 6; ++face) {
            drawViewProj_ = projection * glm::lookAt(probe.origin, probe.origin + directions[face], ups[face]);
            // Probe visibility is independent of the player camera.
            for (auto &item : queue_) item.cameraVisible = true;
            mainPass(scene, view);
            glCopyImageSubData(probeTarget_->colorAttachment(), GL_TEXTURE_2D, 0, 0, 0, 0,
                probe.cube, GL_TEXTURE_CUBE_MAP, 0, 0, 0, face, size, size, 1);
        }
        glGenerateTextureMipmap(probe.cube);
        probe.ready = true;
    }
    capturingProbe_ = false;
    drawViewProj_ = mainView;
    buildQueue(scene, camera);
    stats_ = saved;
}
} // namespace horror
