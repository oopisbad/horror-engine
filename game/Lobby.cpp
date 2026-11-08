#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include "game/Lobby.hpp"
#include <array>
#include <chrono>
#include <sstream>
#include <thread>
#include <cstdio>

namespace horror {
namespace {
using Clock = std::chrono::steady_clock;
bool nonblocking(SOCKET s) { u_long one = 1; return ioctlsocket(s,FIONBIO,&one) == 0; }
void closeSocket(SOCKET &s) { if (s != INVALID_SOCKET) closesocket(s); s = INVALID_SOCKET; }
struct Peer {
    SOCKET socket = INVALID_SOCKET;
    std::string incoming, outgoing;
    uint32_t id = 0;
    bool ready = false, greeted = false;
    Clock::time_point received = Clock::now();
};
bool pump(Peer &p, std::vector<std::string> &lines) {
    if (p.outgoing.size() > 4096) return false;
    if (!p.outgoing.empty()) {
        int n = send(p.socket,p.outgoing.data(),static_cast<int>(p.outgoing.size()),0);
        if (n > 0) p.outgoing.erase(0,n);
        else if (n == 0 || WSAGetLastError() != WSAEWOULDBLOCK) return false;
    }
    char buf[512];
    for (int budget = 0; budget < 8; ++budget) {
        int n = recv(p.socket,buf,sizeof(buf),0);
        if (n == 0) return false;
        if (n < 0) { if (WSAGetLastError() != WSAEWOULDBLOCK) return false; break; }
        p.received = Clock::now();
        p.incoming.append(buf,n);
        if (p.incoming.size() > 2048) return false;
        size_t end;
        while ((end=p.incoming.find('\n')) != std::string::npos) {
            if (end > 256) return false;
            lines.push_back(p.incoming.substr(0,end));
            p.incoming.erase(0,end+1);
        }
    }
    return Clock::now()-p.received < std::chrono::seconds(10);
}
}
struct Lobby::Impl {
    bool initialized = false, isHost = false, connecting = false;
    SOCKET listener = INVALID_SOCKET;
    std::array<Peer,3> peers;
    Peer server;
    uint32_t self = 0;
    bool ready = false;
    std::vector<LobbyMember> roster;
    std::string message = "Not connected";
    Clock::time_point beat = Clock::now(), connectTime;
    void broadcast() {
        roster = {{1,ready}};
        for (auto &p:peers) if(p.socket != INVALID_SOCKET && p.greeted) roster.push_back({p.id,p.ready});
        std::string line="ROSTER";
        for(auto &m:roster) line += " " + std::to_string(m.id) + " " + std::to_string(m.ready);
        line += "\n";
        for(auto &p:peers) if(p.socket != INVALID_SOCKET && p.greeted) p.outgoing += line;
    }
};
Lobby::Lobby():impl_(std::make_unique<Impl>()) { WSADATA data; impl_->initialized=WSAStartup(MAKEWORD(2,2),&data)==0; }
Lobby::~Lobby() { leave(); if(impl_->initialized) WSACleanup(); }
void Lobby::leave() {
    auto &s=*impl_; closeSocket(s.listener); closeSocket(s.server.socket); s.server=Peer{};
    for(auto &p:s.peers) {closeSocket(p.socket);p=Peer{};}
    s.isHost=s.connecting=s.ready=false; s.self=0; s.roster.clear(); s.message="Not connected";
}
bool Lobby::host(uint16_t port) {
    leave(); auto &s=*impl_;
    if(!s.initialized) {s.message="Windows networking unavailable";return false;}
    s.listener=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    BOOL exclusive=TRUE; setsockopt(s.listener,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,reinterpret_cast<const char*>(&exclusive),sizeof(exclusive));
    sockaddr_in address{}; address.sin_family=AF_INET; address.sin_port=htons(port); address.sin_addr.s_addr=htonl(INADDR_ANY);
    if(s.listener==INVALID_SOCKET || bind(s.listener,reinterpret_cast<sockaddr*>(&address),sizeof(address))!=0 || listen(s.listener,4)!=0 || !nonblocking(s.listener)) {
        leave();s.message="Cannot host: port unavailable";return false;
    }
    s.isHost=true;s.self=1;s.message="Hosting on TCP port "+std::to_string(port);s.broadcast();return true;
}
bool Lobby::join(const std::string &ip,uint16_t port) {
    leave();auto &s=*impl_;sockaddr_in address{};address.sin_family=AF_INET;address.sin_port=htons(port);
    if(!s.initialized || inet_pton(AF_INET,ip.c_str(),&address.sin_addr)!=1) {s.message="Enter a valid IPv4 address";return false;}
    s.server.socket=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    if(s.server.socket==INVALID_SOCKET || !nonblocking(s.server.socket)) {leave();s.message="Socket creation failed";return false;}
    int result=connect(s.server.socket,reinterpret_cast<sockaddr*>(&address),sizeof(address));
    if(result!=0 && WSAGetLastError()!=WSAEWOULDBLOCK) {leave();s.message="Connection failed";return false;}
    s.connecting=true;s.connectTime=Clock::now();s.message="Connecting...";return true;
}
void Lobby::poll() {
    auto &s=*impl_;
    if(s.connecting) {
        fd_set writeSet,errorSet;FD_ZERO(&writeSet);FD_ZERO(&errorSet);FD_SET(s.server.socket,&writeSet);FD_SET(s.server.socket,&errorSet);timeval zero{};
        select(0,nullptr,&writeSet,&errorSet,&zero);
        if(FD_ISSET(s.server.socket,&errorSet) || Clock::now()-s.connectTime>std::chrono::seconds(8)) {leave();s.message="Connection failed or timed out";return;}
        if(!FD_ISSET(s.server.socket,&writeSet)) return;
        int error=0,len=sizeof(error);getsockopt(s.server.socket,SOL_SOCKET,SO_ERROR,reinterpret_cast<char*>(&error),&len);
        if(error) {leave();s.message="Connection failed";return;}
        s.connecting=false;s.server.outgoing="HELLO 1\n";s.server.received=Clock::now();s.message="Joining lobby...";
    }
    if(s.isHost) {
        // Bounded work, even if many connection attempts arrive at once.
        for(int i=0;i<4;++i) {
            SOCKET incoming=accept(s.listener,nullptr,nullptr);if(incoming==INVALID_SOCKET) break;
            Peer *slot=nullptr;for(auto &p:s.peers) if(p.socket==INVALID_SOCKET) {slot=&p;break;}
            if(!slot || !nonblocking(incoming)) {closesocket(incoming);continue;}
            *slot=Peer{};slot->socket=incoming;slot->id=static_cast<uint32_t>(slot-s.peers.data())+2;
        }
        for(auto &p:s.peers) {
            if(p.socket==INVALID_SOCKET) continue;
            std::vector<std::string> lines;bool ok=pump(p,lines);
            for(auto &line:lines) {
                if(!p.greeted && line=="HELLO 1") {p.greeted=true;p.outgoing+="WELCOME "+std::to_string(p.id)+"\n";s.broadcast();}
                else if(p.greeted && (line=="READY 0" || line=="READY 1")) {p.ready=line.back()=='1';s.broadcast();}
                else if(line!="PING") ok=false;
            }
            if(!ok) {closeSocket(p.socket);p=Peer{};s.broadcast();}
        }
    } else if(s.server.socket!=INVALID_SOCKET) {
        std::vector<std::string> lines;
        if(!pump(s.server,lines)) {leave();s.message="Disconnected: host left, lobby full, or connection lost";return;}
        for(auto &line:lines) {
            std::istringstream in(line);std::string cmd;in>>cmd;
            if(cmd=="WELCOME") {uint32_t id=0;in>>id;if(id<2 || id>4) {leave();s.message="Invalid lobby protocol";return;}s.self=id;s.message="Connected";}
            else if(cmd=="ROSTER") {
                std::vector<LobbyMember> roster;uint32_t id;int ready;unsigned mask=0;
                while(in>>id>>ready) {if(id<1 || id>4 || ready<0 || ready>1 || (mask&(1u<<id))) {leave();s.message="Invalid lobby roster";return;}mask|=1u<<id;roster.push_back({id,ready==1});}
                if(roster.empty() || !(mask&2) || !(mask&(1u<<s.self))) {leave();s.message="Invalid lobby roster";return;}s.roster=std::move(roster);
            } else if(cmd!="PING") {leave();s.message="Unsupported lobby protocol";return;}
        }
    }
    if(Clock::now()-s.beat>std::chrono::seconds(1)) {
        s.beat=Clock::now();
        if(s.isHost) {for(auto &p:s.peers) if(p.socket!=INVALID_SOCKET) p.outgoing+="PING\n";}
        else if(s.server.socket!=INVALID_SOCKET) s.server.outgoing+="PING\n";
    }
}
void Lobby::setReady(bool ready) {auto &s=*impl_;s.ready=ready;if(s.isHost)s.broadcast();else if(s.self)s.server.outgoing+=std::string("READY ")+(ready?"1\n":"0\n");}
bool Lobby::hosting() const {return impl_->isHost;}
bool Lobby::active() const {return impl_->isHost || impl_->server.socket!=INVALID_SOCKET;}
uint32_t Lobby::localId() const {return impl_->self;}
const std::vector<LobbyMember> &Lobby::members() const {return impl_->roster;}
const std::string &Lobby::status() const {return impl_->message;}
int runLobbyTests() {
    Lobby host,a,b,c,extra;
    uint16_t port=37151;
    if(!host.host(port)) return 1;
    a.join("127.0.0.1",port);b.join("127.0.0.1",port);c.join("127.0.0.1",port);
    auto pumpAll=[&](int n) {for(int i=0;i<n;++i){host.poll();a.poll();b.poll();c.poll();extra.poll();std::this_thread::sleep_for(std::chrono::milliseconds(2));}};
    pumpAll(250);
    bool ok=host.members().size()==4 && a.members().size()==4 && b.members().size()==4 && c.members().size()==4;
    a.setReady(true);pumpAll(80);
    bool ready=false;for(auto &m:host.members())if(m.id==a.localId())ready=m.ready;ok &= ready;
    extra.join("127.0.0.1",port);pumpAll(80);ok &= !extra.active() && host.members().size()==4;
    b.leave();pumpAll(80);ok &= host.members().size()==3 && a.members().size()==3;
    extra.join("127.0.0.1",port);pumpAll(80);ok &= extra.members().size()==4;
    host.leave();pumpAll(80);ok &= !a.active() && !c.active() && !extra.active();
    ok &= !extra.join("invalid-address",port);
    std::printf("Lobby integration: %s (4 peers, ready, capacity, leave, rejoin, host shutdown, invalid address)\n",ok?"PASS":"FAIL");
    return ok?0:1;
}
}
