#include "asset/LevelBuilder.hpp"

#include "asset/GltfAsset.hpp"
#include "core/Log.hpp"
#include "render/Mesh.hpp"
#include "scene/Scene.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <functional>
#include <limits>

namespace horror {

namespace {

bool startsWith(const std::string &s, const char *prefix)
{
    const size_t n = std::char_traits<char>::length(prefix);
    return s.size() >= n && s.compare(0, n, prefix) == 0;
}

// (min, max) local bounds of every static primitive under one glTF mesh.
bool meshLocalBounds(const GltfMesh &mesh, glm::vec3 &outMin, glm::vec3 &outMax)
{
    outMin = glm::vec3(std::numeric_limits<float>::max());
    outMax = glm::vec3(-std::numeric_limits<float>::max());
    bool any = false;
    for (const GltfMeshPart &part : mesh.parts) {
        if (!part.mesh)
            continue;
        outMin = glm::min(outMin, part.mesh->minBounds());
        outMax = glm::max(outMax, part.mesh->maxBounds());
        any = true;
    }
    return any;
}

} // namespace

LevelInstance LevelBuilder::instantiate(Scene &scene, std::shared_ptr<GltfModel> model,
    const Transform &rootTransform, const LevelOptions &options, const std::string &namePrefix)
{
    LevelInstance result;
    result.model = std::move(model);
    if (!result.model || !result.model->valid()) {
        HOR_LOG_ERROR("level: refusing to instantiate an invalid glTF model");
        return result;
    }

    GltfModel &m = *result.model;
    const std::string baseName = m.path().substr(m.path().find_last_of('/') + 1);

    result.root = scene.create(namePrefix + baseName);
    // Copy only the TRS: Transform also owns the parent/children links that
    // create() just set up. Assigning a whole Transform{} replaced them with
    // "no parent", which silently detached every node from the level root
    // (the root's placement, yaw and scale then did not reach the meshes).
    auto setLocal = [](Transform &target, const Transform &local) {
        target.position = local.position;
        target.rotation = local.rotation;
        target.scale = local.scale;
    };
    setLocal(scene.transform(result.root), rootTransform);
    result.entities.push_back(result.root);

    auto addEntity = [&](const std::string &name, EntityId parent, const Transform &transform) {
        EntityId e = scene.create(name, parent);
        setLocal(scene.transform(e), transform);
        result.entities.push_back(e);
        return e;
    };

    // Recursive node walk; `parentEntity` is the entity that owns the node's
    // transform (the level root for scene roots).
    std::function<void(int, EntityId)> buildNode = [&](int nodeIndex, EntityId parentEntity) {
        const GltfNode &node = m.nodes()[static_cast<size_t>(nodeIndex)];
        Transform local;
        local.position = node.translation;
        local.rotation = node.rotation;
        local.scale = node.scale;

        const bool isCollision = startsWith(node.name, "COL_");
        const bool isSpawn = startsWith(node.name, "SPAWN_");
        const bool isTrigger = startsWith(node.name, "TRIGGER_");

        if ((isCollision && !options.importCollision) || (isSpawn && !options.importSpawnPoints)
            || (isTrigger && !options.importTriggers)) {
            // Keep the node's transform in the hierarchy even when its
            // component is not imported: children's local TRS is relative to
            // this node, so re-parenting them straight to the parent placed
            // the whole subtree as if the marker sat at the parent's origin.
            // A component-less entity costs one Transform.
            EntityId holder = addEntity(namePrefix + node.name, parentEntity, local);
            for (int child : node.children)
                buildNode(child, holder);
            return;
        }

        EntityId entity = addEntity(namePrefix + node.name, parentEntity, local);

        if (options.importExtras && !node.extras.empty()) {
            EntityExtras extras;
            extras.values = node.extras;
            scene.add(entity, std::move(extras));
        }

        if (node.light >= 0 && options.importLights) {
            const GltfLight &light = m.lights()[static_cast<size_t>(node.light)];
            switch (light.type) {
            case GltfLight::Type::Point: {
                PointLight pl;
                pl.color = light.color;
                pl.intensity = light.intensity;
                pl.range = light.range > 0.0f ? light.range : 10.0f;
                scene.add(entity, pl);
                result.lights.push_back(entity);
                break;
            }
            case GltfLight::Type::Spot: {
                SpotLight sl;
                sl.color = light.color;
                sl.intensity = light.intensity;
                sl.range = light.range > 0.0f ? light.range : 15.0f;
                sl.innerConeDeg = light.innerConeDeg;
                sl.outerConeDeg = light.outerConeDeg;
                scene.add(entity, sl);
                result.lights.push_back(entity);
                break;
            }
            case GltfLight::Type::Directional:
                HOR_LOG_WARN_ONCE("glTF directional light '{}' ignored (no directional component)",
                    light.name);
                break;
            }
        }

        // Mesh bounds drive collision/trigger volumes even for invisible
        // markers; mark that the node has a mesh before skipping rendering.
        const GltfMesh *mesh = node.mesh >= 0 ? &m.meshes()[static_cast<size_t>(node.mesh)] : nullptr;
        glm::vec3 localMin(0.0f), localMax(0.0f);
        const bool haveBounds = mesh && meshLocalBounds(*mesh, localMin, localMax);

        if (isCollision) {
            if (haveBounds) {
                AABBCollider collider;
                collider.center = (localMin + localMax) * 0.5f;
                collider.halfExtents = (localMax - localMin) * 0.5f;
                collider.resolveCollisions = true;
                scene.add(entity, collider);
            } else {
                HOR_LOG_WARN("COL_ node '{}' has no mesh; no collider created", node.name);
            }
        } else if (isSpawn) {
            // Facing is a world direction; it needs the evaluated hierarchy
            // (level root yaw included), so it is filled in after all nodes
            // exist -- see the loop at the end of instantiate().
            SpawnPoint spawn;
            spawn.tag = node.name.substr(6); // strip "SPAWN_"
            scene.add(entity, spawn);
            result.spawnPoints.push_back(entity);
        } else if (isTrigger) {
            if (!haveBounds) {
                // A trigger without geometry used to be added anyway, with its
                // world bounds defaulting to (0,0,0): a zero-size volume at the
                // map origin that could fire on anything near it. Match COL_:
                // warn and create nothing, so the author sees why.
                HOR_LOG_WARN("TRIGGER_ node '{}' has no mesh; no trigger created", node.name);
            } else {
                TriggerVolume trigger;
                trigger.tag = node.name.substr(8); // strip "TRIGGER_"
                // Store the node-local box only. Scene::refreshTriggerBounds()
                // transforms it by the entity's world matrix, so the level
                // root's position/yaw/scale, the floor snap and later root
                // motion all apply (baking model-space numbers into a
                // world-space field put scaled/yawed levels' triggers at the
                // wrong place and size).
                trigger.localCenter = (localMin + localMax) * 0.5f;
                trigger.localHalfExtents = (localMax - localMin) * 0.5f;
                scene.add(entity, trigger);
                result.triggers.push_back(entity);
            }
        } else if (options.importMeshes && mesh) {
            for (size_t partIndex = 0; partIndex < mesh->parts.size(); ++partIndex) {
                const GltfMeshPart &part = mesh->parts[partIndex];
                if (!part.mesh)
                    continue;
                const Material *material = part.material >= 0
                        && static_cast<size_t>(part.material) < m.materials().size()
                    ? m.materials()[static_cast<size_t>(part.material)].get()
                    : nullptr;

                // A separate child entity per primitive keeps one renderer
                // component per entity and lets multi-material meshes render
                // correctly. The child is at local identity, so the node
                // transform acts as the model placement.
                const std::string partName = namePrefix + node.name + "[" + std::to_string(partIndex) + "]";
                const bool skinned = node.skin >= 0
                    && static_cast<size_t>(node.skin) < m.skeletons().size();
                EntityId partEntity = addEntity(partName, entity, Transform{});

                if (skinned) {
                    SkinnedMeshRenderer renderer;
                    renderer.mesh = part.mesh.get();
                    renderer.material = material;
                    scene.add(partEntity, renderer);

                    Animator animator;
                    animator.skeleton = m.skeletons()[static_cast<size_t>(node.skin)];
                    animator.clips = m.clips();
                    scene.add(partEntity, std::move(animator));
                } else {
                    MeshRenderer renderer;
                    renderer.mesh = part.mesh.get();
                    renderer.material = material;
                    scene.add(partEntity, renderer);
                }
                result.meshEntities.push_back(partEntity);
            }
        }

        // Continue down the node hierarchy. Skinned primitive children carry
        // only a renderer; their joint transforms come from the Animator.
        for (int child : node.children)
            buildNode(child, entity);
    };

    for (int rootNode : m.roots())
        buildNode(rootNode, result.root);

    // Spawn facing is the node's -Z in *world* space. Deriving it from the
    // glTF node chain alone ignored rootTransform(): a yawed level rotated the
    // meshes but left every spawn pointing at the unrotated model orientation.
    scene.updateWorldMatrices();
    for (EntityId e : result.spawnPoints) {
        SpawnPoint *spawn = scene.find<SpawnPoint>(e);
        if (!spawn)
            continue;
        const glm::vec3 forward = -glm::vec3(scene.worldMatrix(e)[2]);
        const float len = glm::length(forward);
        spawn->forward = len > 1e-6f ? forward / len : glm::vec3(0.0f, 0.0f, -1.0f);
    }
    scene.refreshTriggerBounds();

    HOR_LOG_INFO("level '{}': {} entities ({} mesh, {} spawn, {} trigger, {} light)",
        baseName, result.entities.size(), result.meshEntities.size(),
        result.spawnPoints.size(), result.triggers.size(), result.lights.size());
    return result;
}

} // namespace horror
