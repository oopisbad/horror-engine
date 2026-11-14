#pragma once

#include "scene/Components.hpp"

#include <array>
#include <span>
#include <vector>

namespace horror {

class Scene;

struct CollisionHit {
    float time = 1.0f;
    glm::vec3 normal{0};
    EntityId entity = kInvalidEntity;
};

class StaticCollision {
public:
    using Triangle = std::array<glm::vec3, 3>;
    void build(std::span<const Triangle> triangles);
    bool empty() const { return nodes_.empty(); }
    void sweep(glm::vec3 center, glm::vec3 half, glm::vec3 delta, CollisionHit &hit) const;

private:
    struct Axis {
        glm::vec3 direction;
        float low, high;
    };
    struct Face {
        glm::vec3 low, high;
        std::array<Axis, 13> axes{};
        unsigned axisCount = 0;
    };
    struct Node {
        glm::vec3 low, high;
        unsigned first, count, right = 0;
    };

    unsigned split(unsigned first, unsigned count);
    void sweepNode(unsigned index, glm::vec3 center, glm::vec3 half,
        glm::vec3 delta, glm::vec3 low, glm::vec3 high, CollisionHit &hit) const;
    std::vector<Face> faces_;
    std::vector<Node> nodes_;
};

struct BoxBody {
    EntityId entity;
    glm::vec3 center;
    glm::vec3 half;
    glm::vec3 spawn;
    glm::vec3 localSpawn;
    glm::mat3 worldToParent{1.0f};
    glm::vec3 velocity{0};
};

// Imported static surfaces are baked in world space. Props are translating,
// non-rotating boxes kept separate from the immutable triangle hierarchy.
class LevelPhysics {
public:
    StaticCollision geometry;
    std::vector<BoxBody> props;

    CollisionHit sweep(glm::vec3 center, glm::vec3 half, glm::vec3 delta,
        EntityId ignore = kInvalidEntity, bool includeProps = true) const;
    void stepProps(Scene &scene, float dt, glm::vec3 playerCenter, glm::vec3 playerHalf);
    void push(EntityId entity, glm::vec3 velocity);
    void reset(Scene &scene);
};

struct CharacterMotion {
    glm::vec3 feet, velocity;
    bool grounded = false;
    bool stepped = false;
};

CharacterMotion moveImportedCharacter(LevelPhysics &physics, glm::vec3 feet,
    glm::vec3 velocity, glm::vec3 half, float dt, bool wasGrounded, bool jumping);

} // namespace horror
