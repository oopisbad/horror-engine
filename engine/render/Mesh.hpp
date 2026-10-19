#pragma once

#include <glad/gl.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace horror {

// Interleaved static vertex: position, normal, uv. Kept as a plain struct so
// loaders (procedural builders today, .gltf/.obj tomorrow) can hand raw
// buffers straight to the VBO.
struct Vertex {
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    glm::vec2 uv{0.0f};
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
    GLsizei indexCount() const { return static_cast<GLsizei>(indices_.size()); }
    const std::string &name() const { return name_; }

    // Exact local-space bounds; used by the collider debugger & culling.
    glm::vec3 minBounds() const { return minBounds_; }
    glm::vec3 maxBounds() const { return maxBounds_; }

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
    std::vector<uint32_t> indices_;
    std::string name_ = "mesh";
    GLuint vao_ = 0;
    GLuint vbo_ = 0;
    GLuint ebo_ = 0;
    glm::vec3 minBounds_{0.0f};
    glm::vec3 maxBounds_{0.0f};

    void upload();
    void computeBounds();
};

} // namespace horror
