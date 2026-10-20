#include "render/Texture.hpp"

#include "core/Log.hpp"

#include <algorithm>
#include <cmath>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ASSERT(x) HOR_ASSERT(x)
#include <stb_image.h>

namespace horror {

Texture::Texture(const std::string &path, const Params &params)
{
    int w = 0, h = 0, channels = 0;
    stbi_uc *pixels = stbi_load(path.c_str(), &w, &h, &channels, 4); // force RGBA for simplicity
    if (!pixels) {
        HOR_LOG_WARN_ONCE("texture load failed: {} -- using placeholder", stbi_failure_reason());
        *this = Texture(GL_RGBA8, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, nullptr, params);
        const uint8_t magenta[4] = { 255, 0, 255, 255 };
        glTextureSubImage2D(texture_, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, magenta);
        width_ = height_ = 1;
        srgb_ = false;
        return;
    }
    *this = Texture(GL_RGBA8, static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1,
        GL_RGBA, GL_UNSIGNED_BYTE, pixels, params);
    stbi_image_free(pixels);
    srgb_ = params.srgb;
}

Texture::Texture(const uint8_t *encoded, size_t size, const Params &params)
{
    int w = 0, h = 0, channels = 0;
    if (!encoded || size == 0) {
        HOR_LOG_WARN_ONCE("embedded texture has no data -- using placeholder");
        *this = Texture(GL_RGBA8, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, nullptr, params);
        const uint8_t magenta[4] = { 255, 0, 255, 255 };
        glTextureSubImage2D(texture_, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, magenta);
        width_ = height_ = 1;
        srgb_ = false;
        return;
    }
    stbi_uc *pixels = stbi_load_from_memory(encoded, static_cast<int>(size), &w, &h, &channels, 4);
    if (!pixels) {
        HOR_LOG_WARN_ONCE("embedded texture decode failed: {} -- using placeholder",
            stbi_failure_reason());
        *this = Texture(GL_RGBA8, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, nullptr, params);
        const uint8_t magenta[4] = { 255, 0, 255, 255 };
        glTextureSubImage2D(texture_, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, magenta);
        width_ = height_ = 1;
        srgb_ = false;
        return;
    }
    *this = Texture(GL_RGBA8, static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1,
        GL_RGBA, GL_UNSIGNED_BYTE, pixels, params);
    stbi_image_free(pixels);
    srgb_ = params.srgb;
}

Texture::Texture(GLenum internalFormat, uint32_t width, uint32_t height, uint32_t depth,
    GLenum format, GLenum type, const void *data, const Params &params)
{
    HOR_CHECK(width > 0 && height > 0 && depth > 0);
    width_ = width;
    height_ = height;
    depth_ = depth;
    srgb_ = params.srgb;

    glCreateTextures(GL_TEXTURE_2D, 1, &texture_);
    // Allocate a full mip chain so mipmapped min-filters address valid data.
    GLsizei levels = 1;
    if (params.minFilter != GL_LINEAR && params.minFilter != GL_NEAREST && internalFormat != GL_DEPTH_COMPONENT24)
        levels = static_cast<GLsizei>(std::floor(std::log2(std::max(width, height)))) + 1;
    glTextureStorage2D(texture_, levels, internalFormat, static_cast<GLsizei>(width),
        static_cast<GLsizei>(height));
    if (data)
        glTextureSubImage2D(texture_, 0, 0, 0, static_cast<GLsizei>(width),
            static_cast<GLsizei>(height), format, type, data);
    applyParams(params);
    if (levels > 1)
        glGenerateTextureMipmap(texture_);
}

Texture::~Texture()
{
    if (texture_)
        glDeleteTextures(1, &texture_);
}

Texture::Texture(Texture &&o) noexcept
    : texture_(o.texture_), width_(o.width_), height_(o.height_), depth_(o.depth_), srgb_(o.srgb_)
{
    o.texture_ = 0;
}

Texture &Texture::operator=(Texture &&o) noexcept
{
    if (this == &o)
        return *this;
    if (texture_)
        glDeleteTextures(1, &texture_);
    texture_ = o.texture_;
    width_ = o.width_;
    height_ = o.height_;
    depth_ = o.depth_;
    srgb_ = o.srgb_;
    o.texture_ = 0;
    return *this;
}

void Texture::applyParams(const Params &params)
{
    glTextureParameteri(texture_, GL_TEXTURE_MIN_FILTER, params.minFilter);
    glTextureParameteri(texture_, GL_TEXTURE_MAG_FILTER, params.magFilter);
    glTextureParameteri(texture_, GL_TEXTURE_WRAP_S, params.wrapS);
    glTextureParameteri(texture_, GL_TEXTURE_WRAP_T, params.wrapT);
}

void Texture::bind(GLuint unit) const
{
    HOR_CHECK(texture_ != 0);
    glBindTextureUnit(unit, texture_);
}

} // namespace horror
