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
    scene.transform(result.root) = rootTransform;
    result.entities.push_back(result.root);

    auto addEntity = [&](const std::string &name, EntityId parent, const Transform &transform) {
        EntityId e = scene.create(name, parent);
        scene.transform(e) = transform;
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
            // Still walk children so a disabled marker doesn't orphan a
            // subtree; children attach to the parent's entity instead.
            for (int child : node.children)
                buildNode(child, parentEntity);
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
            SpawnPoint spawn;
            spawn.tag = node.name.substr(6); // strip "SPAWN_"
            const glm::mat4 world = m.restNodeWorldMatrix(nodeIndex);
            const glm::vec3 forward = -glm::vec3(world[2]);
            const float len = glm::length(forward);
            spawn.forward = len > 1e-6f ? forward / len : glm::vec3(0.0f, 0.0f, -1.0f);
            scene.add(entity, spawn);
            result.spawnPoints.push_back(entity);
        } else if (isTrigger) {
            TriggerVolume trigger;
            trigger.tag = node.name.substr(8); // strip "TRIGGER_"
            if (haveBounds) {
                const glm::mat4 world = m.restNodeWorldMatrix(nodeIndex);
                glm::vec3 mn(std::numeric_limits<float>::max());
                glm::vec3 mx(-std::numeric_limits<float>::max());
                for (int corner = 0; corner < 8; ++corner) {
                    const glm::vec3 local(
                        (corner & 1) ? localMax.x : localMin.x,
                        (corner & 2) ? localMax.y : localMin.y,
                        (corner & 4) ? localMax.z : localMin.z);
                    const glm::vec3 p = glm::vec3(world * glm::vec4(local, 1.0f));
                    mn = glm::min(mn, p);
                    mx = glm::max(mx, p);
                }
                trigger.worldMin = mn;
                trigger.worldMax = mx;
            }
            scene.add(entity, trigger);
            result.triggers.push_back(entity);
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

    HOR_LOG_INFO("level '{}': {} entities ({} mesh, {} spawn, {} trigger, {} light)",
        baseName, result.entities.size(), result.meshEntities.size(),
        result.spawnPoints.size(), result.triggers.size(), result.lights.size());
    return result;
}

} // namespace horror
