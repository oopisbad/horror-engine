#include "asset/LevelBuilder.hpp"

#include "asset/GltfAsset.hpp"
#include "core/Log.hpp"
#include "render/Mesh.hpp"
#include "render/Texture.hpp"
#include "scene/Scene.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <functional>
#include <limits>
#include <filesystem>
#include <cmath>
#include <charconv>
#include <unordered_map>

namespace horror {

namespace {

bool startsWith(const std::string &s, const char *prefix)
{
    const size_t n = std::char_traits<char>::length(prefix);
    return s.size() >= n && s.compare(0, n, prefix) == 0;
}

const std::string &property(const GltfNode &node, const char *key)
{
    static const std::string empty;
    const auto found = node.extras.find(key);
    return found == node.extras.end() ? empty : found->second;
}

float scalar(const GltfNode &node, const char *key, float fallback, float low, float high)
{
    const auto &s = property(node, key);
    float value = 0.0f;
    const auto result = std::from_chars(s.data(), s.data()+s.size(), value);
    return result.ec == std::errc{} && result.ptr == s.data()+s.size() && std::isfinite(value)
        ? std::clamp(value, low, high) : fallback;
}

glm::vec3 tint(const GltfNode &node, glm::vec3 base)
{
    return {scalar(node,"color_r",base.r,0,4), scalar(node,"color_g",base.g,0,4),
        scalar(node,"color_b",base.b,0,4)};
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
    const std::string baseName = std::filesystem::path(m.path()).filename().string();
    if (options.bakePhysics)
        result.physics = std::make_unique<LevelPhysics>();
    std::vector<StaticCollision::Triangle> triangles;
    std::unordered_map<std::string, const Texture *> lightmaps;
    const auto levelFolder = std::filesystem::path(m.resolvedPath()).parent_path();

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
    std::function<void(int, EntityId, const glm::mat4 &)> buildNode = [&](int nodeIndex, EntityId parentEntity, const glm::mat4 &parentWorld) {
        const GltfNode &node = m.nodes()[static_cast<size_t>(nodeIndex)];
        Transform local;
        local.position = node.translation;
        local.rotation = node.rotation;
        local.scale = node.scale;

        const bool isCollision = startsWith(node.name, "COL_");
        const bool isSpawn = startsWith(node.name, "SPAWN_");
        const bool isTrigger = startsWith(node.name, "TRIGGER_");

        const glm::mat4 world = parentWorld * local.localMatrix();

        EntityId entity = addEntity(namePrefix + node.name, parentEntity, local);

        if (options.importExtras && !node.extras.empty()) {
            EntityExtras extras;
            extras.values = node.extras;
            scene.add(entity, std::move(extras));
        }

        const bool isFog = startsWith(node.name, "FOG_");
        const bool isProbe = startsWith(node.name, "PROBE_");
        const bool isDecal = startsWith(node.name, "DECAL_");
        if (options.importExtras) {
            if (isFog) {
                FogVolume volume;
                volume.color = tint(node, volume.color);
                volume.density = scalar(node, "density", volume.density, 0, 3);
                scene.add(entity, volume);
            }
            if (isProbe)
                scene.add(entity, ReflectionProbe{scalar(node, "strength", 1, 0, 2)});
            if (isDecal) {
                SurfaceDecal decal;
                if (startsWith(node.name, "DECAL_LEAK")) decal.pattern = SurfaceDecal::Pattern::Leak;
                if (startsWith(node.name, "DECAL_SCUFF")) decal.pattern = SurfaceDecal::Pattern::Scuff;
                decal.tint = tint(node, decal.tint);
                decal.strength = scalar(node, "strength", decal.strength, 0, 1);
                decal.wetness = scalar(node, "wetness", 0, 0, 1);
                scene.add(entity, decal);
            }
        }

        if (node.light >= 0 && options.importLights) {
            const GltfLight &light = m.lights()[static_cast<size_t>(node.light)];
            switch (light.type) {
            case GltfLight::Type::Point: {
                PointLight pl;
                pl.color = light.color;
                pl.intensity = light.intensity;
                pl.castShadow = property(node, "cast_shadow") == "true" || property(node, "cast_shadow") == "1";
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
                sl.castsShadows = property(node, "cast_shadow") == "true" || property(node, "cast_shadow") == "1";
                scene.add(entity, sl);
                result.lights.push_back(entity);
                break;
            }
            case GltfLight::Type::Directional:
                HOR_LOG_WARN("glTF '{}' node '{}': directional light '{}' is unsupported",
                    m.path(), node.name, light.name);
                break;
            }
        }

        // Mesh bounds drive collision/trigger volumes even for invisible
        // markers; mark that the node has a mesh before skipping rendering.
        const GltfMesh *mesh = node.mesh >= 0 ? &m.meshes()[static_cast<size_t>(node.mesh)] : nullptr;
        glm::vec3 localMin(0.0f), localMax(0.0f);
        const bool haveBounds = mesh && meshLocalBounds(*mesh, localMin, localMax);

        if (result.physics && options.importCollision && haveBounds && startsWith(node.name, "PROP_")) {
            if (node.skin < 0 && std::abs(glm::determinant(glm::mat3(parentWorld))) > 1e-8f) {
                const auto half = (localMax-localMin)*0.5f;
                const auto center = glm::vec3(world * glm::vec4((localMin+localMax)*0.5f,1));
                const auto extents = glm::abs(glm::vec3(world[0]))*half.x
                    + glm::abs(glm::vec3(world[1]))*half.y + glm::abs(glm::vec3(world[2]))*half.z;
                result.physics->props.push_back({entity,center,glm::max(extents,glm::vec3(0.01f)),
                    center,local.position,glm::inverse(glm::mat3(parentWorld))});
            } else {
                HOR_LOG_WARN("glTF '{}' node '{}': prop needs a nonsingular parent and an unskinned mesh", m.path(), node.name);
            }
        }

        if (isCollision && options.importCollision) {
            if (haveBounds && result.physics) {
                if (startsWith(node.name, "COL_MESH_")) {
                    for (const auto &part : mesh->parts) {
                        const auto &vertices = part.mesh->vertices();
                        const auto &indices = part.collisionIndices;
                        for (size_t i = 0; i+2 < indices.size(); i += 3) {
                            StaticCollision::Triangle triangle;
                            for (size_t k = 0; k < 3; ++k)
                                triangle[k] = glm::vec3(world * glm::vec4(vertices[indices[i+k]].position,1));
                            triangles.push_back(triangle);
                        }
                    }
                } else {
                    glm::vec3 corners[8];
                    for (unsigned i = 0; i < 8; ++i) {
                        const glm::vec3 p(i&1 ? localMax.x : localMin.x,
                            i&2 ? localMax.y : localMin.y, i&4 ? localMax.z : localMin.z);
                        corners[i] = glm::vec3(world * glm::vec4(p,1));
                    }
                    constexpr unsigned faces[][4] = {{0,1,3,2},{4,5,7,6},{0,1,5,4},
                        {2,3,7,6},{0,2,6,4},{1,3,7,5}};
                    for (const auto &f : faces) {
                        triangles.push_back({corners[f[0]],corners[f[1]],corners[f[2]]});
                        triangles.push_back({corners[f[0]],corners[f[2]],corners[f[3]]});
                    }
                }
            } else if (haveBounds) {
                AABBCollider collider;
                collider.center = (localMin + localMax) * 0.5f;
                collider.halfExtents = (localMax - localMin) * 0.5f;
                collider.resolveCollisions = true;
                scene.add(entity, collider);
            } else {
                HOR_LOG_WARN("glTF '{}' node '{}': collision marker needs a mesh", m.path(), node.name);
            }
        } else if (isSpawn && options.importSpawnPoints) {
            SpawnPoint spawn;
            spawn.tag = node.name.substr(6); // strip "SPAWN_"
            const glm::vec3 forward = -glm::vec3(world[2]);
            const float len = glm::length(forward);
            spawn.forward = len > 1e-6f ? forward / len : glm::vec3(0.0f, 0.0f, -1.0f);
            scene.add(entity, spawn);
            result.spawnPoints.push_back(entity);
        } else if (isTrigger && options.importTriggers) {
            if (!haveBounds)
                HOR_LOG_WARN("glTF '{}' node '{}': trigger needs mesh bounds", m.path(), node.name);
            TriggerVolume trigger;
            trigger.tag = node.name.substr(8); // strip "TRIGGER_"
            if (haveBounds) {
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
                scene.add(entity, trigger);
                result.triggers.push_back(entity);
            }
        } else if (!isCollision && !isSpawn && !isTrigger && !isFog && !isProbe && !isDecal && options.importMeshes && mesh) {
            for (size_t partIndex = 0; partIndex < mesh->parts.size(); ++partIndex) {
                const GltfMeshPart &part = mesh->parts[partIndex];
                if (!part.mesh)
                    continue;
                const Material *material = part.material >= 0
                        && static_cast<size_t>(part.material) < m.materials().size()
                    ? m.materials()[static_cast<size_t>(part.material)].get()
                    : nullptr;

                const auto &mapName = property(node, "lightmap");
                const float wetness = scalar(node, "wetness", 0, 0, 1);
                if (options.importExtras && material && (!mapName.empty() || wetness > 0)) {
                    auto *variant = scene.createMaterial();
                    *variant = *material;
                    material = variant;
                    variant->wetness = wetness;
                    if (!mapName.empty() && part.hasLightmapUv && node.skin < 0) {
                        auto [cached, fresh] = lightmaps.try_emplace(mapName, nullptr);
                        if (fresh) {
                            Texture::Params params;
                            params.wrapS = params.wrapT = GL_CLAMP_TO_EDGE;
                            const auto path = levelFolder / mapName;
                            auto texture = std::make_unique<Texture>(path.string(), params);
                            if (!texture->isFallback())
                                cached->second = scene.addTexture(std::move(texture));
                            else
                                HOR_LOG_WARN("glTF '{}' node '{}': lightmap '{}' is unavailable", m.path(), node.name, mapName);
                        }
                        variant->lightmap = cached->second;
                        variant->lightmapStrength = scalar(node, "lightmap_strength", 1, 0, 8);
                        if (variant->lightmap)
                            ++result.lightmappedMeshes;
                    } else if (!mapName.empty()) {
                        HOR_LOG_WARN("glTF '{}' node '{}': lightmaps need static geometry with TEXCOORD_1", m.path(), node.name);
                    }
                }

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
            buildNode(child, entity, world);
    };

    for (int rootNode : m.roots())
        buildNode(rootNode, result.root, rootTransform.localMatrix());
    if (result.physics)
        result.physics->geometry.build(triangles);

    HOR_LOG_INFO("level '{}': {} entities ({} mesh, {} spawn, {} trigger, {} light)",
        baseName, result.entities.size(), result.meshEntities.size(),
        result.spawnPoints.size(), result.triggers.size(), result.lights.size());
    return result;
}

} // namespace horror
