#include "scene/Collision.hpp"
#include "scene/Scene.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace horror {
namespace {

constexpr float skin = 0.001f;
constexpr float floorNormal = 0.7071f;
constexpr std::array<glm::vec3, 3> basis{{{1,0,0}, {0,1,0}, {0,0,1}}};

bool intersects(glm::vec3 lo, glm::vec3 hi, glm::vec3 otherLo, glm::vec3 otherHi)
{
    return glm::all(glm::lessThanEqual(lo, otherHi))
        && glm::all(glm::greaterThanEqual(hi, otherLo));
}

struct Interval {
    float enter = -std::numeric_limits<float>::infinity();
    float exit = 1.0f;
    glm::vec3 normal{0};

    bool clip(glm::vec3 axis, float center, float speed, float low, float high)
    {
        if (std::abs(speed) < 1e-8f)
            return center >= low && center <= high;
        float a = (low - center) / speed;
        float b = (high - center) / speed;
        if (a > b)
            std::swap(a, b);
        if (a > enter) {
            enter = a;
            normal = speed > 0 ? -axis : axis;
        }
        exit = std::min(exit, b);
        return enter <= exit;
    }
};

void sweepBox(glm::vec3 center, glm::vec3 half, glm::vec3 delta,
    glm::vec3 otherCenter, glm::vec3 otherHalf, EntityId entity, CollisionHit &hit)
{
    const auto radius = half + otherHalf;
    Interval interval;
    interval.exit = hit.time;
    for (int i = 0; i < 3; ++i) {
        if (!interval.clip(basis[i], center[i], delta[i],
                otherCenter[i] - radius[i], otherCenter[i] + radius[i]))
            return;
    }
    if (interval.enter >= -1e-5f && interval.enter < hit.time)
        hit = {std::max(0.0f, interval.enter), interval.normal, entity};
}

glm::vec3 advance(glm::vec3 position, glm::vec3 delta, const CollisionHit &hit)
{
    if (hit.time >= 1.0f)
        return position + delta;
    const float closing = -glm::dot(delta, hit.normal);
    return position + delta * std::max(0.0f, hit.time - skin / std::max(closing, skin));
}

void removeInward(glm::vec3 &v, glm::vec3 normal)
{
    v -= normal * std::min(0.0f, glm::dot(v, normal));
}

} // namespace

void StaticCollision::build(std::span<const Triangle> triangles)
{
    faces_.clear();
    nodes_.clear();
    faces_.reserve(triangles.size());
    for (const auto &t : triangles) {
        const glm::vec3 edges[] = {t[1]-t[0], t[2]-t[1], t[0]-t[2]};
        const auto normal = glm::cross(edges[0], edges[1]);
        if (glm::dot(normal, normal) < 1e-12f)
            continue;
        Face face;
        face.low = glm::min(t[0], glm::min(t[1], t[2]));
        face.high = glm::max(t[0], glm::max(t[1], t[2]));
        const auto addAxis = [&](glm::vec3 axis) {
            const float square = glm::dot(axis, axis);
            if (square < 1e-12f)
                return;
            axis /= std::sqrt(square);
            const float a = glm::dot(axis,t[0]), b = glm::dot(axis,t[1]), c = glm::dot(axis,t[2]);
            face.axes[face.axisCount++] = {axis, std::min({a,b,c}), std::max({a,b,c})};
        };
        addAxis(normal);
        for (auto axis : basis) {
            addAxis(axis);
            for (auto edge : edges)
                addAxis(glm::cross(axis, edge));
        }
        faces_.push_back(face);
    }
    if (!faces_.empty())
        split(0, static_cast<unsigned>(faces_.size()));
}

