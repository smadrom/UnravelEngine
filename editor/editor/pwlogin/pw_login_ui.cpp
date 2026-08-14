#include "pw_login_ui.h"

#include <editor/mcp/mcp_system.h>

#include <editor/hub/hub.h>
#include <editor/hub/panels/pw_login_panel/pw_login_panel.h>

#include <engine/assets/asset_manager.h>
#include <engine/defaults/defaults.h>
#include <engine/ecs/ecs.h>
#include <engine/engine.h>
#include <engine/events.h>
#include <engine/input/input.h>
#include <engine/rendering/ecs/components/model_component.h>
#include <engine/rendering/material.h>
#include <engine/rendering/mesh.h>
#include <engine/rendering/model.h>
#include <engine/ui/ecs/components/ui_document_component.h>
#include <engine/ui/ui_tree.h>

#include <RmlUi/Core.h>

#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <utility>

#if defined(_WIN32)
#include <Windows.h>
#endif

namespace unravel
{
namespace
{
constexpr const char* kDocumentAsset = "editor:/data/ui/pw_login.rhtml";

void set_element_text(Rml::ElementDocument* document, const char* id, const std::string& text)
{
    if(Rml::Element* element = document->GetElementById(id))
    {
        if(element->GetInnerRML() != text)
        {
            element->SetInnerRML(text);
        }
    }
}

void set_element_disabled(Rml::ElementDocument* document, const char* id, bool disabled)
{
    if(Rml::Element* element = document->GetElementById(id))
    {
        if(disabled && !element->HasAttribute("disabled"))
        {
            element->SetAttribute("disabled", "");
        }
        else if(!disabled && element->HasAttribute("disabled"))
        {
            element->RemoveAttribute("disabled");
        }
    }
}

void show_screen(Rml::ElementDocument* document, const char* visible_id)
{
    static const char* screens[] = {"pw-screen-login", "pw-screen-select", "pw-screen-inworld"};
    for(const char* id : screens)
    {
        if(Rml::Element* element = document->GetElementById(id))
        {
            const bool show = std::string(id) == visible_id;
            element->SetProperty("display", show ? "block" : "none");
        }
    }
}

auto get_form_control(Rml::ElementDocument* document, const char* id) -> Rml::ElementFormControl*
{
    Rml::Element* element = document->GetElementById(id);
    if(element == nullptr ||
       (element->GetTagName() != "input" && element->GetTagName() != "textarea" && element->GetTagName() != "select"))
    {
        return nullptr;
    }
    return static_cast<Rml::ElementFormControl*>(element);
}

auto get_input_value(Rml::ElementDocument* document, const char* id) -> std::string
{
    if(auto* control = get_form_control(document, id))
    {
        return control->GetValue();
    }
    return {};
}

void set_input_value(Rml::ElementDocument* document, const char* id, const std::string& value)
{
    if(auto* control = get_form_control(document, id))
    {
        control->SetValue(value);
    }
}

auto profession_name(int32_t profession) -> std::string
{
    static const char* names[] = {
        "Воин", "Маг", "Друид", "Оборотень", "Ассасин",
        "Лучник", "Жрец", "Призыватель", "Рыцарь", "Страж",
    };
    if(profession >= 0 && profession < static_cast<int32_t>(std::size(names)))
    {
        return names[profession];
    }
    return "Класс " + std::to_string(profession);
}

auto role_caption(const pw_session_role& role) -> std::string
{
    std::string caption = profession_name(role.profession);
    caption += role.gender == 1 ? " ♀" : " ♂";
    caption += ", ур. " + std::to_string(role.level);
    return caption;
}

// WP5 preview proxy mapping: (profession, gender) -> mesh under the login
// content root. Only the bare 武侠男 body is currently converted; every class
// falls back to it and a missing file only logs a diagnostic. New per-class
// exports slot into this table without touching the flow.
constexpr const char* kProxyFallbackMesh = "characters/player/model_57f7b4eb.gltf";

constexpr uint32_t kProxyAssetMaxWaitFrames = 600;

auto a3dcolor_to_math(uint32_t argb) -> math::color
{
    const float a = static_cast<float>((argb >> 24) & 0xFF) / 255.0f;
    const float r = static_cast<float>((argb >> 16) & 0xFF) / 255.0f;
    const float g = static_cast<float>((argb >> 8) & 0xFF) / 255.0f;
    const float b = static_cast<float>(argb & 0xFF) / 255.0f;
    return math::color(r, g, b, a);
}

// WP7 world markers: flat-color capsules, one per nearby entity kind.
auto world_kind_color(const pw_world_entity& entity) -> math::color
{
    if(entity.kind == "monster")
    {
        return entity.dead ? math::color(0.30f, 0.30f, 0.30f, 1.0f)
                           : math::color(0.90f, 0.15f, 0.10f, 1.0f);
    }
    if(entity.kind == "npc")
    {
        return math::color(0.10f, 0.80f, 0.20f, 1.0f);
    }
    if(entity.kind == "player")
    {
        return math::color(0.20f, 0.45f, 1.00f, 1.0f);
    }
    return math::color(0.95f, 0.85f, 0.10f, 1.0f); // matter/mine
}

void apply_marker_model(rtti::context& ctx, entt::handle marker, const math::color& color)
{
    auto& am = ctx.get_cached<asset_manager>();
    auto mesh_handle = am.get_asset<mesh>("engine:/embedded/capsule_2m");
    mesh_handle.submit();
    model marker_model;
    marker_model.set_lod(mesh_handle, 0);
    auto material_instance = std::make_shared<pbr_material>();
    material_instance->set_base_color(color);
    material_instance->set_cull_type(cull_type::none);
    marker_model.set_material_instance(material_instance, 0);
    auto& model_comp = marker.get_or_emplace<model_component>();
    model_comp.set_model(marker_model);
}
} // namespace

struct pw_login_ui::listener_bridge : public Rml::EventListener
{
    pw_login_ui* owner = nullptr;

