#pragma once
#include "game/GameSession.hpp"
#include "game/Lobby.hpp"
#include "game/MenuVideo.hpp"
#include <functional>
namespace horror {
class Application;
class Frontend {
public:
    Frontend(Application &app, EntityId player, std::function<void()> reset);
    void draw();
    bool setVideo(std::unique_ptr<MenuVideo> video, const std::filesystem::path &file);
    void escape();
    void requestQuit() { confirmQuit_ = true; }
    bool confirmingQuit() const { return confirmQuit_; }
    void start();
    void leave();
    void resume();
    GameSession session;
private:
    enum class Page { Home, Settings, Multiplayer };
    void sync();
    Application &app_;
    EntityId player_;
    std::function<void()> reset_;
    Page page_ = Page::Home;
    Lobby lobby_;
    char address_[64] = "127.0.0.1";
    bool ready_ = false;
    bool confirmQuit_ = false;
    std::unique_ptr<MenuVideo> video_;
};
}
