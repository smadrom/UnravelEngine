#pragma once

#include "../panel_base.h"

#include <base/basetypes.hpp>
#include <context/context.hpp>

namespace unravel
{
class imgui_panels;

/// Viewport panel for the Perfect World login / character-select flow. Renders
/// the login scene through the MCP camera with screen-space UI enabled (the
/// RmlUi login document is composited into the camera output) and blits the
/// result. Visible only while pw_login_ui mode is active.
class pw_login_panel : public panel_base
{
public:
    pw_login_panel(imgui_panels* parent, const char* name);

    void init(rtti::context& ctx);
    void deinit(rtti::context& ctx);

    void on_frame_render(rtti::context& ctx, delta_t dt);
    void on_frame_ui_render(rtti::context& ctx); // gates on pw_login_ui mode

protected:
    void draw_ui(rtti::context& ctx) override;
    auto get_window_flags() const -> ImGuiWindowFlags override { return ImGuiWindowFlags_None; }

private:
    imgui_panels* parent_{};
};
} // namespace unravel
