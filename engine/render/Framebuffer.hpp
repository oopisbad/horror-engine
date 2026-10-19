#pragma once

#include <glad/gl.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace horror {

// Owned GL framebuffer with color(+depth) attachments. Built for the
// offscreen targets a future post-processing chain needs: render the scene
// into this, then blit/up-scale to the swapchain. The shadow map uses a
// depth-only variant, and the post-pass stub wires a full-screen draw.
//
// Anything non-texture-planned doesn't belong here; instead an explicit
// `postProcess` entry point lives on the renderer.
class Framebuffer {
public:
    enum class Kind : uint8_t {
        ColorDepth,   // RGBA8 + depth: offscreen scene target / post scratch
        DepthOnly,    // depth attachment only: shadow maps
    };

    // `hdr` selects an RGBA16F color attachment (scene target for the post
    // chain, where lighting may exceed 1.0 before tonemapping).
    explicit Framebuffer(Kind kind, uint32_t width, uint32_t height, bool hdr = false);
    ~Framebuffer();
    Framebuffer(Framebuffer &&) noexcept;
    Framebuffer &operator=(Framebuffer &&) noexcept;
    Framebuffer(const Framebuffer &) = delete;
    Framebuffer &operator=(const Framebuffer &) = delete;

    void bind() const;
    static void bindDefault() { glBindFramebuffer(GL_FRAMEBUFFER, 0); }

    void resize(uint32_t width, uint32_t height);
    bool valid() const { return fbo_ != 0; }

    GLuint fbo() const { return fbo_; }
    GLuint colorAttachment(uint32_t index = 0) const;
    GLuint depth() const { return depthTexture_; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    bool hdr() const { return hdr_; }

    // Clear color for the target once per frame from the renderer (explicit,
    // rather than per-frame assumptions scattered around unrelated code).
    void setClearColor(const glm::vec4 &rgba) const;

    bool isDepthOnly() const { return kind_ == Kind::DepthOnly; }

    static constexpr int kMaxColorAttachments = 8; // MRT-ready for deferred

private:
    Kind kind_;
    bool hdr_ = false;
    uint32_t width_ = 0, height_ = 0;
    GLuint fbo_ = 0;
    std::vector<GLuint> colorTextures_;
    GLuint depthTexture_ = 0;

    void create();
    void destroy();
};

} // namespace horror
