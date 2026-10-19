#include "anim/AnimationSystem.hpp"

#include "anim/Animator.hpp"
#include "core/Log.hpp"
#include "scene/Scene.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

namespace horror {

namespace {

// Node-local TRS pose for every node in the skeleton.
struct Pose {
    std::vector<glm::vec3> translation;
    std::vector<glm::quat> rotation;
    std::vector<glm::vec3> scale;

    void reset(const Skeleton &sk)
    {
        translation.resize(sk.nodes.size());
        rotation.resize(sk.nodes.size());
        scale.resize(sk.nodes.size());
        for (size_t i = 0; i < sk.nodes.size(); ++i) {
            translation[i] = sk.nodes[i].translation;
            rotation[i] = sk.nodes[i].rotation;
            scale[i] = sk.nodes[i].scale;
        }
    }
};

glm::vec4 keyValue(const AnimationChannel &c, size_t i)
{
    if (c.interpolation == AnimationInterpolation::CubicSpline)
        return c.values[i * 3 + 1]; // in-tangent, value, out-tangent
    return c.values[i];
}

// Component-wise cubic Hermite. glTF stores tangents in value units/second,
// so they are scaled by the segment duration. For rotations this is applied
// to the quaternion components and renormalized -- an approximation of
// spherical spline interpolation that is visually indistinguishable for the
// small angular steps real clips use.
glm::vec4 hermite(const glm::vec4 &p0, const glm::vec4 &m0,
    const glm::vec4 &p1, const glm::vec4 &m1, float u, float dt)
{
    const float u2 = u * u;
    const float u3 = u2 * u;
    const float h00 = 2.0f * u3 - 3.0f * u2 + 1.0f;
    const float h10 = u3 - 2.0f * u2 + u;
    const float h01 = -2.0f * u3 + 3.0f * u2;
    const float h11 = u3 - u2;
    return h00 * p0 + h10 * dt * m0 + h01 * p1 + h11 * dt * m1;
}

glm::vec4 sampleChannel(const AnimationChannel &c, float t)
{
    if (c.times.empty() || c.values.empty())
        return glm::vec4(0.0f);
    const size_t n = c.times.size();
    if (t <= c.times.front())
        return keyValue(c, 0);
    if (t >= c.times.back())
        return keyValue(c, n - 1);

    // Small key counts per channel; linear scan beats a branchy binary search.
    size_t i = 0;
    while (i + 1 < n && t >= c.times[i + 1])
        ++i;
    const size_t j = std::min(i + 1, n - 1);

    const float dt = c.times[j] - c.times[i];
    const float u = dt > 1e-8f ? (t - c.times[i]) / dt : 0.0f;

    if (c.interpolation == AnimationInterpolation::Step)
        return keyValue(c, i);

    if (c.interpolation == AnimationInterpolation::CubicSpline) {
        if (i * 3 + 2 >= c.values.size() || j * 3 >= c.values.size())
            return keyValue(c, i);
        const glm::vec4 p0 = c.values[i * 3 + 1];
        const glm::vec4 m0 = c.values[i * 3 + 2];
        const glm::vec4 p1 = c.values[j * 3 + 1];
        const glm::vec4 m1 = c.values[j * 3];
        glm::vec4 v = hermite(p0, m0, p1, m1, u, dt);
        if (c.path == AnimationChannel::Path::Rotation) {
            glm::quat q(v.w, v.x, v.y, v.z);
            const float len = glm::length(q);
            if (len > 1e-8f)
                q /= len;
            return glm::vec4(q.x, q.y, q.z, q.w);
        }
        return v;
    }

    // Linear.
    glm::vec4 a = keyValue(c, i);
    glm::vec4 b = keyValue(c, j);
    if (c.path == AnimationChannel::Path::Rotation) {
        glm::quat qa(a.w, a.x, a.y, a.z);
        glm::quat qb(b.w, b.x, b.y, b.z);
        glm::quat q = glm::slerp(qa, qb, u);
        return glm::vec4(q.x, q.y, q.z, q.w);
    }
    return glm::mix(a, b, u);
}

void sampleClipInto(const AnimationClip &clip, float time, const Skeleton &sk, Pose &pose)
{
    pose.reset(sk);
    for (const AnimationChannel &c : clip.channels) {
        if (c.node < 0 || static_cast<size_t>(c.node) >= pose.translation.size())
            continue;
        const glm::vec4 v = sampleChannel(c, time);
        switch (c.path) {
        case AnimationChannel::Path::Translation:
            pose.translation[c.node] = glm::vec3(v);
            break;
        case AnimationChannel::Path::Rotation:
            pose.rotation[c.node] = glm::quat(v.w, v.x, v.y, v.z);
            break;
        case AnimationChannel::Path::Scale:
            pose.scale[c.node] = glm::vec3(v);
            break;
        }
    }
}

void blendPose(const Pose &a, const Pose &b, float w, Pose &out)
{
    out.translation.resize(a.translation.size());
    out.rotation.resize(a.rotation.size());
    out.scale.resize(a.scale.size());
    for (size_t i = 0; i < a.translation.size(); ++i) {
        out.translation[i] = glm::mix(a.translation[i], b.translation[i], w);
        out.rotation[i] = glm::slerp(a.rotation[i], b.rotation[i], w);
        out.scale[i] = glm::mix(a.scale[i], b.scale[i], w);
    }
}

// Node world matrices, resolved recursively so arbitrary node ordering (and
// parent-before-child assumptions) never leak into the loader.
void computeNodeWorlds(const Skeleton &sk, const Pose &pose, std::vector<glm::mat4> &worlds)
{
    worlds.assign(sk.nodes.size(), glm::mat4(1.0f));
    std::vector<uint8_t> state(sk.nodes.size(), 0); // 0 = todo, 1 = in progress, 2 = done

    std::function<glm::mat4(int)> eval = [&](int index) -> glm::mat4 {
        if (index < 0 || static_cast<size_t>(index) >= sk.nodes.size())
            return glm::mat4(1.0f);
        if (state[static_cast<size_t>(index)] == 2)
            return worlds[static_cast<size_t>(index)];
        if (state[static_cast<size_t>(index)] == 1) {
            HOR_LOG_WARN_ONCE("skeleton '{}' has a node cycle", sk.name);
            return glm::mat4(1.0f);
        }
        state[static_cast<size_t>(index)] = 1;

        const size_t i = static_cast<size_t>(index);
        glm::mat4 local = glm::translate(glm::mat4(1.0f), pose.translation[i])
            * glm::mat4_cast(pose.rotation[i])
            * glm::scale(glm::mat4(1.0f), pose.scale[i]);
        const int parent = sk.nodes[i].parent;
        if (parent >= 0)
            local = eval(parent) * local;
        worlds[i] = local;
        state[i] = 2;
        return local;
    };

    for (size_t i = 0; i < sk.nodes.size(); ++i)
        eval(static_cast<int>(i));
}

void fireEvents(const AnimationClip &clip, float from, float to, bool looping,
    const std::function<void(const AnimationEvent &)> &callback)
{
    if (!callback || clip.events.empty())
        return;
    auto inRange = [&](float e) { return e > from && e <= to; };
    if (looping && to < from) {
        // Wrapped: (from, duration] then [0, to].
        for (const AnimationEvent &e : clip.events)
            if (e.time > from || e.time <= to)
                callback(e);
        return;
    }
    for (const AnimationEvent &e : clip.events)
        if (inRange(e.time))
            callback(e);
}

} // namespace

void AnimationSystem::update(Scene &scene, float deltaSeconds)
{
    const float dt = std::max(deltaSeconds, 0.0f);
    if (dt <= 0.0f)
        return;

    scene.view<Animator>([&](EntityId, Animator &anim) {
        if (!anim.skeleton || !anim.clips || anim.currentClip < 0 || anim.paused)
            return;

        const Skeleton &sk = *anim.skeleton;
        // Resize once per skeleton, not once per frame of a live alloc.
        if (anim.boneMatrices.size() != static_cast<size_t>(sk.jointCount()))
            anim.boneMatrices.assign(static_cast<size_t>(sk.jointCount()), glm::mat4(1.0f));
        if (anim.jointWorldPositions.size() != static_cast<size_t>(sk.jointCount()))
            anim.jointWorldPositions.assign(static_cast<size_t>(sk.jointCount()), glm::vec3(0.0f));

        const AnimationClip *current = anim.clip(anim.currentClip);
        if (!current)
            return;

        // ---- advance time --------------------------------------------------
        const float oldTime = anim.currentTime;
        float newTime = anim.currentTime + dt * anim.speed;
        if (anim.currentLooping) {
            if (current->duration > 1e-6f) {
                newTime = std::fmod(newTime, current->duration);
                if (newTime < 0.0f)
                    newTime += current->duration;
            } else {
                newTime = 0.0f;
            }
        } else if (newTime >= current->duration) {
            newTime = current->duration;
            anim.finished = true;
        }
        anim.currentTime = newTime;

        if (!anim.currentLooping || newTime >= oldTime)
            fireEvents(*current, oldTime, newTime, anim.currentLooping, anim.onEvent);

        // ---- sample current pose (and outgoing pose during a crossfade) ----
        Pose pose;
        sampleClipInto(*current, anim.currentTime, sk, pose);

        if (anim.previousClip >= 0 && anim.fadeDuration > 0.0f) {
            anim.fadeElapsed += dt;
            if (anim.fadeElapsed >= anim.fadeDuration) {
                anim.previousClip = -1;
                anim.fadeElapsed = anim.fadeDuration = 0.0f;
            } else {
                const AnimationClip *previous = anim.clip(anim.previousClip);
                if (previous) {
                    anim.previousTime += dt * anim.speed;
                    if (previous->duration > 1e-6f)
                        anim.previousTime = std::fmod(anim.previousTime, previous->duration);
                    Pose prevPose;
                    sampleClipInto(*previous, anim.previousTime, sk, prevPose);
                    // Smoothstep keeps the blend from changing speed abruptly
                    // at the start/end of the fade.
                    float w = anim.fadeElapsed / anim.fadeDuration;
                    w = w * w * (3.0f - 2.0f * w);
                    Pose blended;
                    blendPose(prevPose, pose, w, blended);
                    pose = std::move(blended);
                }
            }
        }

        // ---- skinning matrices --------------------------------------------
        std::vector<glm::mat4> worlds;
        computeNodeWorlds(sk, pose, worlds);
        for (int j = 0; j < sk.jointCount(); ++j) {
            const int node = sk.joints[static_cast<size_t>(j)];
            if (node < 0 || static_cast<size_t>(node) >= worlds.size())
                continue;
            const glm::mat4 &nodeWorld = worlds[static_cast<size_t>(node)];
            // Mesh-node space first, then joint world, then bind inverse.
            anim.boneMatrices[static_cast<size_t>(j)] = sk.meshNodeInverse * nodeWorld
                * sk.inverseBind[static_cast<size_t>(j)];
            // Model-space joint position (the debug drawer applies the entity
            // world matrix, matching the skinned vertex path).
            anim.jointWorldPositions[static_cast<size_t>(j)] = glm::vec3(nodeWorld[3]);
        }
    });
}

} // namespace horror
