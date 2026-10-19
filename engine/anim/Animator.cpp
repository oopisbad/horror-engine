#include "anim/Animator.hpp"

#include "core/Log.hpp"

#include <algorithm>

namespace horror {

int Animator::findClip(const std::string &name) const
{
    if (!clips)
        return -1;
    for (size_t i = 0; i < clips->size(); ++i) {
        if ((*clips)[i] && (*clips)[i]->name == name)
            return static_cast<int>(i);
    }
    return -1;
}

const AnimationClip *Animator::clip(int index) const
{
    if (!clips || index < 0 || static_cast<size_t>(index) >= clips->size())
        return nullptr;
    return (*clips)[static_cast<size_t>(index)].get();
}

float Animator::clipDuration(int index) const
{
    const AnimationClip *c = clip(index);
    return c ? c->duration : 0.0f;
}

const std::string &Animator::currentClipName() const
{
    static const std::string none = "none";
    const AnimationClip *c = clip(currentClip);
    return c ? c->name : none;
}

void Animator::playIndex(int index, float fadeTime, bool loop)
{
    if (!clip(index)) {
        HOR_LOG_WARN("animator: clip index {} out of range", index);
        return;
    }
    if (index == currentClip && !finished) {
        // Re-triggering the same clip restarts it (attack chains rely on this).
        currentTime = 0.0f;
        fadeElapsed = fadeDuration = 0.0f;
        previousClip = -1;
        currentLooping = loop;
        return;
    }

    // Whatever is currently visible becomes the outgoing half of the blend.
    if (fadeTime > 0.0f && currentClip >= 0) {
        previousClip = currentClip;
        previousTime = currentTime;
    } else {
        previousClip = -1;
    }
    currentClip = index;
    currentTime = 0.0f;
    fadeDuration = std::max(fadeTime, 0.0f);
    fadeElapsed = 0.0f;
    currentLooping = loop;
    finished = false;
}

bool Animator::play(const std::string &name, float fadeTime, bool loop)
{
    const int index = findClip(name);
    if (index < 0) {
        HOR_LOG_WARN("animator: clip '{}' not found ({} clips)", name,
            clips ? clips->size() : 0);
        return false;
    }
    playIndex(index, fadeTime, loop);
    return true;
}

void Animator::stop()
{
    currentClip = -1;
    previousClip = -1;
    currentTime = previousTime = 0.0f;
    fadeElapsed = fadeDuration = 0.0f;
    finished = true;
}

} // namespace horror
