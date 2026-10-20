#pragma once

#include <glad/gl.h>

#include <glm/glm.hpp>

#include <memory>
#include <vector>

namespace horror {

// Immediate-mode line renderer for debugging: colliders, light positions,
// frustums. Everything batches into one VBO per frame, drawn last with
// depth-test on so lines respect geometry (toggle is in Renderer).
//
// Kept separate from the renderer's mesh path so shipping builds can strip
// it entirely (or make it a no-op) without touching game code.
class DebugDraw {
public:
    DebugDraw();
    ~DebugDraw();
    DebugDraw(DebugDraw &&) = delete;
    DebugDraw &operator=(DebugDraw &&) = delete;

    void addLine(const glm::vec3 &a, const glm::vec3 &b, const glm::vec3 &color);
    // Axis-aligned world-space box outline.
    void addBox(const glm::vec3 &worldMin, const glm::vec3 &worldMax, const glm::vec3 &color);
    // Box outline through an arbitrary model matrix (local min/max corners).
    void addBoxTransformed(const glm::mat4 &model, const glm::vec3 &localMin,
        const glm::vec3 &localMax, const glm::vec3 &color);
    void addSphere(const glm::vec3 &center, float radius, const glm::vec3 &color, int segments = 12);
    void addAxes(const glm::vec3 &origin, float length);
    void addFrustum(const glm::mat4 &viewProjection, const glm::vec3 &color);

    // Overlay (x-ray) mode: primitives added while this is on are drawn after
    // the depth-tested batch with depth testing disabled, so they are visible
    // *through* geometry. The skeleton uses this -- a depth-tested skeleton is
    // 90% hidden inside the body it deforms, which reads as "the bones are not
    // attached" when in fact they are.
    void setOverlayMode(bool on) { overlay_ = on; }
    bool overlayMode() const { return overlay_; }

    // Draws everything accumulated this frame, then clears the batch.
    void flush(const glm::mat4 &viewProjection);

    bool empty() const { return vertices_.empty(); }

private:
    void ensureGpu();
    void drawSegment(const glm::vec3 &a, const glm::vec3 &b, const glm::vec3 &color);
    void drawBatch(std::vector<float> &vertices, const glm::mat4 &viewProjection,
        bool depthTest);

    // Interleaved line vertices: pos(3) + color(3).
    std::vector<float> vertices_;
    std::vector<float> overlayVertices_;
    bool overlay_ = false;
    GLuint vao_ = 0;
    GLuint vbo_ = 0;
    std::unique_ptr<class Shader> shader_; // dtor in cpp where Shader is complete
    bool gpuReady_ = false;
};

} // namespace horror
