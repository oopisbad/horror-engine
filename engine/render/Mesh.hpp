#pragma once

#include <glad/gl.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace horror {

// Interleaved vertex: position, normal, uv0, tangent, joints, weights.
// One format for everything so static and skinned meshes share the pipeline:
// static geometry leaves joints/weights zero and the static shader variant
// simply ignores them. `tangent.w` is the bitangent handedness (+/-1).
// Attribute locations: 0 pos, 1 normal, 2 uv, 3 tangent, 4 joints, 5 weights.
// Joints are uvec4 on purpose (integer attribute, no float conversion).
struct Vertex {
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    glm::vec2 uv{0.0f};
    glm::vec4 tangent{1.0f, 0.0f, 0.0f, 1.0f};
    glm::uvec4 joints{0u};
    glm::vec4 weights{0.0f};
    glm::vec2 lightmapUv{0.0f};
};

// A named GPU mesh: interleaved VBO + element indices in one VAO.
// Immutable after upload; the geometry builders return fresh instances so a
// mesh is cheaply shareable by value inside render items.
class Mesh {
public:
    struct Geometry {
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
    };

    Mesh() = default;
    // Name is for debugging only (gizmos, logs, renderdoc labels).
    explicit Mesh(Geometry geometry, std::string name = "mesh");
    ~Mesh();

    Mesh(Mesh &&) noexcept;
    Mesh &operator=(Mesh &&) noexcept;
    Mesh(const Mesh &) = delete;
    Mesh &operator=(const Mesh &) = delete;

    bool valid() const { return vao_ != 0; }
    GLuint handle() const { return vao_; }
    GLsizei indexCount() const { return indexCount_; }
    GLenum indexType() const { return indexType_; }
    const std::string &name() const { return name_; }

    // Exact local-space bounds; used by the collider debugger & culling.
    glm::vec3 minBounds() const { return minBounds_; }
    glm::vec3 maxBounds() const { return maxBounds_; }

    // CPU copy of the geometry, kept for the skinning probe (--debug-skin) and
    // for any future CPU-side queries. Read-only; meshes are immutable.
    const std::vector<Vertex> &vertices() const { return vertices_; }

    void bind() const;

    // --- procedural builders -----------------------------------------------
    // Centered on the origin; all axis-aligned.
    static Geometry makeCubeGeometry(float halfSize);
    static Geometry makeSphereGeometry(float radius, int lon = 16, int lat = 8);
    static Geometry makePlaneGeometry(float halfExtent, int subdivisions = 1);

    // Convenience: validated cube/plane mesh instances.
    static Mesh makeCube(float halfSize, const char *name = "cube");
    static Mesh makePlane(float halfExtent, int subdivisions = 1, const char *name = "plane");
    static Mesh makeSphere(float radius, const char *name = "sphere");

private:
    std::vector<Vertex> vertices_;
    GLsizei indexCount_ = 0;
    GLenum indexType_ = GL_UNSIGNED_INT;
    std::string name_ = "mesh";
    GLuint vao_ = 0;
    GLuint vbo_ = 0;
    GLuint ebo_ = 0;
    glm::vec3 minBounds_{0.0f};
    glm::vec3 maxBounds_{0.0f};

    void upload(const std::vector<uint32_t> &indices);
    void computeBounds();
};

} // namespace horror
