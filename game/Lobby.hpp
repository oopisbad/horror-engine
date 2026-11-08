#pragma once
#include <memory>
#include <string>
#include <vector>
#include <cstdint>
namespace horror {
struct LobbyMember { uint32_t id; bool ready; };
// Direct-IP, player-hosted lobby. One host coordinates membership for 4 peers.
// No world replication, matchmaking, authentication or NAT traversal yet.
class Lobby {
public:
    Lobby();
    ~Lobby();
    Lobby(const Lobby &) = delete;
    Lobby &operator=(const Lobby &) = delete;
    bool host(uint16_t port = 27015);
    bool join(const std::string &ipv4, uint16_t port = 27015);
    void poll();
    void leave();
    void setReady(bool ready);
    bool hosting() const;
    bool active() const;
    uint32_t localId() const;
    const std::vector<LobbyMember> &members() const;
    const std::string &status() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
int runLobbyTests();
}
