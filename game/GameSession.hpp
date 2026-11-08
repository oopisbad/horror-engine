#pragma once
#include "scene/Components.hpp"
#include <cstdint>
#include <vector>
#include <string>

namespace horror {
// Stable session IDs are distinct from scene-local entity handles. A future
// transport must resolve ownership here rather than trusting a client entity ID.
using PlayerId = uint64_t;
struct SessionPlayer {
    PlayerId id;
    EntityId entity;
    bool locallyControlled;
    std::string characterId = "placeholder";
};
class GameSession {
public:
    enum class State { Menu, Playing, Paused };
    State state() const { return state_; }
    const std::vector<SessionPlayer> &players() const { return players_; }
    uint64_t tick() const { return tick_; }
    void startSolo(EntityId entity) {
        players_ = {{1, entity, true, "placeholder"}};
        tick_ = 0;
        state_ = State::Playing;
    }
    void pause() { if (state_ == State::Playing) state_ = State::Paused; }
    void resume() { if (state_ == State::Paused) state_ = State::Playing; }
    void leave() { players_.clear(); tick_ = 0; state_ = State::Menu; }
    void advance() { if (state_ == State::Playing) ++tick_; }
    EntityId localEntity() const {
        for (const auto &p : players_) if (p.locallyControlled) return p.entity;
        return kInvalidEntity;
    }
private:
    State state_ = State::Menu;
    uint64_t tick_ = 0;
    std::vector<SessionPlayer> players_;
};
}
