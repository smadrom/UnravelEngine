#include "pw_login_panel.h"

#include <editor/pwlogin/pw_login_ui.h>
#include "imgui/imgui.h"
#include "imgui_widgets/utils.h"

#include <editor/mcp/mcp_system.h>
#include <engine/ecs/ecs.h>
#include <engine/ecs/components/transform_component.h>
#include <engine/input/input.h>
#include <engine/rendering/ecs/components/camera_component.h>
#include <engine/rendering/ecs/systems/rendering_system.h>

namespace unravel
{
pw_login_panel::pw_login_panel(imgui_panels* parent, const char* name)
    : panel_base(name)
    , parent_(parent)
{
}

void pw_login_panel::init(rtti::context& ctx)
{
    (void)ctx;
}

void pw_login_panel::deinit(rtti::context& ctx)
{
    (void)ctx;
}

void pw_login_panel::on_frame_ui_render(rtti::context& ctx)
{
    auto& login_ui = ctx.get_cached<pw_login_ui>();
    if(!login_ui.is_active())
    {
        is_visible_ = false;
        return;
    }
    panel_base::on_frame_ui_render(ctx);
}

void pw_login_panel::on_frame_render(rtti::context& ctx, delta_t dt)
{
    auto& login_ui = ctx.get_cached<pw_login_ui>();
    if(!login_ui.is_active() || !is_visible())
    {
        return;
    }
    auto& mcp = ctx.get_cached<mcp_system>();
    auto camera = mcp.ensure_camera(ctx);
    if(!camera || !camera.all_of<transform_component, camera_component>())
    {
        return;
    }
    auto& scene = ctx.get_cached<ecs>().get_scene();
    // Screen-space enabled: the RmlUi login document is composited into this
    // camera's output buffer.
    ctx.get_cached<rendering_system>().render_scene(
        camera, camera.get<camera_component>(), scene, dt, true);
}

void pw_login_panel::draw_ui(rtti::context& ctx)
{
    auto& mcp = ctx.get_cached<mcp_system>();
    auto camera = mcp.ensure_camera(ctx);
    if(!camera || !camera.all_of<camera_component>())
    {
        ImGui::TextUnformatted("PW login camera unavailable");
        return;
    }
    const auto& camera_comp = camera.get<camera_component>();
    const auto& render_view = camera_comp.get_render_view();
    const auto& obuffer = render_view.fbo_safe_get("OBUFFER");
    if(!obuffer)
    {
        ImGui::TextUnformatted("Loading...");
        return;
    }
    auto texture = obuffer->get_texture(0);
    const auto texture_size = obuffer->get_size();
    const ImVec2 tex_size_v(static_cast<float>(texture_size.width), static_cast<float>(texture_size.height));
    const ImVec2 size = ImGui::GetContentRegionAvail();
    if(size.x <= 0 || size.y <= 0)
    {
        return;
    }
    ImGui::ImageWithAspect(ImGui::ToId(texture), tex_size_v, size, ImVec2(0.5f, 0.5f));

    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    input::zone work_zone{};
    work_zone.x = min.x;
    work_zone.y = min.y;
    work_zone.w = max.x - min.x;
    work_zone.h = max.y - min.y;
    ctx.get_cached<input_system>().manager.set_work_zone(work_zone);
    ctx.get_cached<input_system>().manager.set_reference_size({tex_size_v.x, tex_size_v.y});
}
} // namespace unravel