    void ProcessEvent(Rml::Event& event) override
    {
        if(owner == nullptr)
        {
            return;
        }
        owner->process_event(event);
    }
};

pw_login_ui::pw_login_ui()
    : listener_(std::make_unique<listener_bridge>())
{
    listener_->owner = this;
}

pw_login_ui::~pw_login_ui() = default;

auto pw_login_ui::init(rtti::context& ctx) -> bool
{
    auto& ev = ctx.get_cached<events>();
    ev.on_frame_end.connect(sentinel_, -2000, this, &pw_login_ui::on_frame_end);
    return true;
}

auto pw_login_ui::deinit(rtti::context& ctx) -> bool
{
    deactivate(ctx);
    return true;
}

void pw_login_ui::activate(rtti::context& ctx)
{
    if(mode_active_)
    {
        return;
    }
    mode_active_ = true;
    auto& mcp = ctx.get_cached<mcp_system>();
    mcp.ensure_camera(ctx);
    if(!mcp.has_login_terrain() && mcp.get_login_load_status().status == "idle")
    {
        mcp.start_login_load("app:/data/login", 3, false);
    }
    auto& panel = ctx.get_cached<hub>().get_panels().get_pw_login_panel();
    panel.focus();
    ctx.get_cached<input_system>().manager.set_is_input_allowed(true);
}

void pw_login_ui::deactivate(rtti::context& ctx)
{
    if(!mode_active_)
    {
        return;
    }
    mode_active_ = false;
    auto& session = ctx.get_cached<mcp_system>().get_pw_session();
    if(session.get_snapshot().state != pw_session_state::idle)
    {
        session.disconnect();
    }
    remove_preview_proxy();
    clear_world_markers();
    if(ui_entity_ && ui_entity_.valid() && ui_entity_.all_of<ui_document_component>())
    {
        ui_entity_.get<ui_document_component>().set_enabled(false);
    }
    ctx.get_cached<input_system>().manager.set_is_input_allowed(false);
}

void pw_login_ui::ensure_document_entity(rtti::context& ctx)
{
    if(ui_entity_ && ui_entity_.valid() && ui_entity_.all_of<ui_document_component>())
    {
        return;
    }
    auto& scene = ctx.get_cached<ecs>().get_scene();
    ui_entity_ = defaults::create_ui_document_entity(ctx, scene, "PW Login UI");
    auto& ui_comp = ui_entity_.get<ui_document_component>();
    auto& am = ctx.get_cached<asset_manager>();
    ui_comp.asset = am.get_asset<ui_tree>(kDocumentAsset);
    ui_comp.render_mode = ui_render_mode::screen_space_overlay;
    attached_document_ = nullptr;
    rendered_revision_ = 0;
    rendered_selection_ = -2;
    rendered_preview_serial_ = 0;
}

void pw_login_ui::attach_listeners()
{
    auto& ui_comp = ui_entity_.get<ui_document_component>();
    Rml::ElementDocument* document = ui_comp.document;
    if(document == nullptr)
    {
        return;
    }
    for(const char* id : {"pw-connect", "pw-start", "pw-back", "pw-disconnect", "pw-role-list",
                          "pw-move-fwd", "pw-stop", "pw-jump", "pw-target", "pw-attack"})
    {
        if(Rml::Element* element = document->GetElementById(id))
        {
            element->AddEventListener("click", listener_.get());
        }
    }
    attached_document_ = document;
}

void pw_login_ui::process_event(Rml::Event& event)
{
    Rml::Element* target = event.GetTargetElement();
    if(target == nullptr)
    {
        return;
    }
    rtti::context& ctx = engine::context();
    auto& session = ctx.get_cached<mcp_system>().get_pw_session();
    Rml::Element* element = target;
    while(element != nullptr)
    {
        const std::string id = element->GetId();
        if(id == "pw-connect")
        {
            on_connect_clicked();
            return;
        }
        if(id == "pw-start")
        {
            session.enter_selected_role();
            return;
        }
        if(id == "pw-back" || id == "pw-disconnect")
        {
            session.disconnect();
            return;
        }
        if(id == "pw-move-fwd" || id == "pw-stop" || id == "pw-jump" ||
           id == "pw-target" || id == "pw-attack")
        {
            const pw_session_snapshot snapshot = session.get_snapshot();
            if(id == "pw-move-fwd")
            {
                const pw_world_self& self = snapshot.world.self;
                session.world_action("move_to",
                                     self.x + self.dir_x * 5.0f,
                                     self.y + self.dir_y * 5.0f,
                                     self.z + self.dir_z * 5.0f,
                                     0);
            }
            else if(id == "pw-stop")
            {
                session.world_action("stop_move", 0.0f, 0.0f, 0.0f, 0);
            }
            else if(id == "pw-jump")
            {
                session.world_action("jump", 0.0f, 0.0f, 0.0f, 0);
            }
            else if(id == "pw-target")
            {
                // Nearest living monster first, then NPC, then anything alive.
                const pw_world_entity* pick = nullptr;
                for(const char* kind : {"monster", "npc"})
                {
                    for(const pw_world_entity& entity : snapshot.world.entities)
                    {
                        if(entity.kind == kind && !entity.dead &&
                           (pick == nullptr || entity.dist < pick->dist))
                        {
                            pick = &entity;
                        }
                    }
                    if(pick != nullptr)
                    {
                        break;
                    }
                }
                session.world_action("select_target", 0.0f, 0.0f, 0.0f,
                                     pick != nullptr ? pick->id : 0);
            }
            else
            {
                session.world_action("normal_attack", 0.0f, 0.0f, 0.0f, 0);
            }
            return;
        }
        if(element->HasAttribute("data-role-id"))
        {
            const int32_t role_id = std::atoi(element->GetAttribute("data-role-id")->Get<std::string>().c_str());
            if(role_id > 0)
            {
                session.select_role(role_id);
            }
            return;
        }
        element = element->GetParentNode();
    }
}

void pw_login_ui::on_connect_clicked()
{
    rtti::context& ctx = engine::context();
    auto& mcp = ctx.get_cached<mcp_system>();
    auto& session = mcp.get_pw_session();
    if(!ui_entity_ || !ui_entity_.valid() || !ui_entity_.all_of<ui_document_component>())
    {
        return;
    }
    Rml::ElementDocument* document = ui_entity_.get<ui_document_component>().document;
    if(document == nullptr)
    {
        return;
    }
    const char* exe_env = std::getenv("PW_RUNTIME_CLIENT_EXE");
    if(exe_env == nullptr || exe_env[0] == '\0')
    {
        set_element_text(document, "pw-login-error", "PW_RUNTIME_CLIENT_EXE is not configured");
        last_error_text_.clear(); // force the next sync to re-render it
        return;
    }
    pw_session_connect_params params;
    params.executable_path = utf8_to_wide(exe_env);
    const char* workdir_env = std::getenv("PW_RUNTIME_WORKING_DIRECTORY");
    params.working_directory = utf8_to_wide(workdir_env != nullptr ? workdir_env : "");
    params.server = utf8_to_wide(get_input_value(document, "pw-server"));
    params.account_utf8 = get_input_value(document, "pw-account");
    params.password_utf8 = get_input_value(document, "pw-password");
    // The field is cleared immediately; the controller zeroes its own copy after
    // the start call. The password is never persisted in scene/project/preferences.
    set_input_value(document, "pw-password", "");
    if(params.account_utf8.empty() || params.password_utf8.empty() || params.server.empty())
    {
        params.clear_secrets();
        set_element_text(document, "pw-login-error", "Server, account and password are required");
        last_error_text_.clear();
        return;
    }
    session.connect(std::move(params));
}

void pw_login_ui::on_frame_end(rtti::context& ctx, delta_t dt)
{
    (void)dt;
    if(!mode_active_)
    {
        return;
    }
    ctx.get_cached<input_system>().manager.set_is_input_allowed(true);
    ensure_document_entity(ctx);
    sync_document(ctx);
    service_preview_proxy(ctx);
    service_world_view(ctx);
}

void pw_login_ui::sync_document(rtti::context& ctx)
{
    if(!ui_entity_ || !ui_entity_.valid() || !ui_entity_.all_of<ui_document_component>())
    {
        return;
    }
    auto& ui_comp = ui_entity_.get<ui_document_component>();
    Rml::ElementDocument* document = ui_comp.document;
    if(document == nullptr)
    {
        return; // the document loads lazily inside the render pass
    }
    if(document != attached_document_)
    {
        attach_listeners();
    }

    auto& mcp = ctx.get_cached<mcp_system>();
    const pw_session_snapshot snapshot = mcp.get_pw_session().get_snapshot();

    const char* screen = "pw-screen-login";
    switch(snapshot.state)
    {
    case pw_session_state::character_select:
    case pw_session_state::selecting_role:
    case pw_session_state::entering_world:
        screen = "pw-screen-select";
        break;
    case pw_session_state::in_world:
        screen = "pw-screen-inworld";
        break;
    default:
        break;
    }
    if(last_screen_ != screen)
    {
        show_screen(document, screen);
        last_screen_ = screen;
    }

    // --- login screen ---
    std::string status;
    switch(snapshot.state)
    {
    case pw_session_state::starting: status = "Connecting..."; break;
    case pw_session_state::authenticating: status = "Authenticating..."; break;
    case pw_session_state::role_list_loading: status = "Loading characters..."; break;
    default: break;
    }
    const auto load_status = mcp.get_login_load_status();
    if((load_status.status == "loading" || load_status.status == "waiting_assets") && load_status.total > 0)
    {
        status = "Loading login scene " + std::to_string(load_status.done) + "/" +
                 std::to_string(load_status.total);
    }
    if(status != last_status_text_)
    {
        set_element_text(document, "pw-login-status", status);
        last_status_text_ = status;
    }
    std::string error_text;
    if(snapshot.state == pw_session_state::error)
    {
        error_text = snapshot.error_code;
        if(!snapshot.error_message.empty())
        {
            error_text += ": " + snapshot.error_message;
        }
    }
    if(error_text != last_error_text_)
    {
        set_element_text(document, "pw-login-error", error_text);
        last_error_text_ = error_text;
    }
    const bool connecting = snapshot.state == pw_session_state::starting ||
        snapshot.state == pw_session_state::authenticating ||
        snapshot.state == pw_session_state::role_list_loading;
    set_element_disabled(document, "pw-connect", connecting);
    set_element_disabled(document, "pw-server", connecting);
    set_element_disabled(document, "pw-account", connecting);
    set_element_disabled(document, "pw-password", connecting);

    // --- character-select screen ---
    const uint64_t roles_signature = (static_cast<uint64_t>(snapshot.role_list_revision) << 32) |
        static_cast<uint64_t>(snapshot.roles.size());
    if(roles_signature != rendered_revision_)
    {
        std::string rml;
        for(const pw_session_role& role : snapshot.roles)
        {
            rml += "<button class=\"pw-role-item\" data-role-id=\"" + std::to_string(role.role_id) + "\">";
            rml += "<span class=\"pw-role-name\">" + xml_escape(role.name) + "</span>";
            rml += "<span class=\"pw-role-info\">" + xml_escape(role_caption(role)) + "</span>";
            rml += "</button>";
        }
        if(Rml::Element* list = document->GetElementById("pw-role-list"))
        {
            list->SetInnerRML(rml);
        }
        rendered_revision_ = roles_signature;
        rendered_selection_ = -2; // force highlight refresh
    }
    if(snapshot.selected_role_id != rendered_selection_)
    {
        if(Rml::Element* list = document->GetElementById("pw-role-list"))
        {
            for(int i = 0; i < list->GetNumChildren(); ++i)
            {
                Rml::Element* item = list->GetChild(i);
                const bool selected = item->HasAttribute("data-role-id") &&
                    std::atoi(item->GetAttribute("data-role-id")->Get<std::string>().c_str()) == snapshot.selected_role_id;
                item->SetClass("selected", selected);
            }
        }
        const pw_session_role* selected = nullptr;
        for(const pw_session_role& role : snapshot.roles)
        {
            if(role.role_id == snapshot.selected_role_id)
            {
                selected = &role;
                break;
            }
        }
        set_element_text(document, "pw-detail-name", selected != nullptr ? xml_escape(selected->name) : "-");
        set_element_text(document, "pw-detail-class",
                         selected != nullptr ? xml_escape(profession_name(selected->profession)) : "-");
        set_element_text(document, "pw-detail-level",
                         selected != nullptr ? "Уровень " + std::to_string(selected->level) : "-");
        rendered_selection_ = snapshot.selected_role_id;
    }
    // Preview data arrives asynchronously after the selection change, so it
    // syncs on its own serial rather than on the selection.
    if(snapshot.preview.serial != rendered_preview_serial_)
    {
        std::string equip = "-";
        if(snapshot.preview.serial != 0)
        {
            if(!snapshot.preview.error.empty())
            {
                equip = "Предпросмотр недоступен";
            }
            else if(snapshot.preview.equipment.empty())
            {
                equip = "Нет экипировки";
            }
            else
            {
                equip = "Экипировка: ";
                for(size_t i = 0; i < snapshot.preview.equipment.size(); ++i)
                {
                    if(i != 0)
                    {
                        equip += ", ";
                    }
                    equip += "#" + std::to_string(snapshot.preview.equipment[i].second) +
                             " (слот " + std::to_string(snapshot.preview.equipment[i].first) + ")";
                }
            }
        }
        set_element_text(document, "pw-detail-equip", equip);
        rendered_preview_serial_ = snapshot.preview.serial;
    }
    std::string select_status;
    if(snapshot.state == pw_session_state::selecting_role)
    {
        select_status = "Selecting...";
    }
    else if(snapshot.state == pw_session_state::entering_world)
    {
        select_status = "Entering world...";
    }
    set_element_text(document, "pw-select-status", select_status);
    set_element_disabled(document, "pw-start",
                         snapshot.state != pw_session_state::character_select || snapshot.selected_role_id == 0);

    // --- in-world screen ---
    if(snapshot.state == pw_session_state::in_world)
    {
        std::string info = "In world";
        for(const pw_session_role& role : snapshot.roles)
        {
            if(role.role_id == snapshot.attested_role_id)
            {
                info += " as " + role.name;
                break;
            }
        }
        info += " (role " + std::to_string(snapshot.attested_role_id) +
                ", instance " + std::to_string(snapshot.attested_instance_id) + ")";
        info += " · entities: " + std::to_string(snapshot.world.entities.size());
        info += " · pos " + std::to_string(static_cast<int>(snapshot.world.self.x)) + "," +
                std::to_string(static_cast<int>(snapshot.world.self.z));
        if(!snapshot.world.error.empty())
        {
            info += " · " + snapshot.world.error;
        }
        set_element_text(document, "pw-inworld-info", info);
    }
}

auto pw_login_ui::utf8_to_wide(const std::string& value) -> std::wstring
{
    if(value.empty())
    {
        return {};
    }
#if defined(_WIN32)
    const int required = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if(required <= 1)
    {
        return {};
    }
    std::wstring out(static_cast<size_t>(required - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, out.data(), required);
    return out;
#else
    std::wstring out;
    out.reserve(value.size());
    for(const unsigned char ch : value)
    {
        out.push_back(static_cast<wchar_t>(ch));
    }
    return out;
#endif
}

auto pw_login_ui::xml_escape(const std::string& value) -> std::string
{
    std::string out;
    out.reserve(value.size());
    for(const char ch : value)
    {
        switch(ch)
        {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default: out += ch; break;
        }
    }
    return out;
}

auto pw_login_ui::proxy_mesh_ref_for(int32_t profession, int32_t gender) -> std::string
{
    (void)profession;
    (void)gender;
    return kProxyFallbackMesh;
}

void pw_login_ui::remove_preview_proxy()
{
    if(proxy_entity_ && proxy_entity_.valid())
    {
        proxy_entity_.destroy();
    }
    proxy_entity_ = {};
    proxy_mesh_ref_.clear();
    applied_preview_serial_ = 0;
    proxy_wait_frames_ = 0;
}

void pw_login_ui::service_preview_proxy(rtti::context& ctx)
{
    auto& session = ctx.get_cached<mcp_system>().get_pw_session();
    const pw_session_snapshot snapshot = session.get_snapshot();
    if(snapshot.state != pw_session_state::character_select &&
       snapshot.state != pw_session_state::selecting_role &&
       snapshot.state != pw_session_state::entering_world &&
       snapshot.state != pw_session_state::in_world)
    {
        return;
    }
    if(snapshot.preview.serial == 0 || snapshot.preview.serial == applied_preview_serial_)
    {
        return;
    }
    update_preview_proxy(ctx, snapshot.preview);
}

void pw_login_ui::update_preview_proxy(rtti::context& ctx, const pw_session_preview& preview)
{
    auto& mcp = ctx.get_cached<mcp_system>();
    auto& am = ctx.get_cached<asset_manager>();
    const std::string desired_ref = proxy_mesh_ref_for(preview.profession, preview.gender);
    if(desired_ref != proxy_mesh_ref_)
    {
        proxy_mesh_ref_ = desired_ref;
        proxy_wait_frames_ = 0;
    }

    const std::string key = mcp.get_login_content_root() + "/" + proxy_mesh_ref_;
    auto mesh_handle = am.get_asset<mesh>(key, load_flags::standard);
    mesh_handle.submit();
    if(!mesh_handle.is_ready())
    {
        if(mesh_handle && mesh_handle.is_valid())
        {
            proxy_wait_frames_ = 0;
            return; // still streaming in
        }
        if(++proxy_wait_frames_ < kProxyAssetMaxWaitFrames)
        {
            return; // wait for the watcher to compile the asset
        }
        APPLOG_WARNING("pw preview: mesh '{}' unavailable for profession {} gender {}; keeping the previous proxy",
                       proxy_mesh_ref_, preview.profession, preview.gender);
        applied_preview_serial_ = preview.serial;
        return;
    }
    auto mesh_instance = mesh_handle.get(false);
    if(!mesh_instance || mesh_instance->get_submeshes_count(0) == 0)
    {
        APPLOG_WARNING("pw preview: mesh '{}' has no submeshes; keeping the previous proxy", proxy_mesh_ref_);
        applied_preview_serial_ = preview.serial;
        return;
    }

    // Stand = [NewChar] Pos0 from scenectrl.ini (fallback: the baked client pose),
    // grounded on the login terrain, facing the char-select (create) camera.
    const auto scene_config = mcp.get_login_scene_config();
    math::vec3 position{191.983002f, 228.391006f, 286.619995f};
    if(scene_config.loaded && !scene_config.new_char_positions.empty())
    {
        position.x = scene_config.new_char_positions[0].x;
        position.z = scene_config.new_char_positions[0].z;
    }
    float terrain_y = position.y;
    if(mcp.sample_login_terrain(position.x, position.z, terrain_y))
    {
        position.y = terrain_y;
    }
    const auto& bounds = mesh_instance->get_bounds();
    const float local_min_y = bounds.is_populated() && std::isfinite(bounds.min.y) ? bounds.min.y : 0.0f;
    position.y = position.y - local_min_y + 0.03f;

    model proxy_model;
    proxy_model.set_lod(mesh_handle, 0);
    const auto& material_uids = mesh_instance->get_default_material_uids();
    const bool tint = preview.custom_present && preview.color_body != 0;
    for(size_t i = 0; i < material_uids.size(); ++i)
    {
        auto material_handle = am.get_asset<material>(material_uids[i], load_flags::standard);
        material_handle.submit();
        auto material_instance = material_handle.get(true);
        if(material_handle)
        {
            proxy_model.set_material(material_handle, static_cast<uint32_t>(i));
        }
        if(material_instance)
        {
            if(auto pbr = std::dynamic_pointer_cast<pbr_material>(material_instance))
            {
                pbr->set_cull_type(cull_type::none);
                pbr->set_alpha_blend(false);
                pbr->set_alpha_mode(alpha_mode::opaque);
                auto color_map = pbr->get_color_map();
                color_map.submit();
                if(tint)
                {
                    // The role's own customization body color tints the proxy.
                    pbr->set_base_color(a3dcolor_to_math(preview.color_body));
                }
            }
            proxy_model.set_material_instance(material_instance, static_cast<uint32_t>(i));
        }
    }

    if(!proxy_entity_ || !proxy_entity_.valid())
    {
        auto& scene = ctx.get_cached<ecs>().get_scene();
        proxy_entity_ = scene::create_entity(*scene.registry, "PW Role Preview");
    }
    auto& transform = proxy_entity_.get<transform_component>();
    transform.set_position_local(position);
    math::vec3 camera_pos{190.304001f, 229.391006f, 284.288002f};
    if(scene_config.loaded && scene_config.cameras[kLoginSceneCreateIndex].valid)
    {
        camera_pos = scene_config.cameras[kLoginSceneCreateIndex].pos;
    }
    transform.look_at(math::vec3{camera_pos.x, position.y, camera_pos.z}, {0.0f, 1.0f, 0.0f});
    auto& model_comp = proxy_entity_.get_or_emplace<model_component>();
    model_comp.set_model(proxy_model);
    model_comp.init_armature(false);

    if(!preview.error.empty())
    {
        APPLOG_WARNING("pw preview: preview data for role {} incomplete: {}", preview.role_id, preview.error);
    }
    APPLOG_INFO("pw preview: proxy updated role={} prof={} gender={} mesh='{}' tint={}",
                preview.role_id, preview.profession, preview.gender, proxy_mesh_ref_, tint);
    applied_preview_serial_ = preview.serial;
}

void pw_login_ui::clear_world_markers()
{
    for(auto& [id, marker] : world_markers_)
    {
        if(marker.handle && marker.handle.valid())
        {
            marker.handle.destroy();
        }
    }
    world_markers_.clear();
    if(world_self_marker_ && world_self_marker_.valid())
    {
        world_self_marker_.destroy();
    }
    world_self_marker_ = {};
}

void pw_login_ui::service_world_view(rtti::context& ctx)
{
    auto& mcp = ctx.get_cached<mcp_system>();
    const pw_session_snapshot snapshot = mcp.get_pw_session().get_snapshot();
    if(snapshot.state != pw_session_state::in_world || snapshot.world.seq == 0)
    {
        if(world_epoch_ != 0 || !world_markers_.empty() || world_self_marker_)
        {
            clear_world_markers();
        }
        world_epoch_ = 0;
        applied_world_seq_ = 0;
        return;
    }
    if(snapshot.world.epoch != world_epoch_)
    {
        // Reconnect/resync: every replicated entity of the old epoch dies here.
        clear_world_markers();
        world_epoch_ = snapshot.world.epoch;
        applied_world_seq_ = 0;
    }

    auto& scene = ctx.get_cached<ecs>().get_scene();
    const pw_world_self& self = snapshot.world.self;
    const math::vec3 self_pos{self.x, self.y, self.z};

    if(!world_self_marker_ || !world_self_marker_.valid())
    {
        world_self_marker_ = scene::create_entity(*scene.registry, "PW World Self");
        apply_marker_model(ctx, world_self_marker_, math::color::white());
    }
    // Capsule origin is its center; raise by half height to stand on the point.
    world_self_marker_.get<transform_component>().set_position_global(
        self_pos + math::vec3{0.0f, 1.0f, 0.0f});

    for(const pw_world_entity& entity : snapshot.world.entities)
    {
        const uint32_t color_rgba = static_cast<uint32_t>(world_kind_color(entity));
        auto it = world_markers_.find(entity.id);
        if(it == world_markers_.end() || !it->second.handle.valid())
        {
            auto marker = scene::create_entity(
                *scene.registry, "PW " + entity.kind + " " + std::to_string(entity.id));
            apply_marker_model(ctx, marker, world_kind_color(entity));
            it = world_markers_.insert_or_assign(entity.id, world_marker{marker, color_rgba}).first;
        }
        else if(it->second.color_rgba != color_rgba)
        {
            apply_marker_model(ctx, it->second.handle, world_kind_color(entity));
            it->second.color_rgba = color_rgba;
        }
        it->second.handle.get<transform_component>().set_position_global(
            {entity.x, entity.y + 1.0f, entity.z});
    }
    for(auto it = world_markers_.begin(); it != world_markers_.end();)
    {
        const int32_t id = it->first;
        const bool alive = std::any_of(snapshot.world.entities.begin(), snapshot.world.entities.end(),
                                       [id](const pw_world_entity& entity) { return entity.id == id; });
        if(!alive)
        {
            if(it->second.handle.valid())
            {
                it->second.handle.destroy();
            }
            it = world_markers_.erase(it);
        }
        else
        {
            ++it;
        }
    }

    // Camera follows the self marker (behind and above, along the look dir).
    auto camera = mcp.ensure_camera(ctx);
    if(camera && camera.valid() && camera.all_of<transform_component>())
    {
        math::vec3 forward{self.dir_x, 0.0f, self.dir_z};
        const float length = math::length(forward);
        if(length > 1e-4f)
        {
            forward /= length;
        }
        else
        {
            forward = math::vec3{0.0f, 0.0f, 1.0f};
        }
        auto& transform = camera.get<transform_component>();
        transform.set_position_global(self_pos - forward * 7.0f + math::vec3{0.0f, 4.5f, 0.0f});
        transform.look_at(self_pos + math::vec3{0.0f, 1.2f, 0.0f}, {0.0f, 1.0f, 0.0f});
        mcp.sync_camera_to_scene_viewport(ctx);
    }
    applied_world_seq_ = snapshot.world.seq;
}
} // namespace unravel
