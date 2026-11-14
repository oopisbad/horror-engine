#pragma once

#include "core/Log.hpp"
#include "scene/Components.hpp"
#include "render/RenderVolumes.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <memory>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace horror {

class Mesh;
class Shader;
class Texture;
struct Material;

// Scene = component storage + transform hierarchy.
//
// Chosen over a full ECS library for the first pass: tens of entities, a
// handful of systems, zero archetypes -- a plain map-per-component gives all
// the decoupling we need with zero dependencies. The public API surface is
// small enough that migrating to a real archetypal ECS later won't ripple
// into gameplay call sites.
class Scene {
public:
    Scene();
    ~Scene();

    EntityId create(const std::string &name, EntityId parent = kInvalidEntity);
    // NOTE destroy(): tombstones the record; entity ids are never recycled,
    // which also means stale component entries are simply dropped.
    void destroy(EntityId e);

    const std::string &name(EntityId e) const;
    EntityId parentOf(EntityId e) const { return transforms_[e].parent; }
    bool valid(EntityId e) const { return e < entityRecords_.size() && entityRecords_[e].alive; }
    size_t entityCount() const { return entityRecords_.size(); }

    // --- component access ---------------------------------------------------
    // Every entity has a Transform; other components are optional and live
    // in their own maps (see stores_ below).
    Transform &transform(EntityId e) { return transforms_[e]; }
    const Transform &transform(EntityId e) const { return transforms_[e]; }

    template<typename T>
    T &add(EntityId e, T component)
    {
        return storesFor<T>()[e] = std::move(component);
    }

    template<typename T>
    T *find(EntityId e)
    {
        auto &store = storesFor<T>();
        auto it = store.find(e);
        return it == store.end() ? nullptr : &it->second;
    }

    template<typename T>
    bool has(EntityId e) const
    {
        return const_cast<Scene *>(this)->find<T>(e) != nullptr;
    }

    template<typename T, typename Fn>
    void view(Fn &&fn)
    {
        auto &store = storesFor<T>();
        for (auto &[id, comp] : store)
            if (valid(id))
                fn(id, comp);
    }

    // Mutable const-free view is intentionally omitted for now; gameplay
    // mutates components through the same typed stores.

    // --- hierarchy ----------------------------------------------------------
    void attachTo(EntityId child, EntityId parent); // kInvalidEntity = root

    // --- world transforms ---------------------------------------------------
    // Cached world matrices recomputed lazily by updateWorldMatrices().
    glm::mat4 worldMatrix(EntityId e) const
    {
        HOR_CHECK(e < worldMatrices_.size());
        return worldMatrices_[e];
    }
    void updateWorldMatrices();
    // World-space AABB bounds derived from the collider's local box + transform.
    void refreshColliderBounds();

    // Materials imported from glTF are owned by the GltfModel in the asset
    // cache; procedural ones live in the pool below.
    // Material/mesh pools the scene owns (mesh/material pointers handed to
    // components point into these; RAII keeps them valid for scene lifetime).
    // TODO(assets): replace with a streamed asset system + refcounted handles.
    Texture *addTexture(std::unique_ptr<Texture> texture);
    Material *createMaterial(const std::string &name = "material");
    Mesh *addMesh(std::unique_ptr<Mesh> mesh);

    const std::vector<std::string> &entityNames() const { return names_; }

private:
    struct EntityRecord {
        std::string name;
        bool alive = true;
    };

    // Header-defined dispatch (if constexpr over type identity) because the
    // inline add/find/view call sites need the return type deducible here.
    template<typename T>
    auto &storesFor()
    {
        if constexpr (std::is_same_v<T, MeshRenderer>)
            return stores_.meshRenderers;
        else if constexpr (std::is_same_v<T, PointLight>)
            return stores_.pointLights;
        else if constexpr (std::is_same_v<T, SpotLight>)
            return stores_.spotLights;
        else if constexpr (std::is_same_v<T, AABBCollider>)
            return stores_.aabbColliders;
        else if constexpr (std::is_same_v<T, CharacterController>)
            return stores_.characterControllers;
        else if constexpr (std::is_same_v<T, SkinnedMeshRenderer>)
            return stores_.skinnedMeshRenderers;
        else if constexpr (std::is_same_v<T, Animator>)
            return stores_.animators;
        else if constexpr (std::is_same_v<T, SpawnPoint>)
            return stores_.spawnPoints;
        else if constexpr (std::is_same_v<T, TriggerVolume>)
            return stores_.triggerVolumes;
        else if constexpr (std::is_same_v<T, FogVolume>)
            return stores_.fogVolumes;
        else if constexpr (std::is_same_v<T, ReflectionProbe>)
            return stores_.reflectionProbes;
        else if constexpr (std::is_same_v<T, SurfaceDecal>)
            return stores_.surfaceDecals;
        else if constexpr (std::is_same_v<T, EntityExtras>)
            return stores_.entityExtras;
    }

    struct Stores {
        std::unordered_map<EntityId, MeshRenderer> meshRenderers;
        std::unordered_map<EntityId, PointLight> pointLights;
        std::unordered_map<EntityId, SpotLight> spotLights;
        std::unordered_map<EntityId, AABBCollider> aabbColliders;
        std::unordered_map<EntityId, CharacterController> characterControllers;
        std::unordered_map<EntityId, SkinnedMeshRenderer> skinnedMeshRenderers;
        std::unordered_map<EntityId, Animator> animators;
        std::unordered_map<EntityId, SpawnPoint> spawnPoints;
        std::unordered_map<EntityId, TriggerVolume> triggerVolumes;
        std::unordered_map<EntityId, EntityExtras> entityExtras;
        std::unordered_map<EntityId, FogVolume> fogVolumes;
        std::unordered_map<EntityId, ReflectionProbe> reflectionProbes;
        std::unordered_map<EntityId, SurfaceDecal> surfaceDecals;
    };

    std::vector<EntityRecord> entityRecords_;
    std::vector<Transform> transforms_;
    std::vector<glm::mat4> worldMatrices_;
    bool worldDirty_ = true;

    Stores stores_;
    std::vector<std::string> names_;

    std::vector<std::unique_ptr<Texture>> texturePool_;
    std::vector<std::unique_ptr<Material>> materialPool_;
    std::vector<std::unique_ptr<Mesh>> meshPool_;

    friend class ForwardRenderer;
};

} // namespace horror
