#pragma once

#include "scene/Components.hpp"
#include "scene/Collision.hpp"

#include <glm/glm.hpp>

#include <memory>
#include <string>
#include <vector>

namespace horror {

class Scene;
class GltfModel;

// Options for turning a glTF scene into entities. Defaults follow the level
// conventions in docs/BLENDER_EXPORT.md.
struct LevelOptions {
    bool importMeshes = true;
    bool importLights = true;
    bool importSpawnPoints = true;
    bool importTriggers = true;
    bool importCollision = true;   // enable COL_ and COL_MESH_ markers
    bool bakePhysics = false;     // static triangles and pushable PROP_ boxes
    bool importExtras = true;      // node extras -> EntityExtras
};

struct LevelInstance {
    // Keeps the asset (mesh/material/texture memory referenced by components)
    // alive for as long as the level lives. Hold on to this.
    std::shared_ptr<GltfModel> model;

    std::unique_ptr<LevelPhysics> physics;
    EntityId root = kInvalidEntity;
    std::vector<EntityId> entities;     // every entity created, hierarchy included
    std::vector<EntityId> meshEntities;
    std::vector<EntityId> spawnPoints;
    std::vector<EntityId> triggers;
    std::vector<EntityId> lights;
    size_t lightmappedMeshes = 0;
};

// Instantiates a loaded glTF scene into `scene` under a new root entity.
//
// Node name conventions (case-sensitive prefixes):
//   COL_*      invisible box collider from local mesh bounds
//   COL_MESH_* invisible triangle collider when bakePhysics is enabled
//   PROP_*     visible pushable box when bakePhysics is enabled
//   SPAWN_*    spawn marker (SpawnPoint with the node's world facing)
//   TRIGGER_*  trigger volume (world AABB in TriggerVolume)
// Nodes with a mesh become MeshRenderer children (or SkinnedMeshRenderer +
// Animator when the node has a skin). KHR_lights_punctual lights become
// PointLight/SpotLight components on the node entity. glTF `extras` become an
// EntityExtras map.
class LevelBuilder {
public:
    static LevelInstance instantiate(Scene &scene, std::shared_ptr<GltfModel> model,
        const Transform &rootTransform = Transform{}, const LevelOptions &options = {},
        const std::string &namePrefix = "");
};

} // namespace horror
