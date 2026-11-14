#pragma once

#include <glad/gl.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace horror {

// Owned framebuffer with texture attachments for the scene, shadows or post effects.
class Framebuffer {
public:
    enum class Kind : uint8_t {
        Surface,      // HDR scene color, packed surface data and depth
        ColorDepth,   // color and depth: offscreen scene target
        ColorAlpha,   // RGBA16F post buffer without a depth attachment
        ColorOnly,    // compact floating-point RGB for post filters
        DepthOnly,    // depth attachment only: shadow maps
    };

    // `hdr` uses packed floating-point RGB, or RGBA16F for ColorAlpha.
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
