#include "render/DebugDraw.hpp"

#include "core/Log.hpp"
#include "render/Shader.hpp"

#include <glm/gtc/constants.hpp>

#include <cmath>
#include <cstring>

namespace horror {

namespace {
constexpr float kSqrt2Over2 = 0.70710678f;
} // namespace

DebugDraw::DebugDraw() = default;

DebugDraw::~DebugDraw()
{
    if (vbo_)
        glDeleteBuffers(1, &vbo_);
    if (vao_)
        glDeleteVertexArrays(1, &vao_);
}

void DebugDraw::ensureGpu()
{
    if (gpuReady_)
        return;
    shader_ = std::make_unique<Shader>("debug.vert", "debug.frag", "debug");
    glCreateVertexArrays(1, &vao_);
    glCreateBuffers(1, &vbo_);
    glNamedBufferStorage(vbo_, sizeof(float) * 6 * 4096, nullptr, GL_MAP_WRITE_BIT);
    // attribute 0: position vec3, attribute 1: color vec3, stride 6 floats.
    glEnableVertexArrayAttrib(vao_, 0);
    glVertexArrayAttribFormat(vao_, 0, 3, GL_FLOAT, GL_FALSE, 0);
    glVertexArrayAttribBinding(vao_, 0, 0);
    glEnableVertexArrayAttrib(vao_, 1);
    glVertexArrayAttribFormat(vao_, 1, 3, GL_FLOAT, GL_FALSE, sizeof(float) * 3);
    glVertexArrayAttribBinding(vao_, 1, 0);
    glVertexArrayVertexBuffer(vao_, 0, vbo_, 0, sizeof(float) * 6);
    gpuReady_ = true;
}

void DebugDraw::drawSegment(const glm::vec3 &a, const glm::vec3 &b, const glm::vec3 &color)
{
    vertices_.push_back(a.x); vertices_.push_back(a.y); vertices_.push_back(a.z);
    vertices_.push_back(color.r); vertices_.push_back(color.g); vertices_.push_back(color.b);
    vertices_.push_back(b.x); vertices_.push_back(b.y); vertices_.push_back(b.z);
    vertices_.push_back(color.r); vertices_.push_back(color.g); vertices_.push_back(color.b);
}

void DebugDraw::addLine(const glm::vec3 &a, const glm::vec3 &b, const glm::vec3 &color)
{
    drawSegment(a, b, color);
}

void DebugDraw::addBox(const glm::vec3 &mn, const glm::vec3 &mx, const glm::vec3 &color)
{
    addBoxTransformed(glm::mat4(1.0f), mn, mx, color);
}

void DebugDraw::addBoxTransformed(const glm::mat4 &model, const glm::vec3 &lmin,
    const glm::vec3 &lmax, const glm::vec3 &color)
{
    const glm::vec3 corners[8] = {
        { lmin.x, lmin.y, lmin.z }, { lmax.x, lmin.y, lmin.z },
        { lmax.x, lmin.y, lmax.z }, { lmin.x, lmin.y, lmax.z },
        { lmin.x, lmax.y, lmin.z }, { lmax.x, lmax.y, lmin.z },
        { lmax.x, lmax.y, lmax.z }, { lmin.x, lmax.y, lmax.z },
    };
    static constexpr int edges[12][2] = {
        { 0, 1 }, { 1, 2 }, { 2, 3 }, { 3, 0 }, // bottom ring
        { 4, 5 }, { 5, 6 }, { 6, 7 }, { 7, 4 }, // top ring
        { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 }, // pillars
    };
    for (const auto &e : edges) {
        glm::vec3 a = glm::vec3(model * glm::vec4(corners[e[0]], 1.0f));
        glm::vec3 b = glm::vec3(model * glm::vec4(corners[e[1]], 1.0f));
        drawSegment(a, b, color);
    }
}

void DebugDraw::addSphere(const glm::vec3 &center, float radius, const glm::vec3 &color, int segments)
{
    // Three axis-aligned great circles: cheap, readable from any angle.
    for (int plane = 0; plane < 3; ++plane) {
        glm::vec3 prev;
        for (int i = 0; i <= segments; ++i) {
            float t = float(i) / float(segments) * glm::two_pi<float>();
            glm::vec3 offset;
            switch (plane) {
            case 0: offset = glm::vec3(std::cos(t), std::sin(t), 0.0f); break; // XY
            case 1: offset = glm::vec3(0.0f, std::cos(t), std::sin(t)); break; // YZ
            default: offset = glm::vec3(std::cos(t), 0.0f, std::sin(t)); break; // ZX
            }
            glm::vec3 p = center + radius * offset;
            if (i > 0)
                drawSegment(prev, p, color);
            prev = p;
        }
    }
}

void DebugDraw::addAxes(const glm::vec3 &origin, float length)
{
    addLine(origin, origin + glm::vec3(length, 0, 0), glm::vec3(1, 0.2f, 0.2f));
    addLine(origin, origin + glm::vec3(0, length, 0), glm::vec3(0.2f, 1, 0.2f));
    addLine(origin, origin + glm::vec3(0, 0, length), glm::vec3(0.2f, 0.2f, 1));
}

void DebugDraw::addFrustum(const glm::mat4 &viewProjection, const glm::vec3 &color)
{
    // Invert the view-projection to recover the 8 world-space corners.
    glm::mat4 invVP = glm::inverse(viewProjection);
    const glm::vec4 ndc[8] = {
        { -1, -1, -1, 1 }, { +1, -1, -1, 1 }, { +1, +1, -1, 1 }, { -1, +1, -1, 1 },
        { -1, -1, +1, 1 }, { +1, -1, +1, 1 }, { +1, +1, +1, 1 }, { -1, +1, +1, 1 },
    };
    glm::vec3 world[8];
    for (int i = 0; i < 8; ++i) {
        glm::vec4 v = invVP * ndc[i];
        world[i] = glm::vec3(v) / v.w;
    }
    static constexpr int edges[12][2] = {
        { 0, 1 }, { 1, 2 }, { 2, 3 }, { 3, 0 },
        { 4, 5 }, { 5, 6 }, { 6, 7 }, { 7, 4 },
        { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 },
    };
    for (const auto &e : edges)
        drawSegment(world[e[0]], world[e[1]], color);
}

void DebugDraw::flush(const glm::mat4 &viewProjection)
{
    if (vertices_.empty())
        return;
    ensureGpu();

    const size_t byteSize = vertices_.size() * sizeof(float);
    GLint currentSize = 0;
    glGetNamedBufferParameteriv(vbo_, GL_BUFFER_SIZE, &currentSize);
    if (byteSize > static_cast<size_t>(currentSize)) {
        // Outgrew storage: recreate with 2x headroom (buffer storage is
        // immutable, so growth means recreate -- happens at most a few times).
        glDeleteBuffers(1, &vbo_);
        glCreateBuffers(1, &vbo_);
        glNamedBufferStorage(vbo_, byteSize * 2, nullptr, GL_MAP_WRITE_BIT);
        glVertexArrayVertexBuffer(vao_, 0, vbo_, 0, sizeof(float) * 6);
    }

    // Map + memcpy + unmap: cheapest path for a per-frame rewrite.
    void *ptr = glMapNamedBufferRange(vbo_, 0, static_cast<GLsizeiptr>(byteSize), GL_MAP_WRITE_BIT);
    HOR_CHECK(ptr != nullptr);
    std::memcpy(ptr, vertices_.data(), byteSize);
    glUnmapNamedBuffer(vbo_);

    shader_->bind();
    shader_->setUniform("uViewProj", viewProjection);
    glBindVertexArray(vao_);
    glLineWidth(1.5f);
    glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(vertices_.size() / 6));
    glBindVertexArray(0);
    Shader::unbind();

    vertices_.clear();
}

} // namespace horror
