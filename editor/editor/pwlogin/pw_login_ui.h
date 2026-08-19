#pragma once

#include <editor/mcp/pw_session_controller.h>

#include <base/basetypes.hpp>
#include <context/context.hpp>
#include <entt/entt.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

namespace Rml
{
class Event;
}

namespace unravel
{
/**
 * Editor-side owner of the Perfect World login / character-select overlay.
 *
 * The RmlUi document only displays the public pw_session_controller snapshot and
 * forwards user intents to it; all state transitions live in the controller.
 * This class creates the ui_document entity in the active scene, attaches the
 * document event listeners, pushes snapshot updates into the DOM once per frame
 * (on_frame_end, main thread), forces UI input while the mode is active, and
 * kicks off the login-scene load. It never stores the password: the field value
 * is read on Connect, handed to the controller, and cleared immediately.
 */
class pw_login_ui
{
public:
    pw_login_ui();
    ~pw_login_ui();

    auto init(rtti::context& ctx) -> bool;
    auto deinit(rtti::context& ctx) -> bool;

    void activate(rtti::context& ctx);
    void deactivate(rtti::context& ctx);
    auto is_active() const -> bool { return mode_active_; }

private:
    void on_frame_end(rtti::context& ctx, delta_t dt);
    void ensure_document_entity(rtti::context& ctx);
    void sync_document(rtti::context& ctx);
    void attach_listeners();
    void on_connect_clicked();
    void process_event(Rml::Event& event);

    // Preview proxy (WP5): mirrors the selected role's preview data onto a
    // scene proxy entity. Purely cosmetic; failures only log a diagnostic and
    // never affect the session.
    void service_preview_proxy(rtti::context& ctx);
    void update_preview_proxy(rtti::context& ctx, const pw_session_preview& preview);
    void remove_preview_proxy();

    // World view (WP7): replicates the in-world snapshot (self + nearby
    // entities) into the scene as colored capsule markers and follows the self
    // marker with the MCP camera. Markers are keyed by (epoch, id); an epoch
    // bump (reconnect/resync) wipes and re-creates them. Cosmetic only.
    void service_world_view(rtti::context& ctx);
    void clear_world_markers();

    static auto utf8_to_wide(const std::string& value) -> std::wstring;
    static auto xml_escape(const std::string& value) -> std::string;
    static auto proxy_mesh_ref_for(int32_t profession, int32_t gender) -> std::string;

    struct listener_bridge;
    std::unique_ptr<listener_bridge> listener_;

    struct world_marker
    {
        entt::handle handle;
        uint32_t color_rgba = 0;
    };

    std::shared_ptr<int> sentinel_ = std::make_shared<int>(0);
    entt::handle ui_entity_;
    entt::handle proxy_entity_;
    std::string proxy_mesh_ref_;      // mesh currently requested/shown on the proxy
    uint32_t applied_preview_serial_ = 0;
    int proxy_wait_frames_ = 0;
    std::unordered_map<int32_t, world_marker> world_markers_;
    entt::handle world_self_marker_;
    uint32_t world_epoch_ = 0;
    uint32_t world_map_loaded_epoch_ = 0; // Last auto-load attempt, including fail-soft failures.
    uint64_t applied_world_seq_ = 0;
    bool mode_active_ = false;
    void* attached_document_ = nullptr; // document the listeners are attached to
    uint64_t rendered_revision_ = 0;    // role list content currently in the DOM
    int32_t rendered_selection_ = -2;   // selection highlight currently in the DOM
    uint32_t rendered_preview_serial_ = 0; // preview data currently in the DOM
    std::string last_screen_;
    std::string last_status_text_;
    std::string last_error_text_;
};
} // namespace unravel
