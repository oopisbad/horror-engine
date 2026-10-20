#pragma once

#include "scene/Components.hpp"

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace horror {

class Scene;

// One CPU-skinning probe at one animation time. Everything is in WORLD space,
// i.e. already multiplied by the entity's world matrix -- the same matrix the
// renderer uses as uModel and the bone debug draw uses on joint positions. If
// the two paths disagree, these numbers say by how much.
struct SkinSample {
    float time = 0.0f;             // seconds into the clip
    glm::vec3 footVertex{0.0f};    // lowest skinned vertex
    glm::vec3 footJoint{0.0f};     // that vertex's dominant joint, drawn where
                                   // the bone debug overlay draws it
    int footJointIndex = -1;
    glm::vec3 headVertex{0.0f};    // highest skinned vertex
    glm::vec3 headJoint{0.0f};
    int headJointIndex = -1;
    float footOffset = 0.0f;       // |footVertex - footJoint|
    float headOffset = 0.0f;       // |headVertex - headJoint|
    float lowestY = 0.0f;          // lowest skinned vertex, world Y
    float highestY = 0.0f;

    // World AABB of every skinned vertex, and how many joints fall outside it
    // (expanded by 5 cm). A skeleton drawn in the wrong space scatters joints
    // far outside the body; a correct one has none.
    glm::vec3 aabbMin{0.0f};
    glm::vec3 aabbMax{0.0f};
    int jointsOutside = 0;

    // Largest distance from any joint to the closest skinned vertex. This is
    // the direct answer to "is the skeleton attached to the mesh?": every bone
    // lives inside flesh, so a correct rig has a gap of a few centimetres and
    // a mis-spaced one has a gap of tens of centimetres or metres. Only
    // computed when requested (it is O(joints * vertices)).
    float maxJointGap = 0.0f;
    int worstJoint = -1;

    // World AABB of the joints the skeleton overlay draws (same data, same
    // transform). Project this and the skinned AABB to compare on screen.
    glm::vec3 jointsMin{0.0f};
    glm::vec3 jointsMax{0.0f};
};

// CPU-skins every vertex of one skinned entity with the *exact* joint matrices
// the renderer uploads to the UBO (Animator::boneMatrices), for `samples`
// evenly spaced times across the current clip. This is the ground truth for
// "does the mesh agree with what we think we are drawing".
//
// Returns an empty vector when the entity is not a skinned mesh with an
// Animator and clips. The animator's playback state is restored afterwards.
// `measureJointGap` turns on the (more expensive) joint-to-nearest-vertex
// distance, which is what proves the skeleton is inside the body.
std::vector<SkinSample> sampleSkin(Scene &scene, EntityId entity, int samples,
    bool measureJointGap = false);

// The skin probe for one model level, formatted for the console.
void printSkinReport(Scene &scene, const std::string &label, EntityId entity, int samples);

} // namespace horror
