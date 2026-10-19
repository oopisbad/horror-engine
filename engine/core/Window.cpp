#include "core/Window.hpp"

#include "core/Log.hpp"

#include <stdexcept>

namespace horror {

void Window::WindowDeleter::operator()(GLFWwindow *w) const
{
    if (w)
        glfwDestroyWindow(w);
}

void Window::glfwErrorCallback(int error, const char *message)
{
    HOR_LOG_ERROR("GLFW error {}: {}", error, message ? message : "(no message)");
}

Window::Window(Desc desc)
{
    HOR_ASSERT(desc.width > 0 && desc.height > 0);

    glfwSetErrorCallback(glfwErrorCallback);
    if (!glfwInit()) {
        HOR_LOG_FATAL("glfwInit() failed");
        throw std::runtime_error("glfwInit() failed");
    }

    if (desc.fullscreen) {
        GLFWmonitor *monitor = glfwGetPrimaryMonitor();
        const GLFWvidmode *mode = glfwGetVideoMode(monitor);
        HOR_ASSERT(mode);
        desc.width = static_cast<uint32_t>(mode->width);
        desc.height = static_cast<uint32_t>(mode->height);
        glfwWindowHint(GLFW_RED_BITS, mode->redBits);
        glfwWindowHint(GLFW_GREEN_BITS, mode->greenBits);
        glfwWindowHint(GLFW_BLUE_BITS, mode->blueBits);
        glfwWindowHint(GLFW_REFRESH_RATE, mode->refreshRate);
        glfwWindowHint(GLFW_AUTO_ICONIFY, GLFW_FALSE);
    }

    // Core-profile context; rejecting a compat context on purpose: core 4.5
    // behavior differs (e.g., no default VAO bound) and the engine assumes it.
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, desc.glMajor);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, desc.glMinor);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif
    // sRGB is deliberately NOT requested; tonemapping is a renderer decision.
    glfwWindowHint(GLFW_SRGB_CAPABLE, GLFW_TRUE);

    GLFWmonitor *screen = desc.fullscreen ? glfwGetPrimaryMonitor() : nullptr;
    window_.reset(glfwCreateWindow(static_cast<int>(desc.width), static_cast<int>(desc.height),
        desc.title.data(), screen, nullptr));
    if (!window_) {
        HOR_LOG_FATAL("OpenGL {}.{} core context creation failed (driver too old?)",
            desc.glMajor, desc.glMinor);
        throw std::runtime_error("window creation failed");
    }

    makeCurrent();
    // Load GL function pointers once, for the whole process.
    int version = gladLoadGL(reinterpret_cast<GLADloadfunc>(glfwGetProcAddress));
    if (version == 0) {
        HOR_LOG_FATAL("gladLoadGL failed to initialize OpenGL loader");
        throw std::runtime_error("gladLoadGL failed");
    }
    HOR_CHECK(GLAD_GL_VERSION_4_5);
    HOR_LOG_INFO("GL context version ({}) on {}",
        reinterpret_cast<const char *>(glGetString(GL_VERSION)),
        reinterpret_cast<const char *>(glGetString(GL_RENDERER)));
    glfwSwapInterval(desc.vsync ? 1 : 0);
    vsync_ = desc.vsync;
    // Default minimum size: resizing to 0x0 would break the projection math.
    glfwSetWindowSizeLimits(window_.get(), 320, 240, GLFW_DONT_CARE, GLFW_DONT_CARE);

    // GLFW reports fb size separately from logical size (HiDPI); track both.
    int fw = 0, fh = 0, ww = 0, wh = 0;
    glfwGetFramebufferSize(window_.get(), &fw, &fh);
    glfwGetWindowSize(window_.get(), &ww, &wh);
    width_ = static_cast<uint32_t>(ww);
    height_ = static_cast<uint32_t>(wh);
    resizedSinceLastFrame = false;

    // Single-window engine: stash `this` in the user pointer for callbacks.
    // Swap to an id->Window* registry when a second window is ever needed.
    glfwSetWindowUserPointer(window_.get(), this);
    glfwSetFramebufferSizeCallback(window_.get(), [](GLFWwindow *w, int bw, int bh) {
        auto self = static_cast<Window *>(glfwGetWindowUserPointer(w));
        if (!self)
            return;
        glViewport(0, 0, bw, bh);
        self->resizedSinceLastFrame = true;
        // Physical and logical sizes differ on HiDPI; the camera aspect uses
        // the logical size, which GLFW does not push to this callback.
        int ww = 0, wh = 0;
        glfwGetWindowSize(w, &ww, &wh);
        if (ww > 0 && wh > 0) {
            self->width_ = static_cast<uint32_t>(ww);
            self->height_ = static_cast<uint32_t>(wh);
        }
        if (self->onFramebufferResized)
            self->onFramebufferResized(static_cast<uint32_t>(bw), static_cast<uint32_t>(bh));
    });
}

Window::~Window()
{
    window_.reset(); // Destroy GL objects in a valid context first (RAII order).
    glfwTerminate();
}

void Window::makeCurrent() const
{
    glfwMakeContextCurrent(window_.get());
}

void Window::swapBuffers() const
{
    glfwSwapBuffers(window_.get());
}

void Window::pollEvents() const
{
    glfwPollEvents();
    // Cleared after every poll: the flag describes "the last polled batch".
    const_cast<Window *>(this)->resizedSinceLastFrame = false;
}

void Window::setVsync(bool enabled)
{
    if (enabled == vsync_)
        return;
    makeCurrent(); // SwapInterval is context-global, cheap to re-assert.
    glfwSwapInterval(enabled ? 1 : 0);
    vsync_ = enabled;
    HOR_LOG_DEBUG("vsync {}", enabled ? "on" : "off");
}

bool Window::shouldClose() const
{
    return glfwWindowShouldClose(window_.get()) != 0;
}

uint32_t Window::frameWidth() const
{
    int w = 0, h = 0;
    glfwGetFramebufferSize(window_.get(), &w, &h);
    return static_cast<uint32_t>(w);
}

uint32_t Window::frameHeight() const
{
    int w = 0, h = 0;
    glfwGetFramebufferSize(window_.get(), &w, &h);
    return static_cast<uint32_t>(h);
}

uint32_t Window::width() const { return width_; }
uint32_t Window::height() const { return height_; }

double Window::aspect() const
{
    return double(width_) / double(height_);
}

} // namespace horror
