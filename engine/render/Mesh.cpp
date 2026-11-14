#include "render/Mesh.hpp"

#include "core/Log.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace horror {

Mesh::Mesh(Geometry geometry, std::string name)
    : vertices_(std::move(geometry.vertices))

    , name_(name.empty() ? "mesh" : std::move(name))
{
    if (vertices_.empty() || geometry.indices.empty()
        || geometry.indices.size() > static_cast<size_t>(std::numeric_limits<GLsizei>::max()))
        throw std::invalid_argument("Invalid mesh size");
    const auto largestIndex = *std::max_element(geometry.indices.begin(), geometry.indices.end());
    if (largestIndex >= vertices_.size())
        throw std::invalid_argument("Mesh index outside vertex buffer");
    indexCount_ = static_cast<GLsizei>(geometry.indices.size());
    if (largestIndex <= std::numeric_limits<uint16_t>::max())
        indexType_ = GL_UNSIGNED_SHORT;
    upload(geometry.indices);
    computeBounds();
}

Mesh::~Mesh()
{
    if (vbo_)
        glDeleteBuffers(1, &vbo_);
    if (ebo_)
        glDeleteBuffers(1, &ebo_);
    if (vao_)
        glDeleteVertexArrays(1, &vao_);
}

Mesh::Mesh(Mesh &&o) noexcept
    : vertices_(std::move(o.vertices_))
    , indexCount_(o.indexCount_)
    , indexType_(o.indexType_)
    , name_(std::move(o.name_))
    , vao_(o.vao_)
    , vbo_(o.vbo_)
    , ebo_(o.ebo_)
    , minBounds_(o.minBounds_)
    , maxBounds_(o.maxBounds_)
{
    o.vao_ = o.vbo_ = o.ebo_ = 0;
}

Mesh &Mesh::operator=(Mesh &&o) noexcept
{
    if (this == &o)
        return *this;
    if (vbo_)
        glDeleteBuffers(1, &vbo_);
    if (ebo_)
        glDeleteBuffers(1, &ebo_);
    if (vao_)
        glDeleteVertexArrays(1, &vao_);
    vertices_ = std::move(o.vertices_);
    indexCount_ = o.indexCount_;
    indexType_ = o.indexType_;
    name_ = std::move(o.name_);
    vao_ = o.vao_;
    vbo_ = o.vbo_;
    ebo_ = o.ebo_;
    minBounds_ = o.minBounds_;
    maxBounds_ = o.maxBounds_;
    o.vao_ = o.vbo_ = o.ebo_ = 0;
    return *this;
}

void Mesh::upload(const std::vector<uint32_t> &indices)
{
    glCreateBuffers(1, &vbo_);
    glNamedBufferStorage(vbo_, vertices_.size() * sizeof(Vertex), vertices_.data(), 0 /* immutable */);
    glCreateBuffers(1, &ebo_);
    if (indexType_ == GL_UNSIGNED_SHORT) {
        const std::vector<uint16_t> compact(indices.begin(), indices.end());
        glNamedBufferStorage(ebo_, compact.size() * sizeof(uint16_t), compact.data(), 0);
    } else {
        glNamedBufferStorage(ebo_, indices.size() * sizeof(uint32_t), indices.data(), 0);
    }

    glCreateVertexArrays(1, &vao_);
    // Bind buffers upfront (DSA path); attributes reference them by offset.
    glVertexArrayVertexBuffer(vao_, 0, vbo_, 0, sizeof(Vertex));
    glVertexArrayElementBuffer(vao_, ebo_);

    // attribute 0: position vec3
    glEnableVertexArrayAttrib(vao_, 0);
    glVertexArrayAttribFormat(vao_, 0, 3, GL_FLOAT, GL_FALSE, offsetof(Vertex, position));
    glVertexArrayAttribBinding(vao_, 0, 0);
    // attribute 1: normal vec3
    glEnableVertexArrayAttrib(vao_, 1);
    glVertexArrayAttribFormat(vao_, 1, 3, GL_FLOAT, GL_FALSE, offsetof(Vertex, normal));
    glVertexArrayAttribBinding(vao_, 1, 0);
    // attribute 2: uv vec2
    glEnableVertexArrayAttrib(vao_, 2);
    glVertexArrayAttribFormat(vao_, 2, 2, GL_FLOAT, GL_FALSE, offsetof(Vertex, uv));
    glVertexArrayAttribBinding(vao_, 2, 0);
    // attribute 3: tangent vec4 (w = handedness)
    glEnableVertexArrayAttrib(vao_, 3);
    glVertexArrayAttribFormat(vao_, 3, 4, GL_FLOAT, GL_FALSE, offsetof(Vertex, tangent));
    glVertexArrayAttribBinding(vao_, 3, 0);
    // attribute 4: joints uvec4 -- integer format, not normalized floats.
    glEnableVertexArrayAttrib(vao_, 4);
    glVertexArrayAttribIFormat(vao_, 4, 4, GL_UNSIGNED_INT, offsetof(Vertex, joints));
    glVertexArrayAttribBinding(vao_, 4, 0);
    glEnableVertexArrayAttrib(vao_, 6);
    glVertexArrayAttribFormat(vao_, 6, 2, GL_FLOAT, GL_FALSE, offsetof(Vertex, lightmapUv));
    glVertexArrayAttribBinding(vao_, 6, 0);
    // attribute 5: weights vec4 (sum normalized to 1 by the loader)
    glEnableVertexArrayAttrib(vao_, 5);
    glVertexArrayAttribFormat(vao_, 5, 4, GL_FLOAT, GL_FALSE, offsetof(Vertex, weights));
    glVertexArrayAttribBinding(vao_, 5, 0);

    HOR_LOG_TRACE("mesh '{}' uploaded: {} verts, {} indices", name_, vertices_.size(), indices.size());
}

