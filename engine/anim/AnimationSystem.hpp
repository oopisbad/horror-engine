#pragma once

namespace horror {

class Scene;

// Advances every Animator in the scene: time/fade bookkeeping, pose sampling,
// crossfade blending, node-world evaluation, skin matrix build, and animation
// events. Call once per frame with the variable frame delta (not the fixed
// gameplay step) so playback stays smooth.
class AnimationSystem {
public:
    static void update(Scene &scene, float deltaSeconds);
};

} // namespace horror
