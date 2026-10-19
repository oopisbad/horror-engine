#pragma once

#include <glad/gl.h>

#include <cstdint>
#include <filesystem>
#include <string>

namespace horror {

// Simple 2D texture wrapper (sampler state included). stb_image loads the
// file when one exists; otherwise a magenta 1x1 placeholder is created so a
// missing file is visible in-scene instead of silently showing black.
class Texture {
public:
    struct Params {
        GLint minFilter = GL_LINEAR_MIPMAP_LINEAR;
        GLint magFilter = GL_LINEAR;
        GLint wrapS = GL_REPEAT;
        GLint wrapT = GL_REPEAT;
        bool srgb = true; // color textures default to sRGB decode
    };

    Texture() = default;
    // Loads from disk; falls back to an error placeholder when missing.
    explicit Texture(const std::string &path,
        const Params &params); // no default arg: GCC can't see nested PMIs yet
    // Decodes an encoded image from memory (glTF embedded buffer views,
    // data URIs). Same fallback behavior as the file constructor.
    Texture(const uint8_t *encoded, size_t size, const Params &params);
    // Manual upload (procedural data, fallbacks, middleware textures).
    Texture(GLenum internalFormat, uint32_t width, uint32_t height, uint32_t depth,
        GLenum format, GLenum type, const void *data, const Params &params);
    ~Texture();

    Texture(Texture &&) noexcept;
    Texture &operator=(Texture &&) noexcept;
    Texture(const Texture &) = delete;
    Texture &operator=(const Texture &) = delete;

    void bind(GLuint unit = 0) const;

    GLuint handle() const { return texture_; }
    int width() const { return static_cast<int>(width_); }
    int height() const { return static_cast<int>(height_); }
    bool isSrgb() const { return srgb_; }

    // Bind to a sampler uniform unit; defaults to unit 0 (see material binds).
    // Texture units are tracked explicitly in the renderer instead.

    // Placeholder texture shared by everything that fails to load.
    static const Texture &placeholder();

private:
    void applyParams(const Params &params);

    GLuint texture_ = 0;
    uint32_t width_ = 0, height_ = 0, depth_ = 0;
    bool srgb_ = false;
};

} // namespace horror
