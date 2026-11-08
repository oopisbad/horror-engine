#include "game/Frontend.hpp"
#include "core/Application.hpp"
#include <imgui.h>
#include <algorithm>
#include <utility>

namespace horror {
Frontend::Frontend(Application &app, EntityId player, std::function<void()> reset)
    : app_(app), player_(player), reset_(std::move(reset)) { sync(); }
void Frontend::sync() {
    const bool playing = session.state() == GameSession::State::Playing;
    app_.simulationEnabled = playing;
    app_.gameplayInputEnabled = playing;
    app_.showImGui = false;
    app_.input().consumeJumpPress();
    app_.input().consumeMouseDelta();
    app_.input().setMouseCaptured(playing);
}
void Frontend::start() { lobby_.leave(); reset_(); session.startSolo(player_); page_ = Page::Home; sync(); }
void Frontend::leave() { session.leave(); page_ = Page::Home; sync(); }
void Frontend::resume() { session.resume(); page_ = Page::Home; sync(); }
void Frontend::escape() {
    if (confirmQuit_) { confirmQuit_ = false; return; }
    if (page_ != Page::Home) { lobby_.leave(); page_ = Page::Home; return; }
    if (session.state() == GameSession::State::Playing) { session.pause(); sync(); }
    else if (session.state() == GameSession::State::Paused) resume();
}
bool Frontend::setVideo(std::unique_ptr<MenuVideo> video, const std::filesystem::path &file) {
    if (!video || !video->open(file)) return false;
    video_ = std::move(video);
    return true;
}
void Frontend::draw() {
    lobby_.poll();
    if (session.state() == GameSession::State::Playing) return;
    ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0,0,0,1));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.10f,0.10f,0.10f,0.6f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.18f,0.18f,0.18f,0.6f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1,1,1,1));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24,24));
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0,0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8,16));
    ImGui::Begin("Frontend", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    if (video_) {
        video_->update(ImGui::GetIO().DeltaTime);
        if (video_->texture() && video_->aspect() > 0) {
            float w=vp->Size.x,h=w/video_->aspect();
            if(h>vp->Size.y) {h=vp->Size.y;w=h*video_->aspect();}
            ImVec2 p(vp->Pos.x+(vp->Size.x-w)*0.5f,vp->Pos.y+(vp->Size.y-h)*0.5f);
            ImGui::GetWindowDrawList()->AddImage(static_cast<ImTextureID>(video_->texture()),p,ImVec2(p.x+w,p.y+h));
        }
    }
    const float left=std::max(24.0f,vp->Size.x*0.075f);
    ImGui::SetCursorPos(ImVec2(left, std::max(30.0f,vp->Size.y*0.32f)));
    ImGui::BeginGroup();
    const ImVec2 button(std::min(280.0f,vp->Size.x-left-24),42);
    if(page_ == Page::Home) {
        ImGui::SetWindowFontScale(1.5f);
        if(session.state()==GameSession::State::Paused) {
            if(ImGui::Button("Resume",button)) resume();
            if(ImGui::Button("Main menu",button)) leave();
        } else if(ImGui::Button("Play",button)) start();
        // Intentional placeholder: settings design is still pending.
        ImGui::Button("Settings",button);
        if(ImGui::Button("Multiplayer",button)) page_=Page::Multiplayer;
        if(ImGui::Button("Quit",button)) requestQuit();
        ImGui::SetWindowFontScale(1.0f);
    } else {
        ImGui::TextUnformatted("Multiplayer");
        ImGui::TextUnformatted(lobby_.status().c_str());
        if(!lobby_.active()) {
            if(ImGui::Button("Host lobby",button)) {ready_=false;lobby_.host();}
            ImGui::SetNextItemWidth(button.x);
            ImGui::InputText("Host IPv4",address_,sizeof(address_));
            if(ImGui::Button("Join lobby",button)) {ready_=false;lobby_.join(address_);}
        } else {
            ImGui::Text("Players: %d / 4",static_cast<int>(lobby_.members().size()));
            for(auto &p:lobby_.members()) ImGui::Text("Player %u%s%s - %s",p.id,p.id==1?" (host)":"",p.id==lobby_.localId()?" (you)":"",p.ready?"ready":"not ready");
            if(lobby_.localId() && ImGui::Checkbox("Ready",&ready_)) lobby_.setReady(ready_);
            if(ImGui::Button("Leave lobby",button)) lobby_.leave();
        }
        ImGui::TextWrapped("Lobby preview. Shared gameplay is not available yet.");
        if(ImGui::Button("Back",button)) {lobby_.leave();page_=Page::Home;}
    }
    ImGui::EndGroup();
    if (confirmQuit_) ImGui::OpenPopup("Misclick?");
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x*0.5f, vp->Pos.y + vp->Size.y*0.5f), ImGuiCond_Appearing, ImVec2(0.5f,0.5f));
    if (ImGui::BeginPopupModal("Misclick?", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        if (!confirmQuit_) ImGui::CloseCurrentPopup();
        else {
            ImGui::TextUnformatted("Ready to leave the dark?");
            if (ImGui::Button("Keep playing", ImVec2(130,36))) { confirmQuit_ = false; ImGui::CloseCurrentPopup(); }
            ImGui::SetItemDefaultFocus();
            ImGui::SameLine();
            if (ImGui::Button("Quit game", ImVec2(130,36))) { lobby_.leave(); app_.quit(); confirmQuit_ = false; ImGui::CloseCurrentPopup(); }
        }
        ImGui::EndPopup();
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(5);
}
}