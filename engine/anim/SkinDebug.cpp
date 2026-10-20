#include "anim/SkinDebug.hpp"

#include "anim/AnimationSystem.hpp"
#include "anim/Animator.hpp"
#include "core/Log.hpp"
#include "render/Mesh.hpp"
#include "scene/Scene.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace horror {

namespace {

// Linear blend skinning of one vertex with the packed joint matrices, exactly
// as mesh.vert does it (four influences, weights normalised by the loader).
glm::vec3 skinVertex(const Vertex &v, const std::vector<glm::mat4> &bones,
    bool &weighted)
{
    glm::mat4 skin(0.0f);
    float weightSum = 0.0f;
    for (int i = 0; i < 4; ++i) {
        const float w = v.weights[i];
        if (w <= 0.0f)
            continue;
        const uint32_t joint = v.joints[i];
        if (joint >= bones.size())
            continue;
        skin += w * bones[joint];
        weightSum += w;
    }
    weighted = weightSum > 1e-5f;
    if (!weighted)
        return v.position;
    return glm::vec3(skin * glm::vec4(v.position, 1.0f));
}

int dominantJoint(const Vertex &v)
{
    int best = -1;
    float bestWeight = 0.0f;
    for (int i = 0; i < 4; ++i) {
        if (v.weights[i] > bestWeight) {
            bestWeight = v.weights[i];
            best = static_cast<int>(v.joints[i]);
        }
    }
    return best;
}

} // namespace

std::vector<SkinSample> sampleSkin(Scene &scene, EntityId entity, int samples)
{
    std::vector<SkinSample> out;
    const SkinnedMeshRenderer *renderer = scene.find<SkinnedMeshRenderer>(entity);
    Animator *anim = scene.find<Animator>(entity);
    if (!renderer || !renderer->mesh || !anim || !anim->clips || anim->currentClip < 0)
        return out;
    const AnimationClip *clip = anim->clip(anim->currentClip);
    if (!clip)
        return out;
    const std::vector<Vertex> &vertices = renderer->mesh->vertices();
    if (vertices.empty())
        return out;

    const float duration = std::max(clip->duration, 0.0f);
    const int count = std::max(samples, 1);

    // Save playback state: probing must not disturb the running game.
    const float savedTime = anim->currentTime;
    const int savedPrevious = anim->previousClip;
    const float savedFadeElapsed = anim->fadeElapsed;
    const float savedFadeDuration = anim->fadeDuration;
    anim->previousClip = -1;
    anim->fadeDuration = 0.0f;

    const glm::mat4 world = scene.worldMatrix(entity);

    for (int i = 0; i < count; ++i) {
        const float t = count == 1 ? 0.0f : duration * static_cast<float>(i) / static_cast<float>(count - 1);
        anim->currentTime = t;
        AnimationSystem::evaluate(*anim);

        SkinSample sample;
        sample.time = t;
        int lowestIndex = -1, highestIndex = -1;
        float lowest = std::numeric_limits<float>::max();
        float highest = std::numeric_limits<float>::lowest();
        glm::vec3 aabbMin(std::numeric_limits<float>::max());
        glm::vec3 aabbMax(std::numeric_limits<float>::lowest());

        for (size_t v = 0; v < vertices.size(); ++v) {
            bool weighted = false;
            const glm::vec3 model = skinVertex(vertices[v], anim->boneMatrices, weighted);
            const glm::vec3 pos = glm::vec3(world * glm::vec4(model, 1.0f));
            aabbMin = glm::min(aabbMin, pos);
            aabbMax = glm::max(aabbMax, pos);
            if (pos.y < lowest) {
                lowest = pos.y;
                lowestIndex = static_cast<int>(v);
            }
            if (pos.y > highest) {
                highest = pos.y;
                highestIndex = static_cast<int>(v);
            }
        }

        sample.lowestY = lowest;
        sample.highestY = highest;
        sample.aabbMin = aabbMin;
        sample.aabbMax = aabbMax;

        // Joints must live inside the body they deform. 5 cm of slack covers
        // fingertips/nose tips poking past the vertex hull at odd poses.
        const glm::vec3 slack(0.05f);
        for (const glm::vec3 &jp : anim->jointWorldPositions) {
            const glm::vec3 p = glm::vec3(world * glm::vec4(jp, 1.0f));
            if (glm::any(glm::lessThan(p, aabbMin - slack))
                || glm::any(glm::greaterThan(p, aabbMax + slack)))
                ++sample.jointsOutside;
        }

        const auto resolve = [&](int vertexIndex, glm::vec3 &vertexWorld, glm::vec3 &jointWorld,
                                 int &jointIndex) {
            if (vertexIndex < 0)
                return;
            const Vertex &v = vertices[static_cast<size_t>(vertexIndex)];
            bool weighted = false;
            const glm::vec3 model = skinVertex(v, anim->boneMatrices, weighted);
            vertexWorld = glm::vec3(world * glm::vec4(model, 1.0f));

            jointIndex = dominantJoint(v);
            if (jointIndex >= 0 && static_cast<size_t>(jointIndex) < anim->jointWorldPositions.size()) {
                // Exactly what the bone debug overlay draws: the recorded joint
                // position through the entity's world matrix.
                jointWorld = glm::vec3(world
                    * glm::vec4(anim->jointWorldPositions[static_cast<size_t>(jointIndex)], 1.0f));
            } else {
                jointIndex = -1;
                jointWorld = vertexWorld;
            }
        };

        resolve(lowestIndex, sample.footVertex, sample.footJoint, sample.footJointIndex);
        resolve(highestIndex, sample.headVertex, sample.headJoint, sample.headJointIndex);
        sample.footOffset = glm::length(sample.footVertex - sample.footJoint);
        sample.headOffset = glm::length(sample.headVertex - sample.headJoint);
        out.push_back(sample);
    }

    anim->currentTime = savedTime;
    anim->previousClip = savedPrevious;
    anim->fadeElapsed = savedFadeElapsed;
    anim->fadeDuration = savedFadeDuration;
    AnimationSystem::evaluate(*anim);
    return out;
}

