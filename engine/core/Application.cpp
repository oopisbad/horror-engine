#include "core/Application.hpp"

#include "anim/AnimationSystem.hpp"
#include "debug/FlightRecorder.hpp"
#include "debug/ImGuiOverlay.hpp"
#include "render/Framebuffer.hpp"

#include <algorithm>
#include <thread>
#include <chrono>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <cstdio>
#include <cstring>
#include <vector>

namespace horror {
namespace {

class FramePacer {
public:
    explicit FramePacer(unsigned rate) : period_(rate ? 1.0 / rate : 0.0) {
#ifdef _WIN32
        if (rate) {
            // High-resolution waitable timer is local to this process. If the
            // Windows build does not support the flag, keep portable pacing.
            constexpr DWORD highResolution = 0x00000002;
            timer_ = CreateWaitableTimerExW(nullptr, nullptr, highResolution,
                TIMER_MODIFY_STATE | SYNCHRONIZE);
        }
#endif
    }
    ~FramePacer() {
#ifdef _WIN32
        if (timer_) CloseHandle(timer_);
#endif
    }

    void wait(std::chrono::steady_clock::time_point start) const {
        if (period_.count() <= 0.0)
            return;
        const auto deadline = start + period_;
        const auto remaining = deadline - std::chrono::steady_clock::now();
        if (remaining.count() <= 0.0)
            return;
#ifdef _WIN32
        if (timer_) {
            using Tick = std::chrono::duration<int64_t, std::ratio<1, 10000000>>;
            LARGE_INTEGER due{};
            due.QuadPart = -std::max<int64_t>(1, std::chrono::duration_cast<Tick>(remaining).count());
            if (SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE)) {
                if (WaitForSingleObject(timer_, INFINITE) == WAIT_OBJECT_0)
                    return;
            }
        }
#endif
        std::this_thread::sleep_until(deadline);
    }

private:
    std::chrono::duration<double> period_;
#ifdef _WIN32
    HANDLE timer_ = nullptr;
#endif
};

} // namespace

Application::Application(Config config)
    : config_(config)
    , window_(Window::Desc{
          config_.windowTitle, config_.windowWidth, config_.windowHeight,
          4, 5, config_.vsync, config_.fullscreen, config_.visible })
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

    const FramePacer pacer(config_.frameRateLimit);
    float nextShaderCheck = 0.0f;
    while (running_ && !window_.shouldClose()) {
        const auto frameStart = std::chrono::steady_clock::now();
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

        if (onDrawUI) onDrawUI();
        if (!simulationEnabled) input_.consumeJumpPress();
        const int steps = logic_.stepsToRun(frameDt);
        for (int i = 0; i < steps; ++i)
            if (simulationEnabled && onFixedStep)
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
        if (simulationEnabled) AnimationSystem::update(scene_, frameDt);

        if (worldVisible) {
            scene_.updateWorldMatrices();
            scene_.refreshColliderBounds();
            camera_.update(window_.aspect());
            renderer_.setTime(elapsed_);
            renderer_.render(scene_, camera_);
        } else {
            renderer_.resetHistory();
            Framebuffer::bindDefault();
            glViewport(0, 0, window_.frameWidth(), window_.frameHeight());
            const GLfloat black[4] = {0, 0, 0, 1};
            glClearBufferfv(GL_COLOR, 0, black);
        }

        imgui_->renderPass();
        if (!screenshotPath_.empty()) {
            takeScreenshot(screenshotPath_);
            screenshotPath_.clear();
        }

        window_.swapBuffers();
        if (elapsed_ >= nextShaderCheck) {
            renderer_.tickShaderReloads();
            nextShaderCheck = elapsed_ + 0.25f;
        }

        ++framesRun_;
        if (frameLimit_ > 0 && framesRun_ >= frameLimit_)
            break;
        pacer.wait(frameStart);
    }
    return exitCode_;
}

void Application::handleHotkeys()
{
    // Every hotkey is noted for the flight recorder *before* acting, so a dump
    // always shows what was pressed on the frame something went wrong.
    if (input_.pressed(Action::Quit)) {
        noteHotkey(kHotkeyQuit);
        if (onEscape) onEscape(); else quit();
    }
    if (gameplayInputEnabled && input_.pressed(Action::ToggleDebugUI)) {
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
    Framebuffer::bindDefault();
    const auto w = static_cast<int32_t>(window_.frameWidth());
    const auto h = static_cast<int32_t>(window_.frameHeight());
    if (w <= 0 || h <= 0 || w > 65535 || h > 65535)
        return;
    std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 3);
    GLint previousPackAlignment = 0;
    glGetIntegerv(GL_PACK_ALIGNMENT, &previousPackAlignment);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_BGR, GL_UNSIGNED_BYTE, pixels.data());
    glPixelStorei(GL_PACK_ALIGNMENT, previousPackAlignment);
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
    header[17] = 0x20; // pixel rows below are top-down
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