unsigned StaticCollision::split(unsigned first, unsigned count)
{
    Node node{faces_[first].low, faces_[first].high, first, count};
    for (unsigned i = first + 1; i < first + count; ++i) {
        node.low = glm::min(node.low, faces_[i].low);
        node.high = glm::max(node.high, faces_[i].high);
    }
    const unsigned index = static_cast<unsigned>(nodes_.size());
    nodes_.push_back(node);
    if (count <= 6)
        return index;
    const auto size = node.high - node.low;
    const int axis = size.x >= size.y && size.x >= size.z ? 0 : (size.y >= size.z ? 1 : 2);
    const unsigned middle = first + count / 2;
    std::nth_element(faces_.begin()+first, faces_.begin()+middle, faces_.begin()+first+count,
        [axis](const Face &a, const Face &b) {
            return a.low[axis]+a.high[axis] < b.low[axis]+b.high[axis];
        });
    split(first, middle-first);
    const unsigned right = split(middle, first+count-middle);
    nodes_[index].right = right;
    nodes_[index].count = 0;
    return index;
}

void StaticCollision::sweep(glm::vec3 center, glm::vec3 half, glm::vec3 delta,
    CollisionHit &hit) const
{
    if (nodes_.empty() || glm::dot(delta, delta) < 1e-14f)
        return;
    const auto end = center + delta;
    sweepNode(0, center, half, delta, glm::min(center, end)-half,
        glm::max(center,end)+half, hit);
}

void StaticCollision::sweepNode(unsigned index, glm::vec3 center, glm::vec3 half,
    glm::vec3 delta, glm::vec3 low, glm::vec3 high, CollisionHit &hit) const
{
    const auto &node = nodes_[index];
    if (!intersects(low, high, node.low, node.high))
        return;
    if (!node.count) {
        sweepNode(index+1, center, half, delta, low, high, hit);
        sweepNode(node.right, center, half, delta, low, high, hit);
        return;
    }
    for (unsigned i = node.first; i < node.first+node.count; ++i) {
        const auto &face = faces_[i];
        if (!intersects(low, high, face.low, face.high))
            continue;
        Interval interval;
        interval.exit = hit.time;
        for (unsigned j = 0; j < face.axisCount; ++j) {
            const auto &axis = face.axes[j];
            const float extent = glm::dot(glm::abs(axis.direction), half);
            if (!interval.clip(axis.direction, glm::dot(center,axis.direction),
                    glm::dot(delta,axis.direction), axis.low-extent, axis.high+extent)) {
                interval.enter = -1.0f;
                break;
            }
        }
        if (interval.enter >= -1e-5f && interval.enter < hit.time)
            hit = {std::max(0.0f,interval.enter), interval.normal, kInvalidEntity};
    }
}

CollisionHit LevelPhysics::sweep(glm::vec3 center, glm::vec3 half, glm::vec3 delta,
    EntityId ignore, bool includeProps) const
{
    CollisionHit hit;
    geometry.sweep(center, half, delta, hit);
    if (includeProps)
        for (const auto &prop : props)
            if (prop.entity != ignore)
                sweepBox(center,half,delta,prop.center,prop.half,prop.entity,hit);
    return hit;
}

void LevelPhysics::push(EntityId entity, glm::vec3 velocity)
{
    if (entity == kInvalidEntity)
        return;
    for (auto &prop : props) {
        if (prop.entity == entity) {
            velocity.y = 0.0f;
            prop.velocity += velocity;
            const float speed = glm::length(prop.velocity);
            if (speed > 5.0f)
                prop.velocity *= 5.0f / speed;
            return;
        }
    }
}

