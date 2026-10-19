#include "scene/Scene.hpp"

#include "core/Log.hpp"
#include "render/Material.hpp"
#include "render/Mesh.hpp"
#include "render/Shader.hpp"

#include <algorithm>
#include <functional>
#include <limits>

namespace horror {

glm::mat4 Transform::localMatrix() const
{
    glm::mat4 m = glm::mat4_cast(rotation);
    m[0] *= scale.x;
    m[1] *= scale.y;
    m[2] *= scale.z;
    m[3] = glm::vec4(position, 1.0f);
    return m;
}

glm::mat4 Transform::compose(const glm::mat4 &parentWorld, const Transform &child)
{
    return parentWorld * child.localMatrix();
}

Scene::Scene() = default;

Scene::~Scene() = default;

EntityId Scene::create(const std::string &name, EntityId parent)
{
    EntityId id = static_cast<EntityId>(entityRecords_.size());
    entityRecords_.push_back({ name, true });
    names_.push_back(name);
    transforms_.emplace_back();
    worldMatrices_.emplace_back(1.0f);
    if (parent != kInvalidEntity)
        attachTo(id, parent);
    return id;
}

void Scene::destroy(EntityId e)
{
    if (!valid(e))
        return;
    entityRecords_[e].alive = false;
    // Detach from parent's children list so the hierarchy stays coherent.
    EntityId p = transforms_[e].parent;
    if (p != kInvalidEntity) {
        auto &siblings = transforms_[p].children;
        std::erase(siblings, e);
        transforms_[e].parent = kInvalidEntity;
    }
    stores_.meshRenderers.erase(e);
    stores_.pointLights.erase(e);
    stores_.spotLights.erase(e);
    stores_.aabbColliders.erase(e);
    stores_.characterControllers.erase(e);
}

const std::string &Scene::name(EntityId e) const
{
    static const std::string invalid = "invalid";
    return valid(e) ? entityRecords_[e].name : invalid;
}

void Scene::attachTo(EntityId child, EntityId parent)
{
    HOR_CHECK(child != parent);
    EntityId oldParent = transforms_[child].parent;
    if (oldParent != kInvalidEntity)
        std::erase(transforms_[oldParent].children, child);
    transforms_[child].parent = parent;
    if (parent != kInvalidEntity)
        transforms_[parent].children.push_back(child);
}

// Depth-first pass guards against a cyclic graph accidentally built by
// gameplay code (fatal log + subtree skipped in that case).
void Scene::updateWorldMatrices()
{
    static constexpr int kMaxHierarchyDepth = 64;
    const glm::mat4 identity(1.0f);

    std::function<void(EntityId, const glm::mat4 &, int)> walk =
        [&](EntityId e, const glm::mat4 &parentWorld, int depth) {
            if (depth > kMaxHierarchyDepth) {
                HOR_LOG_FATAL("transform hierarchy too deep (cycle?)");
                return;
            }
            const Transform &t = transform(e);
            glm::mat4 world = Transform::compose(parentWorld, t);
            worldMatrices_[e] = world;
            for (EntityId child : t.children)
                walk(child, world, depth + 1);
        };

    for (size_t i = 0; i < transforms_.size(); ++i) {
        if (transforms_[i].parent == kInvalidEntity && entityRecords_[i].alive)
            walk(static_cast<EntityId>(i), identity, 0);
    }
}

void Scene::refreshColliderBounds()
{
    view<AABBCollider>([this](EntityId e, AABBCollider &col) {
        const glm::mat4 &world = worldMatrices_[e];
        // Transform the 8 local corners: handles scale + rotation exactly.
        glm::vec3 mn(std::numeric_limits<float>::max());
        glm::vec3 mx(std::numeric_limits<float>::lowest());
        for (int i = 0; i < 8; ++i) {
            glm::vec3 local = col.center + col.halfExtents
                * glm::vec3((i & 1) ? 1.f : -1.f, (i & 2) ? 1.f : -1.f, (i & 4) ? 1.f : -1.f);
            glm::vec3 worldCorner = glm::vec3(world * glm::vec4(local, 1.0f));
            mn = glm::min(mn, worldCorner);
            mx = glm::max(mx, worldCorner);
        }
        col.worldMin = mn;
        col.worldMax = mx;
    });
}

Material *Scene::createMaterial(const std::string &name)
{
    // Material is a plain struct in Material.hpp; pooling via unique_ptr keeps
    // pointers stable while gameplay mutates surface parameters in place.
    materialPool_.push_back(std::make_unique<Material>());
    materialPool_.back()->name = name;
    return materialPool_.back().get();
}

Mesh *Scene::addMesh(std::unique_ptr<Mesh> mesh)
{
    meshPool_.push_back(std::move(mesh));
    return meshPool_.back().get();
}

} // namespace horror