void Mesh::computeBounds()
{
    minBounds_ = glm::vec3(std::numeric_limits<float>::max());
    maxBounds_ = glm::vec3(std::numeric_limits<float>::lowest());
    for (const auto &v : vertices_) {
        minBounds_ = glm::min(minBounds_, v.position);
        maxBounds_ = glm::max(maxBounds_, v.position);
    }
}

void Mesh::bind() const
{
    HOR_CHECK(vao_ != 0);
    glBindVertexArray(vao_);
}

namespace {
// Quad helper: two CCW triangles (a,b,c,d).
void addQuad(std::vector<uint32_t> &indices, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    indices.insert(indices.end(), { a, b, c, a, c, d });
}
} // namespace

Mesh::Geometry Mesh::makeCubeGeometry(float halfSize)
{
    Geometry g;
    g.vertices.resize(24); // 6 faces * 4 verts; corners split per face so each
                           // face gets a crisp normal instead of a shared one
    g.indices.reserve(36);
    const float s = halfSize;

    static const glm::vec3 normals[6] = {
        { 0, 0, 1 }, { 0, 0, -1 }, { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 },
    };

    uint32_t idx = 0;
    for (int f = 0; f < 6; ++f) {
        const glm::vec3 n = normals[f];
        // Build a face tangent basis with cross(u, v) == -n. The quad below is
        // wound a-b-c/a-c-d, whose geometric normal works out to -cross(u, v),
        // so this yields CCW-outward faces for every side. (Hand-written u/v
        // tables per face are easy to get backwards -- four of six were.)
        const glm::vec3 helper = std::abs(n.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
        const glm::vec3 u = glm::normalize(glm::cross(helper, n));
        const glm::vec3 v = glm::cross(u, n);

        const glm::vec3 corner = n * s; // face center on the half-size shell
        const glm::vec3 cu = u * s;
        const glm::vec3 cv = v * s;
        // Quad corners: a=+u+v, b=+u-v, c=-u-v, d=-u+v.
        glm::vec3 a = corner + cu + cv;
        glm::vec3 b = corner + cu - cv;
        glm::vec3 c = corner - cu - cv;
        glm::vec3 d = corner - cu + cv;
        g.vertices[f * 4 + 0] = { a, n, glm::vec2(1, 1) };
        g.vertices[f * 4 + 1] = { b, n, glm::vec2(1, 0) };
        g.vertices[f * 4 + 2] = { c, n, glm::vec2(0, 0) };
        g.vertices[f * 4 + 3] = { d, n, glm::vec2(0, 1) };
        addQuad(g.indices, idx + 0, idx + 1, idx + 2, idx + 3);
        idx += 4;
    }
    return g;
}

Mesh::Geometry Mesh::makeSphereGeometry(float radius, int lon, int lat)
{
    Geometry g;
    HOR_CHECK(lon >= 3 && lat >= 2);
    // Standard lat/long sphere; rings and segments; normals == normalized pos.
    for (int i = 0; i <= lat; ++i) {
        float v = float(i) / float(lat);
        float phi = v * 3.141592653589f;
        for (int j = 0; j <= lon; ++j) {
            float u = float(j) / float(lon);
            float theta = u * 2.0f * 3.141592653589f;
            glm::vec3 pos(
                std::cos(theta) * std::sin(phi),
                std::cos(phi),
                std::sin(theta) * std::sin(phi));
            g.vertices.push_back({ pos * radius, pos, glm::vec2(u, v) });
        }
    }
    for (int i = 0; i < lat; ++i) {
        for (int j = 0; j < lon; ++j) {
            uint32_t a = i * (lon + 1) + j;
            uint32_t b = a + lon + 1;
            // CCW seen from outside (a -> a+1 -> b, then b -> a+1 -> b+1);
            // the naive (a,b,a+1) order winds inward and back-face culling
            // then hides the whole sphere.
            g.indices.push_back(a); g.indices.push_back(a + 1); g.indices.push_back(b);
            g.indices.push_back(b); g.indices.push_back(a + 1); g.indices.push_back(b + 1);
        }
    }
    return g;
}

Mesh::Geometry Mesh::makePlaneGeometry(float halfExtent, int subdivisions)
{
    Geometry g;
    HOR_CHECK(subdivisions >= 1);
    int n = subdivisions;
    // XY-plane quads subdivided; upward face normal +Z (for a floor, the
    // scene transform rotates it flat).
    for (int i = 0; i <= n; ++i) {
        for (int j = 0; j <= n; ++j) {
            float u = float(j) / float(n);
            float v = float(i) / float(n);
            glm::vec2 p(-halfExtent + 2 * halfExtent * u, -halfExtent + 2 * halfExtent * v);
            g.vertices.push_back({ glm::vec3(p, 0.0f), glm::vec3(0, 0, 1), glm::vec2(u, v) });
        }
    }
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            uint32_t a = i * (n + 1) + j;
            uint32_t b = a + n + 1;
            // Outward (local +Z) winding; see the sphere above for the order.
            g.indices.push_back(a); g.indices.push_back(a + 1); g.indices.push_back(b);
            g.indices.push_back(b); g.indices.push_back(a + 1); g.indices.push_back(b + 1);
        }
    }
    return g;
}

Mesh Mesh::makeCube(float halfSize, const char *name)
{
    return Mesh(makeCubeGeometry(halfSize), name);
}
Mesh Mesh::makePlane(float halfExtent, int subdivisions, const char *name)
{
    return Mesh(makePlaneGeometry(halfExtent, subdivisions), name);
}
Mesh Mesh::makeSphere(float radius, const char *name)
{
    return Mesh(makeSphereGeometry(radius), name);
}

} // namespace horror