void printSkinReport(Scene &scene, const std::string &label, EntityId entity, int samples)
{
    const Animator *anim = scene.find<Animator>(entity);
    if (!anim) {
        HOR_LOG_WARN("[skin] {}: no animator", label);
        return;
    }
    const std::vector<SkinSample> report = sampleSkin(scene, entity, samples);
    if (report.empty()) {
        HOR_LOG_WARN("[skin] {}: nothing to probe", label);
        return;
    }

    HOR_LOG_INFO("[skin] {} clip='{}' duration={:.3f}s samples={}", label,
        anim->currentClipName(), anim->clipDuration(anim->currentClip), report.size());
    float minOffset = std::numeric_limits<float>::max();
    float maxOffset = 0.0f;
    float minY = std::numeric_limits<float>::max();
    float maxY = std::numeric_limits<float>::lowest();
    int worstJointsOutside = 0;
    for (const SkinSample &s : report) {
        worstJointsOutside = std::max(worstJointsOutside, s.jointsOutside);
        minOffset = std::min({ minOffset, s.footOffset, s.headOffset });
        maxOffset = std::max({ maxOffset, s.footOffset, s.headOffset });
        minY = std::min(minY, s.lowestY);
        maxY = std::max(maxY, s.highestY);
        HOR_LOG_INFO(
            "[skin]   t={:.3f} footJoint={} footOff={:.4f} headJoint={} headOff={:.4f} "
            "lowY={:.4f} highY={:.4f} jointsOutside={} footV=({:.3f},{:.3f},{:.3f}) "
            "footJ=({:.3f},{:.3f},{:.3f})",
            s.time, s.footJointIndex, s.footOffset, s.headJointIndex, s.headOffset, s.lowestY,
            s.highestY, s.jointsOutside, s.footVertex.x, s.footVertex.y, s.footVertex.z,
            s.footJoint.x, s.footJoint.y, s.footJoint.z);
    }
    HOR_LOG_INFO(
        "[skin] {} offsets [{:.4f}..{:.4f}] m, lowest vertex [{:.4f}..{:.4f}] m, joints outside "
        "the body: {}",
        label, minOffset, maxOffset, minY, maxY, worstJointsOutside);
}

} // namespace horror
