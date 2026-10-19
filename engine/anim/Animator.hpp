#pragma once

#include "anim/Skeleton.hpp"

#include <glm/glm.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace horror {

// Playback state for one skinned mesh. Lives next to its SkinnedMeshRenderer
// component; AnimationSystem::update() advances it every frame and fills
// boneMatrices, which the renderer uploads to the bone UBO.
//
// Crossfades blend node-local TRS poses, not skin matrices: blending baked
// matrices visibly collapses joints when two poses differ a lot.
struct Animator {
    // Shared, immutable clip/skeleton data (owned by the loaded glTF model).
    std::shared_ptr<const Skeleton> skeleton;
    std::shared_ptr<const std::vector<std::shared_ptr<const AnimationClip>>> clips;

    // --- authored knobs -----------------------------------------------------
    float speed = 1.0f;
    bool paused = false;

    // --- runtime ------------------------------------------------------------
    int currentClip = -1;
    int previousClip = -1;
    float currentTime = 0.0f;
    float previousTime = 0.0f;
    float fadeDuration = 0.0f;
    float fadeElapsed = 0.0f;
    bool currentLooping = true;
    bool finished = false;

    // Packed skinning matrices (jointCount entries) and joint world positions
    // for debug drawing.
    std::vector<glm::mat4> boneMatrices;
    std::vector<glm::vec3> jointWorldPositions;

    // Fired for every AnimationEvent crossed this frame; may be empty.
    std::function<void(const AnimationEvent &)> onEvent;

    int findClip(const std::string &name) const;
    const AnimationClip *clip(int index) const;

    // Starts a clip with an optional crossfade from whatever is playing.
    // Looping clips wrap; one-shot clips stop on the last frame and set
    // `finished`. Returns false when the name is unknown.
    bool play(const std::string &name, float fadeTime = 0.2f, bool loop = true);
    void playIndex(int index, float fadeTime = 0.2f, bool loop = true);
    void stop();

    float clipDuration(int index) const;
    const std::string &currentClipName() const;
};

} // namespace horror
