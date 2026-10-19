#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <memory>
#include <string>
#include <vector>

namespace horror {

// Rest-pose node hierarchy + skinning binding for one glTF skin.
//
// The skeleton carries a private *copy* of the model's node hierarchy so it
// can be evaluated (and shared with an Animator) without keeping the whole
// asset alive or walking back into loader-owned memory. For typical rigs this
// is well under a hundred nodes; clips reference these node indices.
struct Skeleton {
    struct Node {
        int parent = -1;
        glm::vec3 translation{0.0f};
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        glm::vec3 scale{1.0f};
    };

    std::vector<Node> nodes;              // whole model hierarchy
    std::vector<int> joints;              // node index per joint
    std::vector<int> jointParents;        // joint index into `joints`, -1 = root
    std::vector<glm::mat4> inverseBind;   // one per joint
    std::string name;

    int jointCount() const { return static_cast<int>(joints.size()); }
};

// How a sampler's keyframes interpolate (glTF 2.0 sampler.interpolation).
enum class AnimationInterpolation { Linear, Step, CubicSpline };

// One animated node property. Values are vec4 for all paths: translation and
// scale use xyz (w unused), rotation stores a quaternion in xyzw. Cubic-spline
// channels store the glTF triplet layout (in-tangent, value, out-tangent), so
// `values.size() == times.size() * 3` for those.
struct AnimationChannel {
    enum class Path { Translation, Rotation, Scale };

    int node = -1;
    Path path = Path::Translation;
    AnimationInterpolation interpolation = AnimationInterpolation::Linear;
    std::vector<float> times;
    std::vector<glm::vec4> values;
};

// Time-stamped gameplay event (footstep, hit, ...). Fired by the AnimationSystem
// when playback crosses the time, loop wraps included.
struct AnimationEvent {
    float time = 0.0f;
    std::string name;
};

struct AnimationClip {
    std::string name;
    float duration = 0.0f; // seconds (max keyframe time)
    std::vector<AnimationChannel> channels;
    std::vector<AnimationEvent> events; // sorted by time
};

} // namespace horror
