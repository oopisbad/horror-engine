#include "core/Application.hpp"

#include "anim/AnimationSystem.hpp"
#include "debug/ImGuiOverlay.hpp"
#include "render/Framebuffer.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace horror {

Application::Application(Config config)
    : config_(config)
    , window_(Window::Desc{
          config_.windowTitle, config_.windowWidth, config_.windowHeight,
          4, 5, config_.vsync, config_.fullscreen })
    , renderer_(config_.shadowMapSize)
    , imgui_(std::make_unique<ImGuiOverlay>())
    , logic_(config_.fixedStepSeconds)
{
    input_.attach(window_);
    // Size the offscreen scene target now and follow every resize; the renderer
    // cannot query the window itself (kept decoupled).
    renderer_.setOutputSize(window_.frameWidth(), window_.frameHeight());
    window_.onFramebufferResized = [this](uint32_t w, uint32_t h) {
        renderer_.setOutputSize(w, h);
    };
    imgui_->init(*this);
    // FPS games start looking around immediately; overlay opens with F1.
    input_.setMouseCaptured(!showImGui);
}

Application::~Application()
{
    imgui_->shutdown();
}

int Application::run()
{
    while (running_ && !window_.shouldClose()) {
        const float frameDt = frameTimer_.tickRealtime();
        elapsed_ += frameDt;
        window_.pollEvents();
        input_.update();

        handleHotkeys();

        // The overlay claims the mouse while it's open; mouse-capture flips
        // with it so FPS mouselook never fights a visible UI cursor.
        input_.mouseWantedByImGui = imgui_->wantsMouse();
        input_.keyboardWantedByImGui = imgui_->wantsTextInput() && !input_.mouseCaptured();
        imgui_->frame(frameDt, *this);
        if (showImGui)
            imgui_->drawDebugWindows(*this);

        const int steps = logic_.stepsToRun(frameDt);
        for (int i = 0; i < steps; ++i)
            if (onFixedStep)
                onFixedStep(logic_.fixedStepSeconds());
        if (onFrameUpdate)
            onFrameUpdate(frameDt);

        // Animation runs at the variable frame rate (not the fixed step) so
        // playback stays smooth; gameplay decides what to play per frame.
        AnimationSystem::update(scene_, frameDt);

        scene_.updateWorldMatrices();
        scene_.refreshColliderBounds();
        camera_.update(window_.aspect());
        renderer_.setWireframe(renderer_.wireframe());
        renderer_.setDebugDrawEnabled(renderer_.debugDrawEnabled());
        renderer_.setTime(elapsed_);
        renderer_.render(scene_, camera_);

        if (!screenshotPath_.empty()) {
            takeScreenshot(screenshotPath_);
            screenshotPath_.clear();
        }

        imgui_->renderPass();
        window_.swapBuffers();
        renderer_.tickShaderReloads();

        ++framesRun_;
        if (frameLimit_ > 0 && framesRun_ >= frameLimit_)
            break;
    }
    return exitCode_;
}

void Application::handleHotkeys()
{
    if (input_.pressed(Action::Quit))
        quit();
    if (input_.pressed(Action::ToggleDebugUI)) {
        showImGui = !showImGui;
        // Grab/release the cursor with the overlay so aiming stops while UI.
        input_.setMouseCaptured(!showImGui);
    }
    if (input_.pressed(Action::ToggleWireframe))
        renderer_.setWireframe(!renderer_.wireframe());
    if (input_.pressed(Action::ToggleDebugDraw))
        renderer_.setDebugDrawEnabled(!renderer_.debugDrawEnabled());
    if (input_.pressed(Action::ToggleVsync))
        window_.setVsync(!window_.vsync());
}

void Application::takeScreenshot(const std::string &path)
{
    // Flip-Y correction for OpenGL's bottom-up framebuffer; raw 8-bit read
    // (gamma in the default framebuffer may differ per driver; verified
    // visually rather than pixel-perfect).
    glFlush();
    int32_t x = 0, y = 0, w = 0, h = 0;
    // use viewport from the last render
    // Screenshot reads the default framebuffer. The last bound pass may have
    // left the shadow-map viewport active, so reset explicitly to the window.
    Framebuffer::bindDefault();
    glFlush();
    w = static_cast<int32_t>(window_.frameWidth());
    h = static_cast<int32_t>(window_.frameHeight());
    x = 0;
    y = 0;
    std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 3);
    glReadPixels(x, y, w, h, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
    if (pixels.empty()) {
        HOR_LOG_WARN("screenshot capture failed (zero-size buffer)");
        return;
    }
    // Write TGA (24-bit, uncompressed, top-down); keeps the debug helper
    // dependency-free and works for both smoke tests and CI runs.
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) {
        HOR_LOG_WARN("screenshot open failed: {}", path);
        return;
    }
    uint8_t header[18] = {};
    header[2] = 2; // uncompressed truecolor
    header[12] = static_cast<uint8_t>(w & 0xFF);
    header[13] = static_cast<uint8_t>((w >> 8) & 0xFF);
    header[14] = static_cast<uint8_t>(h & 0xFF);
    header[15] = static_cast<uint8_t>((h >> 8) & 0xFF);
    header[16] = 24; // bpp
    std::fwrite(header, 1, sizeof(header), f);
    std::vector<uint8_t> flipped(pixels.size());
    for (int row = 0; row < h; ++row)
        std::memcpy(&flipped[static_cast<size_t>(row) * w * 3],
            &pixels[static_cast<size_t>(h - 1 - row) * w * 3], size_t(w) * 3);
    std::fwrite(flipped.data(), 1, flipped.size(), f);
    std::fclose(f);
    HOR_LOG_INFO("screenshot written: {}", path);
}

} // namespace horror