void LevelPhysics::stepProps(Scene &scene, float dt, glm::vec3 playerCenter, glm::vec3 playerHalf)
{
    for (auto &prop : props) {
        prop.velocity.y = std::max(-30.0f, prop.velocity.y - 9.81f*dt);
        glm::vec3 remainder = prop.velocity * dt;
        for (int i = 0; i < 4 && glm::dot(remainder,remainder) > 1e-10f; ++i) {
            auto hit = sweep(prop.center, prop.half, remainder, prop.entity);
            sweepBox(prop.center, prop.half, remainder, playerCenter, playerHalf, kInvalidEntity, hit);
            prop.center = advance(prop.center, remainder, hit);
            if (hit.time >= 1.0f)
                break;
            remainder *= 1.0f-hit.time;
            removeInward(remainder, hit.normal);
            removeInward(prop.velocity, hit.normal);
            if (hit.normal.y > floorNormal) {
                const float damping = std::max(0.0f, 1.0f-7.0f*dt);
                prop.velocity.x *= damping;
                prop.velocity.z *= damping;
            }
        }
        if (prop.center.y < prop.spawn.y-35.0f) {
            prop.center = prop.spawn;
            prop.velocity = glm::vec3(0);
        }
        scene.transform(prop.entity).position = prop.localSpawn + prop.worldToParent * (prop.center - prop.spawn);
    }
}

void LevelPhysics::reset(Scene &scene)
{
    for (auto &prop : props) {
        prop.center = prop.spawn;
        prop.velocity = glm::vec3(0);
        scene.transform(prop.entity).position = prop.localSpawn + prop.worldToParent * (prop.center - prop.spawn);
    }
}

CharacterMotion moveImportedCharacter(LevelPhysics &physics, glm::vec3 feet,
    glm::vec3 velocity, glm::vec3 half, float dt, bool wasGrounded, bool jumping)
{
    const glm::vec3 offset(0,half.y,0);
    const glm::vec3 start = feet + offset;
    glm::vec3 center = start;
    glm::vec3 remainder = velocity * dt;
    bool grounded = false;
    bool stepped = false;
    bool triedStep = false;
    for (int i = 0; i < 5 && glm::dot(remainder,remainder) > 1e-10f; ++i) {
        const auto hit = physics.sweep(center,half,remainder);
        if (hit.time >= 1.0f) {
            center += remainder;
            break;
        }
        const bool floor = hit.normal.y >= floorNormal;
        grounded |= floor && remainder.y <= 0.0f;
        if (!triedStep && !floor && std::abs(hit.normal.y) < floorNormal && wasGrounded && !jumping) {
            triedStep = true;
            const glm::vec3 up(0,0.31f,0);
            const glm::vec3 across(remainder.x,0,remainder.z);
            if (physics.sweep(center,half,up).time >= 1.0f
                && physics.sweep(center+up,half,across).time >= 1.0f) {
                const auto raised = center+up+across;
                const glm::vec3 down(0,-0.41f,0);
                const auto landing = physics.sweep(raised,half,down);
                if (landing.time < 1.0f && landing.normal.y >= floorNormal
                    && raised.y + down.y * landing.time > start.y + 0.004f) {
                    center = advance(raised,down,landing);
                    velocity.y = 0.0f;
                    grounded = stepped = true;
                    break;
                }
            }
        }
        if (std::abs(hit.normal.y) < floorNormal)
            physics.push(hit.entity, -hit.normal * std::max(0.0f, -glm::dot(velocity,hit.normal)) * 0.55f);
        center = advance(center,remainder,hit);
        remainder *= 1.0f-hit.time;
        // Prevent steep slopes from converting forward movement into climbing.
        auto slideNormal = hit.normal;
        if (slideNormal.y > 0.0f && !floor)
            slideNormal = glm::normalize(glm::vec3(slideNormal.x,0,slideNormal.z));
        removeInward(remainder,slideNormal);
        removeInward(velocity,slideNormal);
    }
    if (!jumping && (wasGrounded || grounded) && velocity.y <= 0.0f) {
        const glm::vec3 down(0,-0.12f,0);
        const auto hit = physics.sweep(center,half,down);
        if (hit.time < 1.0f && hit.normal.y >= floorNormal) {
            center = advance(center,down,hit);
            grounded = true;
        } else {
            grounded = false;
        }
    }
    if (grounded)
        velocity.y = 0.0f;
    return {center-offset,velocity,grounded,stepped};
}

} // namespace horror
