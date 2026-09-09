#include "script_glue_common.h"
#include "script_bindings.h"
#include "../script_interop.h"

#include <engine/pw/pw_map_loader.h>

namespace unravel
{
namespace
{
//-------------------------------------------------
// PW map queries for gameplay scripts. The loaded PW map owns a heightfield that has no
// physics collider, so scripts sample it directly to stand a character on the ground.
//-------------------------------------------------
auto internal_m2n_pw_map_has_terrain() -> bool
{
    auto& ctx = engine::context();
    return ctx.get_cached<pw_map_loader>().has_login_terrain();
}

auto internal_m2n_pw_map_sample_terrain(float world_x, float world_z, float* out_height) -> bool
{
    auto& ctx = engine::context();
    const auto& loader = ctx.get_cached<pw_map_loader>();
    float height = 0.0f;
    if(!loader.has_login_terrain() || !loader.sample_login_terrain(world_x, world_z, height)) return false;
    if(out_height) *out_height = height;
    return true;
}
} // namespace

void register_pw_map_script_bindings()
{
    APPLOG_TRACE("{}", __func__);
    auto reg = dotnet::internal_call_registry("Unravel.Core.PwMap");
    reg.add_internal_call("internal_m2n_pw_map_has_terrain", dotnet_internal_call(internal_m2n_pw_map_has_terrain));
    reg.add_internal_call("internal_m2n_pw_map_sample_terrain",
                          dotnet_internal_call(internal_m2n_pw_map_sample_terrain));
}
} // namespace unravel
