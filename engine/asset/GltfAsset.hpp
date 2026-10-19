#pragma once

#include "anim/Skeleton.hpp"
#include "render/Material.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace horror {

class Mesh;
class Texture;

// One drawable primitive of a glTF mesh (usually one material).
struct GltfMeshPart {
    std::shared_ptr<Mesh> mesh;
    int material = -1; // index into GltfModel::materials(), -1 = default
};

struct GltfMesh {
    std::string name;
    std::vector<GltfMeshPart> parts;
    glm::vec3 boundsMin{0.0f};
    glm::vec3 boundsMax{0.0f};
};

// A scene node with its local transform. glTF allows a full matrix instead of
// TRS; that case is decomposed at load time (shear is not supported).
struct GltfNode {
    std::string name;
    glm::vec3 translation{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};

    int parent = -1;
    std::vector<int> children;
    int mesh = -1;
    int skin = -1;
    int light = -1;
    std::unordered_map<std::string, std::string> extras;

    glm::mat4 localMatrix() const;
};

// KHR_lights_punctual. Spot cone angles are converted to degrees (half-angle),
// matching SpotLight's authoring convention.
struct GltfLight {
    enum class Type { Directional, Point, Spot } type = Type::Point;
    std::string name;
    glm::vec3 color{1.0f};
    float intensity = 1.0f;
    float range = 0.0f;
    float innerConeDeg = 30.0f;
    float outerConeDeg = 45.0f;
};

// A fully loaded .glb/.gltf: GPU meshes, materials, textures, the node
// hierarchy, skins, animation clips and punctual lights.
//
// Conventions: glTF 2.0 is Y-up, right-handed, meters. The engine uses the
// same (camera looks down -Z, +Y up), so no axis conversion is applied.
class GltfModel {
public:
    // `path` may be relative; it is resolved against the asset root with
    // core/AssetPaths. Loading failures set error() instead of throwing so a
    // missing test asset cannot take the whole engine down.
    explicit GltfModel(std::string path);
    ~GltfModel();

    GltfModel(const GltfModel &) = delete;
    GltfModel &operator=(const GltfModel &) = delete;

    bool valid() const { return valid_; }
    const std::string &error() const { return error_; }
    const std::string &path() const { return path_; }

    const std::vector<GltfNode> &nodes() const { return nodes_; }
    const std::vector<GltfMesh> &meshes() const { return meshes_; }
    const std::vector<std::shared_ptr<Material>> &materials() const { return materials_; }
    const std::vector<std::shared_ptr<Texture>> &textures() const { return textures_; }
    const std::vector<std::shared_ptr<const Skeleton>> &skeletons() const { return skeletons_; }
    const std::vector<GltfLight> &lights() const { return lights_; }
    const std::vector<int> &roots() const { return roots_; }

    // Shared clip list; every Animator on this model points at it.
    // Returned by value: the const-qualified shared_ptr is a different type
    // than the stored one, so a reference would bind to a temporary.
    std::shared_ptr<const std::vector<std::shared_ptr<const AnimationClip>>> clips() const
    {
        return clips_;
    }

    // Rest-pose world matrix of one node (recursive, cached per call).
    glm::mat4 restNodeWorldMatrix(int node) const;

    // Model-space bounds over all meshes.
    glm::vec3 boundsMin() const { return boundsMin_; }
    glm::vec3 boundsMax() const { return boundsMax_; }

    // Convenience lookups for the level builder.
    const AnimationClip *findClip(const std::string &name) const;
    int findNode(const std::string &name) const;

private:
    void load();
    bool valid_ = false;
    std::string path_;
    std::string resolvedPath_;
    std::string error_;

    std::vector<GltfNode> nodes_;
    std::vector<int> roots_;
    std::vector<GltfMesh> meshes_;
    std::vector<std::shared_ptr<Material>> materials_;
    std::vector<std::shared_ptr<Texture>> textures_;
    std::vector<std::shared_ptr<const Skeleton>> skeletons_;
    std::vector<GltfLight> lights_;
    std::shared_ptr<std::vector<std::shared_ptr<const AnimationClip>>> clips_;
    glm::vec3 boundsMin_{0.0f};
    glm::vec3 boundsMax_{0.0f};
};

// Cached asset loading. Weak cache: models stay alive while a caller holds a
// shared_ptr (level entities reference their meshes/materials, so keep the
// level's model handles for the lifetime of the level).
class AssetLibrary {
public:
    std::shared_ptr<GltfModel> loadModel(const std::string &relativePath);
    void clear();
    size_t liveCount() const;

private:
    std::unordered_map<std::string, std::weak_ptr<GltfModel>> models_;
};

} // namespace horror
