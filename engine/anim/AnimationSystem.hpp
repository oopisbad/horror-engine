#pragma once

namespace horror {

class Scene;
struct Animator;

// Advances every Animator in the scene: time/fade bookkeeping, pose sampling,
// crossfade blending, node-world evaluation, skin matrix build, and animation
// events. Call once per frame with the variable frame delta (not the fixed
// gameplay step) so playback stays smooth.
class AnimationSystem {
public:
    static void update(Scene &scene, float dt);

    // Evaluates one Animator at its current time WITHOUT advancing it: pose
    // sampling, node worlds, skin matrices and debug joint positions. Used by
    // the CPU-skinning probe (--debug-skin) and the self-tests, which need to
    // inspect a specific clip time deterministically.
    static void evaluate(Animator &anim);
};

} // namespace horror
