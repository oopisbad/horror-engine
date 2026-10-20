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
};

// CPU-skins every vertex of one skinned entity with the *exact* joint matrices
// the renderer uploads to the UBO (Animator::boneMatrices), for `samples`
// evenly spaced times across the current clip. This is the ground truth for
// "does the mesh agree with what we think we are drawing".
//
// Returns an empty vector when the entity is not a skinned mesh with an
// Animator and clips. The animator's playback state is restored afterwards.
std::vector<SkinSample> sampleSkin(Scene &scene, EntityId entity, int samples);

// The skin probe for one model level, formatted for the console.
void printSkinReport(Scene &scene, const std::string &label, EntityId entity, int samples);

} // namespace horror
