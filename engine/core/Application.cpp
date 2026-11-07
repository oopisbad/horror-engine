#include "core/Application.hpp"

#include "anim/AnimationSystem.hpp"
#include "debug/FlightRecorder.hpp"
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
    // The fixed step may run before the first render pass; give every collider
    // real world bounds up front, otherwise the first steps test zero-size
    // boxes sitting at the origin.
    scene_.updateWorldMatrices();
    scene_.refreshColliderBounds();
    scene_.refreshTriggerBounds();

    while (running_ && !window_.shouldClose()) {
        const float frameDt = frameTimer_.tickRealtime();
        elapsed_ += frameDt;
        // Edge state rolls BEFORE new events arrive (pressed/released compare
        // this frame against the previous one; see Input::update).
        input_.update();
        window_.pollEvents();

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

        // Flight recorder frame row: pending look delta plus the capture /
        // focus / ImGui state that produced it. One ring slot, no allocation.
        {
            FrameTrace trace;
            trace.dt = frameDt;
            trace.steps = steps;
            trace.camPos = camera_.position;
            trace.camYaw = camera_.yaw;
            trace.camPitch = camera_.pitch;
            trace.pendingMouse = input_.mouseDelta();
            trace.mouseCaptured = input_.mouseCaptured();
            trace.windowFocused = input_.windowFocused();
            trace.imguiMouse = input_.mouseWantedByImGui;
            trace.imguiKeyboard = input_.keyboardWantedByImGui;
            trace.cameraWriter = lastCameraWriter();
            FlightRecorder::get().recordFrame(trace);
        }

        // Animation runs at the variable frame rate (not the fixed step) so
        // playback stays smooth; gameplay decides what to play per frame.
        AnimationSystem::update(scene_, frameDt);

        scene_.updateWorldMatrices();
        scene_.refreshColliderBounds();
        scene_.refreshTriggerBounds();
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
    // Every hotkey is noted for the flight recorder *before* acting, so a dump
    // always shows what was pressed on the frame something went wrong.
    if (input_.pressed(Action::Quit)) {
        noteHotkey(kHotkeyQuit);
        quit();
    }
    if (input_.pressed(Action::ToggleDebugUI)) {
        noteHotkey(kHotkeyUI);
        showImGui = !showImGui;
        // Grab/release the cursor with the overlay so aiming stops while UI.
        input_.setMouseCaptured(!showImGui);
    }
    if (input_.pressed(Action::ToggleWireframe)) {
        noteHotkey(kHotkeyWireframe);
        renderer_.setWireframe(!renderer_.wireframe());
    }
    if (input_.pressed(Action::ToggleDebugDraw)) {
        noteHotkey(kHotkeyDebugDraw);
        renderer_.setDebugDrawEnabled(!renderer_.debugDrawEnabled());
    }
    if (input_.pressed(Action::ToggleVsync)) {
        noteHotkey(kHotkeyVsync);
        window_.setVsync(!window_.vsync());
    }
    if (input_.pressed(Action::DumpRecorder)) {
        noteHotkey(kHotkeyDump);
        FlightRecorder::get().dump("debug_dump.txt", "F9 hotkey");
    }
}

void Application::takeScreenshot(const std::string &path)
{
    // Flip-Y correction for OpenGL's bottom-up framebuffer; raw 8-bit read
    // (gamma in the default framebuffer may differ per driver; verified
    // visually rather than pixel-perfect).
    glFlush();
    // Screenshot reads the default framebuffer. The last bound pass may have
    // left the shadow-map viewport active, so reset explicitly to the window.
    Framebuffer::bindDefault();
    glFlush();
    const int32_t w = static_cast<int32_t>(window_.frameWidth());
    const int32_t h = static_cast<int32_t>(window_.frameHeight());
    if (w <= 0 || h <= 0) {
        HOR_LOG_WARN("screenshot capture failed ({}x{} framebuffer)", w, h);
        return;
    }
    // glReadPixels honours GL_PACK_ALIGNMENT (default 4): each RGB row starts
    // on a 4-byte boundary, so a width whose 3*w is not a multiple of 4 needs
    // up to 3 bytes of padding per row. The old buffer was exactly w*h*3, so
    // any such width (1281, 1366, ...) wrote past the end of the vector -- heap
    // corruption -- and the tightly-packed flip loop sheared the image. Read
    // tightly packed instead, and size the allocation for the aligned case as
    // a defensive margin.
    const size_t rowBytes = static_cast<size_t>(w) * 3;
    const size_t alignedRowBytes = (rowBytes + 3u) & ~size_t(3u);
    std::vector<uint8_t> pixels(alignedRowBytes * static_cast<size_t>(h), 0);
    while (glGetError() != GL_NO_ERROR) { // drain stale errors before the read
    }
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    const GLenum readError = glGetError();
    if (readError != GL_NO_ERROR) {
        HOR_LOG_WARN("glReadPixels failed (0x{:x}); screenshot not written",
            static_cast<unsigned>(readError));
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
    std::vector<uint8_t> flipped(rowBytes * static_cast<size_t>(h));
    for (int row = 0; row < h; ++row)
        std::memcpy(&flipped[static_cast<size_t>(row) * rowBytes],
            &pixels[static_cast<size_t>(h - 1 - row) * rowBytes], rowBytes);
    std::fwrite(flipped.data(), 1, flipped.size(), f);
    std::fclose(f);
    HOR_LOG_INFO("screenshot written: {}", path);
}

} // namespace horror
