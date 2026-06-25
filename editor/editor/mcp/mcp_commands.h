#pragma once

#include <string>

namespace rtti
{
class context;
}

namespace unravel
{
class mcp_system;

namespace mcp_commands
{
auto dispatch(rtti::context& ctx, const std::string& req_json, mcp_system& sys) -> std::string;
}

} // namespace unravel
