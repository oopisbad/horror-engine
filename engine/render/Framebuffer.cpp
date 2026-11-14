#include "render/Framebuffer.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <stdexcept>

#include "core/Log.hpp"

namespace horror {

Framebuffer::Framebuffer(Kind kind, uint32_t width, uint32_t height, bool hdr)
    : kind_(kind)
    , hdr_(hdr)
    , width_(width)
    , height_(height)
{
    HOR_CHECK(width > 0 && height > 0);
    create();
}

Framebuffer::~Framebuffer()
{
    destroy();
}

Framebuffer::Framebuffer(Framebuffer &&o) noexcept
    : kind_(o.kind_)
    , hdr_(o.hdr_)
    , width_(o.width_)
    , height_(o.height_)
    , fbo_(o.fbo_)
    , colorTextures_(std::move(o.colorTextures_))
    , depthTexture_(o.depthTexture_)
{
    o.fbo_ = 0;
    o.colorTextures_.clear();
    o.depthTexture_ = 0;
}

Framebuffer &Framebuffer::operator=(Framebuffer &&o) noexcept
{
    if (this == &o)
        return *this;
    destroy();
    kind_ = o.kind_;
    hdr_ = o.hdr_;
    width_ = o.width_;
    height_ = o.height_;
    fbo_ = o.fbo_;
    colorTextures_ = std::move(o.colorTextures_);
    depthTexture_ = o.depthTexture_;
    o.fbo_ = 0;
    o.colorTextures_.clear();
    o.depthTexture_ = 0;
    return *this;
}

void Framebuffer::destroy()
{
    if (fbo_)
        glDeleteFramebuffers(1, &fbo_);
    if (!colorTextures_.empty())
        glDeleteTextures(static_cast<GLsizei>(colorTextures_.size()), colorTextures_.data());
    if (depthTexture_)
        glDeleteTextures(1, &depthTexture_);
    fbo_ = 0;
    colorTextures_.clear();
    depthTexture_ = 0;
}

void Framebuffer::create()
{
    glCreateFramebuffers(1, &fbo_);

    if (kind_ != Kind::DepthOnly) {
        GLuint color = 0;
        glCreateTextures(GL_TEXTURE_2D, 1, &color);
        // The scene and bloom need nonnegative HDR RGB. Only volume needs alpha.
        const GLenum format = !hdr_ ? GL_RGBA8
            : (kind_ == Kind::ColorAlpha ? GL_RGBA16F : GL_R11F_G11F_B10F);
        glTextureStorage2D(color, 1, format,
            static_cast<GLsizei>(width_), static_cast<GLsizei>(height_));
        glTextureParameteri(color, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTextureParameteri(color, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(color, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(color, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        colorTextures_.push_back(color);
        glNamedFramebufferTexture(fbo_, GL_COLOR_ATTACHMENT0, color, 0);
        glNamedFramebufferDrawBuffer(fbo_, GL_COLOR_ATTACHMENT0);
        glNamedFramebufferReadBuffer(fbo_, GL_COLOR_ATTACHMENT0);
    }

    if (kind_ == Kind::Surface) {
        GLuint surface = 0;
        glCreateTextures(GL_TEXTURE_2D, 1, &surface);
        glTextureStorage2D(surface, 1, GL_RGBA8, width_, height_);
        glTextureParameteri(surface, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTextureParameteri(surface, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTextureParameteri(surface, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(surface, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        colorTextures_.push_back(surface);
        glNamedFramebufferTexture(fbo_, GL_COLOR_ATTACHMENT1, surface, 0);
        constexpr GLenum attachments[] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
        glNamedFramebufferDrawBuffers(fbo_, 2, attachments);
    }

    if (kind_ == Kind::DepthOnly || kind_ == Kind::ColorDepth || kind_ == Kind::Surface) {
        // DEPTH_COMPONENT24 for the main frame; stencil left off for now.
        glCreateTextures(GL_TEXTURE_2D, 1, &depthTexture_);
        glTextureStorage2D(depthTexture_, 1, GL_DEPTH_COMPONENT24,
            static_cast<GLsizei>(width_), static_cast<GLsizei>(height_));
        glTextureParameteri(depthTexture_, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTextureParameteri(depthTexture_, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(depthTexture_, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(depthTexture_, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glNamedFramebufferTexture(fbo_, GL_DEPTH_ATTACHMENT, depthTexture_, 0);
    }

    if (kind_ == Kind::ColorDepth || kind_ == Kind::Surface) {
        glTextureParameteri(depthTexture_, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTextureParameteri(depthTexture_, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    if (kind_ == Kind::DepthOnly) {
        // A depth-only FBO must explicitly disable color draw/read buffers;
        // otherwise the inherited default (COLOR_ATTACHMENT0, which has no
        // image) makes the framebuffer incomplete and the shadow pass writes
        // into the void.
        glNamedFramebufferDrawBuffer(fbo_, GL_NONE);
        glNamedFramebufferReadBuffer(fbo_, GL_NONE);
    }

    const GLenum status = glCheckNamedFramebufferStatus(fbo_, GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        HOR_LOG_ERROR("framebuffer incomplete (status 0x{:x}, kind {}, {}x{})",
            static_cast<unsigned>(status), static_cast<int>(kind_), width_, height_);
        destroy();
        throw std::runtime_error("OpenGL framebuffer is incomplete");
    }
}

void Framebuffer::resize(uint32_t width, uint32_t height)
{
    // Guard against degenerate sizes (minimized window) before reallocating.
    if (width_ == width && height_ == height)
        return;
    if (width == 0 || height == 0)
        return;
    HOR_CHECK(width > 0 && height > 0);
    width_ = width;
    height_ = height;
    destroy();
    create();
    HOR_LOG_DEBUG("fb resized to {}x{}", width, height);
}

void Framebuffer::bind() const
{
    HOR_CHECK(fbo_ != 0);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glViewport(0, 0, static_cast<GLint>(width_), static_cast<GLint>(height_));
}

void Framebuffer::setClearColor(const glm::vec4 &rgba) const
{
    HOR_CHECK(fbo_ != 0);
    glClearColor(rgba.r, rgba.g, rgba.b, rgba.a);
}

GLuint Framebuffer::colorAttachment(uint32_t index) const
{
    HOR_CHECK(index < colorTextures_.size());
    return colorTextures_[index];
}

} // namespace horror
