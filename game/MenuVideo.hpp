#pragma once
#include <filesystem>
#include <cstdint>
namespace horror {
// Decoder adapter boundary. Implement this when the background video is supplied.
// Upload frames on the render thread; return an OpenGL texture owned by adapter.
// Frontend keeps a black fallback until open succeeds and a frame is available.
class MenuVideo {
public:
    virtual ~MenuVideo() = default;
    virtual bool open(const std::filesystem::path &file) = 0;
    virtual void update(float seconds) = 0;
    virtual uint32_t texture() const = 0;
    virtual float aspect() const = 0;
};
}
