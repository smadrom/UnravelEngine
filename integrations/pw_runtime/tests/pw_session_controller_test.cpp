// State-machine test driver for pw_session_controller against the fake PW
// sidecar. Exercises the full stack: controller -> pw_runtime_client ->
// PWRuntimeBridge.dll -> pw_runtime_fake_server.exe (scenario selected via a
// pw_fake_scenario.json file in the child working directory).
//
// Usage: pw_session_controller_test --script <path> [--trace <path>]
// Script DSL (one step per line, '#' comments):
//   connect exe=<path> workdir=<path> server=<s> account=<a> password=<p> [connect_ms=<n>] [enter_ms=<n>]
//   wait_state <name> <timeout_ms>
//   assert_state <name> | assert_not_state <name>
//   assert_error <code>
//   assert_revision <n> | assert_roles <count> | assert_selection <role_id|0>
//   assert_capability <0|1> | assert_camera <none|login|selchar|choose>
//   assert_attested <role_id> <instance_id>
//   wait_preview <role_id> <timeout_ms>
//   assert_preview <role_id> <profession> <gender> | assert_preview_none
//   assert_preview_equipment <count> | assert_preview_error <substr>
//   wait_world <timeout_ms> | assert_world_none
//   wait_world_entities <min_count> <timeout_ms>
//   assert_world_self <role_id> <instance_id> | assert_world_entities <count>
//   assert_world_kind <kind>
//   world_action <action> [x=<f> y=<f> z=<f> | id=<n>] [expect_reject]
//   assert_world_no_error | assert_world_error <substr>
//   select <role_id> [expect_reject] | enter [expect_reject] | refresh [expect_reject]
//   cancel [expect_reject] | disconnect [expect_reject]
//   sleep <ms>
// Exit 0 = all steps passed; non-zero = first failing step.

#include <editor/mcp/pw_runtime_client.h>
#include <editor/mcp/pw_session_controller.h>

#include <editor/mcp/json.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using json = nlohmann::json;
using namespace unravel;

namespace
{
struct step
{
    int line_no = 0;
    std::string action;
    std::vector<std::string> positional;
    std::map<std::string, std::string> kv;
    std::string raw;
};

auto parse_script(const std::string& path, std::vector<step>& steps, std::string& error) -> bool
{
    std::ifstream in(path);
    if(!in)
    {
        error = "cannot open script: " + path;
        return false;
    }
    std::string line;
    int line_no = 0;
    while(std::getline(in, line))
    {
        ++line_no;
        const size_t comment = line.find('#');
        if(comment != std::string::npos)
        {
            line.erase(comment);
        }
        std::istringstream tokens(line);
        std::string token;
        std::vector<std::string> parts;
        while(tokens >> token)
        {
            parts.push_back(token);
        }
        if(parts.empty())
        {
            continue;
        }
        step s;
        s.line_no = line_no;
        s.raw = line;
        s.action = parts[0];
        for(size_t i = 1; i < parts.size(); ++i)
        {
            const size_t eq = parts[i].find('=');
            if(eq != std::string::npos)
            {
                s.kv[parts[i].substr(0, eq)] = parts[i].substr(eq + 1);
            }
            else
            {
                s.positional.push_back(parts[i]);
            }
        }
        steps.push_back(std::move(s));
    }
    return true;
}

auto utf8_to_wide(const std::string& value) -> std::wstring
{
    std::wstring out;
    out.reserve(value.size());
    for(const unsigned char ch : value)
    {
        out.push_back(static_cast<wchar_t>(ch));
    }
    return out;
}

auto state_by_name(const std::string& name, pw_session_state& out) -> bool
{
    static const std::pair<const char*, pw_session_state> table[] = {
        {"idle", pw_session_state::idle},
        {"starting", pw_session_state::starting},
        {"authenticating", pw_session_state::authenticating},
        {"role_list_loading", pw_session_state::role_list_loading},
        {"character_select", pw_session_state::character_select},
        {"selecting_role", pw_session_state::selecting_role},
        {"entering_world", pw_session_state::entering_world},
        {"in_world", pw_session_state::in_world},
        {"error", pw_session_state::error},
    };
    for(const auto& entry : table)
    {
        if(name == entry.first)
        {
            out = entry.second;
            return true;
        }
    }
    return false;
}

auto camera_by_name(const std::string& name, pw_session_camera& out) -> bool
{
    static const std::pair<const char*, pw_session_camera> table[] = {
        {"none", pw_session_camera::none},
        {"login", pw_session_camera::login},
        {"selchar", pw_session_camera::selchar},
        {"choose", pw_session_camera::choose},
    };
    for(const auto& entry : table)
    {
        if(name == entry.first)
        {
            out = entry.second;
            return true;
        }
    }
    return false;
}

struct tracer
{
    pw_session_snapshot last{};
    bool first = true;
    std::ofstream file;
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();

    void observe(const pw_session_snapshot& snapshot)
    {
        const bool changed = first || snapshot.state != last.state ||
            snapshot.role_list_revision != last.role_list_revision ||
            snapshot.roles.size() != last.roles.size() ||
            snapshot.selected_role_id != last.selected_role_id ||
            snapshot.desired_camera != last.desired_camera ||
            snapshot.mutation_capable != last.mutation_capable ||
            snapshot.error_code != last.error_code ||
            snapshot.attested_role_id != last.attested_role_id ||
            snapshot.attested_instance_id != last.attested_instance_id;
        if(!changed)
        {
            return;
        }
        first = false;
        last = snapshot;
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
        json record = {
            {"t", ms},
            {"state", pw_session_state_name(snapshot.state)},
            {"revision", snapshot.role_list_revision},
            {"roles", snapshot.roles.size()},
            {"selected", snapshot.selected_role_id},
            {"camera", pw_session_camera_name(snapshot.desired_camera)},
            {"capable", snapshot.mutation_capable ? 1 : 0},
            {"error", snapshot.error_code},
            {"attestedRoleId", snapshot.attested_role_id},
            {"attestedInstanceId", snapshot.attested_instance_id},
        };
        const std::string line = record.dump();
        std::cout << line << std::endl;
        if(file)
        {
            file << line << "\n" << std::flush;
        }
    }
};

auto fail(const step& s, const std::string& reason) -> int
{
    std::cerr << "FAIL line " << s.line_no << " (" << s.action << "): " << reason << std::endl;
    return 1;
}

auto has_flag(const step& s, const std::string& flag) -> bool
{
    for(const auto& token : s.positional)
    {
        if(token == flag)
        {
            return true;
        }
    }
    return false;
}
} // namespace

int main(int argc, char** argv)
{
    std::string script_path;
    std::string trace_path;
    for(int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if(arg == "--script" && i + 1 < argc)
        {
            script_path = argv[++i];
        }
        else if(arg == "--trace" && i + 1 < argc)
        {
            trace_path = argv[++i];
        }
    }
    if(script_path.empty())
    {
        std::cerr << "usage: pw_session_controller_test --script <path> [--trace <path>]" << std::endl;
        return 2;
    }
    std::vector<step> steps;
    std::string error;
    if(!parse_script(script_path, steps, error))
    {
        std::cerr << error << std::endl;
        return 2;
    }

    pw_runtime_client runtime;
    pw_session_controller controller(runtime);
    controller.start();

    tracer trace;
    if(!trace_path.empty())
    {
        trace.file.open(trace_path, std::ios::trunc);
    }
    trace.observe(controller.get_snapshot());

    auto wait_state = [&](pw_session_state target, uint32_t timeout_ms) -> bool
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while(std::chrono::steady_clock::now() < deadline)
        {
            const pw_session_snapshot snapshot = controller.get_snapshot();
            trace.observe(snapshot);
            if(snapshot.state == target)
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    };

    int exit_code = 0;
    for(const step& s : steps)
    {
        trace.observe(controller.get_snapshot());
        if(s.action == "connect")
        {
            pw_session_connect_params params;
            params.executable_path = utf8_to_wide(s.kv.count("exe") ? s.kv.at("exe") : "");
            params.working_directory = utf8_to_wide(s.kv.count("workdir") ? s.kv.at("workdir") : "");
            params.server = utf8_to_wide(s.kv.count("server") ? s.kv.at("server") : "");
            params.account_utf8 = s.kv.count("account") ? s.kv.at("account") : "";
            params.password_utf8 = s.kv.count("password") ? s.kv.at("password") : "";
            if(s.kv.count("connect_ms"))
            {
                params.connect_deadline_ms = static_cast<uint32_t>(std::stoul(s.kv.at("connect_ms")));
            }
            if(s.kv.count("enter_ms"))
            {
                params.enter_deadline_ms = static_cast<uint32_t>(std::stoul(s.kv.at("enter_ms")));
            }
            if(!controller.connect(std::move(params)))
            {
                exit_code = fail(s, "connect intent rejected");
                break;
            }
        }
        else if(s.action == "wait_state")
        {
            if(s.positional.size() < 2)
            {
                exit_code = fail(s, "usage: wait_state <name> <timeout_ms>");
                break;
            }
            pw_session_state target;
            if(!state_by_name(s.positional[0], target))
            {
                exit_code = fail(s, "unknown state " + s.positional[0]);
                break;
            }
            const uint32_t timeout_ms = static_cast<uint32_t>(std::stoul(s.positional[1]));
            if(!wait_state(target, timeout_ms))
            {
                const pw_session_snapshot snapshot = controller.get_snapshot();
                exit_code = fail(s, "timed out waiting for " + s.positional[0] +
                                    "; current=" + pw_session_state_name(snapshot.state) +
                                    " error=" + snapshot.error_code);
                break;
            }
        }
        else if(s.action == "assert_state" || s.action == "assert_not_state")
        {
            pw_session_state target;
            if(s.positional.empty() || !state_by_name(s.positional[0], target))
            {
                exit_code = fail(s, "unknown state");
                break;
            }
            const pw_session_snapshot snapshot = controller.get_snapshot();
            trace.observe(snapshot);
            const bool matches = snapshot.state == target;
            if((s.action == "assert_state") != matches)
            {
                exit_code = fail(s, "state=" + std::string(pw_session_state_name(snapshot.state)) +
                                    " expected" + (s.action == "assert_state" ? " " : " not ") + s.positional[0]);
                break;
            }
        }
        else if(s.action == "assert_error")
        {
            const pw_session_snapshot snapshot = controller.get_snapshot();
            trace.observe(snapshot);
            if(snapshot.state != pw_session_state::error || snapshot.error_code != s.positional.at(0))
            {
                exit_code = fail(s, "state=" + std::string(pw_session_state_name(snapshot.state)) +
                                    " error_code=" + snapshot.error_code + " expected " + s.positional.at(0));
                break;
            }
        }
        else if(s.action == "assert_revision" || s.action == "assert_roles" ||
                s.action == "assert_selection" || s.action == "assert_capability")
        {
            const pw_session_snapshot snapshot = controller.get_snapshot();
            trace.observe(snapshot);
            const long expected = std::stol(s.positional.at(0));
            long actual = 0;
            if(s.action == "assert_revision") actual = static_cast<long>(snapshot.role_list_revision);
            if(s.action == "assert_roles") actual = static_cast<long>(snapshot.roles.size());
            if(s.action == "assert_selection") actual = snapshot.selected_role_id;
            if(s.action == "assert_capability") actual = snapshot.mutation_capable ? 1 : 0;
            if(actual != expected)
            {
                exit_code = fail(s, s.action + " actual=" + std::to_string(actual) +
                                    " expected=" + std::to_string(expected));
                break;
            }
        }
        else if(s.action == "assert_camera")
        {
            pw_session_camera expected;
            if(s.positional.empty() || !camera_by_name(s.positional[0], expected))
            {
                exit_code = fail(s, "unknown camera");
                break;
            }
            const pw_session_snapshot snapshot = controller.get_snapshot();
            trace.observe(snapshot);
            if(snapshot.desired_camera != expected)
            {
                exit_code = fail(s, "camera=" + std::string(pw_session_camera_name(snapshot.desired_camera)) +
                                    " expected " + s.positional[0]);
                break;
            }
        }
        else if(s.action == "assert_attested")
        {
            const pw_session_snapshot snapshot = controller.get_snapshot();
            trace.observe(snapshot);
            if(s.positional.size() < 2 ||
               snapshot.attested_role_id != std::stol(s.positional[0]) ||
               snapshot.attested_instance_id != std::stol(s.positional[1]))
            {
                exit_code = fail(s, "attested role=" + std::to_string(snapshot.attested_role_id) +
                                    " instance=" + std::to_string(snapshot.attested_instance_id));
                break;
            }
        }
        else if(s.action == "wait_preview")
        {
            if(s.positional.size() < 2)
            {
                exit_code = fail(s, "usage: wait_preview <role_id> <timeout_ms>");
                break;
            }
            const long role_id = std::stol(s.positional[0]);
            const uint32_t timeout_ms = static_cast<uint32_t>(std::stoul(s.positional[1]));
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
            bool arrived = false;
            while(std::chrono::steady_clock::now() < deadline)
            {
                const pw_session_snapshot snapshot = controller.get_snapshot();
                trace.observe(snapshot);
                if(snapshot.preview.serial != 0 && snapshot.preview.role_id == role_id)
                {
                    arrived = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if(!arrived)
            {
                const pw_session_snapshot snapshot = controller.get_snapshot();
                exit_code = fail(s, "timed out waiting for preview of " + std::to_string(role_id) +
                                    "; preview.role_id=" + std::to_string(snapshot.preview.role_id) +
                                    " serial=" + std::to_string(snapshot.preview.serial));
                break;
            }
        }
        else if(s.action == "assert_preview")
        {
            const pw_session_snapshot snapshot = controller.get_snapshot();
            trace.observe(snapshot);
            if(s.positional.size() < 3 || snapshot.preview.serial == 0 ||
               snapshot.preview.role_id != std::stol(s.positional[0]) ||
               snapshot.preview.profession != std::stol(s.positional[1]) ||
               snapshot.preview.gender != std::stol(s.positional[2]))
            {
                exit_code = fail(s, "preview role=" + std::to_string(snapshot.preview.role_id) +
                                    " prof=" + std::to_string(snapshot.preview.profession) +
                                    " gender=" + std::to_string(snapshot.preview.gender) +
                                    " serial=" + std::to_string(snapshot.preview.serial));
                break;
            }
        }
        else if(s.action == "assert_preview_equipment")
        {
            const pw_session_snapshot snapshot = controller.get_snapshot();
            trace.observe(snapshot);
            const long expected = std::stol(s.positional.at(0));
            if(static_cast<long>(snapshot.preview.equipment.size()) != expected)
            {
                exit_code = fail(s, "preview equipment=" +
                                    std::to_string(snapshot.preview.equipment.size()) +
                                    " expected=" + std::to_string(expected));
                break;
            }
        }
        else if(s.action == "assert_preview_error")
        {
            const pw_session_snapshot snapshot = controller.get_snapshot();
            trace.observe(snapshot);
            if(s.positional.empty() || snapshot.preview.serial == 0 ||
               snapshot.preview.error.find(s.positional[0]) == std::string::npos)
            {
                exit_code = fail(s, "preview error='" + snapshot.preview.error +
                                    "' serial=" + std::to_string(snapshot.preview.serial) +
                                    " expected substring '" + s.positional.at(0) + "'");
                break;
            }
        }
        else if(s.action == "assert_preview_none")
        {
            const pw_session_snapshot snapshot = controller.get_snapshot();
            trace.observe(snapshot);
            if(snapshot.preview.serial != 0)
            {
                exit_code = fail(s, "preview serial=" + std::to_string(snapshot.preview.serial) +
                                    " expected none");
                break;
            }
        }
        else if(s.action == "wait_world")
        {
            if(s.positional.empty())
            {
                exit_code = fail(s, "usage: wait_world <timeout_ms>");
                break;
            }
            const uint32_t timeout_ms = static_cast<uint32_t>(std::stoul(s.positional[0]));
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
            bool arrived = false;
            while(std::chrono::steady_clock::now() < deadline)
            {
                const pw_session_snapshot snapshot = controller.get_snapshot();
                trace.observe(snapshot);
                if(snapshot.world.seq != 0)
                {
                    arrived = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if(!arrived)
            {
                exit_code = fail(s, "timed out waiting for the world stream");
                break;
            }
        }
        else if(s.action == "wait_world_entities")
        {
            if(s.positional.size() < 2)
            {
                exit_code = fail(s, "usage: wait_world_entities <min_count> <timeout_ms>");
                break;
            }
            const long min_count = std::stol(s.positional[0]);
            const uint32_t timeout_ms = static_cast<uint32_t>(std::stoul(s.positional[1]));
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
            bool arrived = false;
            while(std::chrono::steady_clock::now() < deadline)
            {
                const pw_session_snapshot snapshot = controller.get_snapshot();
                trace.observe(snapshot);
                if(static_cast<long>(snapshot.world.entities.size()) >= min_count)
                {
                    arrived = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if(!arrived)
            {
                const pw_session_snapshot snapshot = controller.get_snapshot();
                exit_code = fail(s, "timed out waiting for world entities; have " +
                                    std::to_string(snapshot.world.entities.size()) +
                                    " error=" + snapshot.world.error);
                break;
            }
        }
        else if(s.action == "assert_world_self")
        {
            const pw_session_snapshot snapshot = controller.get_snapshot();
            trace.observe(snapshot);
            if(s.positional.size() < 2 || snapshot.world.seq == 0 ||
               snapshot.world.self.role_id != std::stol(s.positional[0]) ||
               snapshot.world.self.instance_id != std::stol(s.positional[1]))
            {
                exit_code = fail(s, "world self role=" + std::to_string(snapshot.world.self.role_id) +
                                    " instance=" + std::to_string(snapshot.world.self.instance_id) +
                                    " seq=" + std::to_string(snapshot.world.seq));
                break;
            }
        }
        else if(s.action == "assert_world_entities")
        {
            const pw_session_snapshot snapshot = controller.get_snapshot();
            trace.observe(snapshot);
            const long expected = std::stol(s.positional.at(0));
            if(static_cast<long>(snapshot.world.entities.size()) != expected)
            {
                exit_code = fail(s, "world entities=" +
                                    std::to_string(snapshot.world.entities.size()) +
                                    " expected=" + std::to_string(expected) +
                                    " error=" + snapshot.world.error);
                break;
            }
        }
        else if(s.action == "assert_world_kind")
        {
            const pw_session_snapshot snapshot = controller.get_snapshot();
            trace.observe(snapshot);
            bool found = false;
            for(const pw_world_entity& entity : snapshot.world.entities)
            {
                if(entity.kind == s.positional.at(0))
                {
                    found = true;
                    break;
                }
            }
            if(!found)
            {
                exit_code = fail(s, "no world entity of kind " + s.positional.at(0));
                break;
            }
        }
        else if(s.action == "assert_world_none")
        {
            const pw_session_snapshot snapshot = controller.get_snapshot();
            trace.observe(snapshot);
            if(snapshot.world.seq != 0 || !snapshot.world.entities.empty())
            {
                exit_code = fail(s, "world seq=" + std::to_string(snapshot.world.seq) +
                                    " entities=" + std::to_string(snapshot.world.entities.size()) +
                                    " expected none");
                break;
            }
        }
        else if(s.action == "world_action")
        {
            if(s.positional.empty())
            {
                exit_code = fail(s, "usage: world_action <action> [x=<f> y=<f> z=<f> | id=<n>] [expect_reject]");
                break;
            }
            float x = 0.0f;
            float y = 0.0f;
            float z = 0.0f;
            int32_t id = 0;
            if(s.kv.count("x")) x = std::stof(s.kv.at("x"));
            if(s.kv.count("y")) y = std::stof(s.kv.at("y"));
            if(s.kv.count("z")) z = std::stof(s.kv.at("z"));
            if(s.kv.count("id")) id = std::stol(s.kv.at("id"));
            const bool accepted = controller.world_action(s.positional[0], x, y, z, id);
            if(accepted == has_flag(s, "expect_reject"))
            {
                exit_code = fail(s, has_flag(s, "expect_reject")
                                        ? "world_action expected rejection but was accepted"
                                        : "world_action intent rejected");
                break;
            }
            trace.observe(controller.get_snapshot());
        }
        else if(s.action == "assert_world_no_error" || s.action == "assert_world_error")
        {
            const pw_session_snapshot snapshot = controller.get_snapshot();
            trace.observe(snapshot);
            if(s.action == "assert_world_no_error" && !snapshot.world.error.empty())
            {
                exit_code = fail(s, "world error='" + snapshot.world.error + "' expected none");
                break;
            }
            if(s.action == "assert_world_error" &&
               (s.positional.empty() ||
                snapshot.world.error.find(s.positional[0]) == std::string::npos))
            {
                exit_code = fail(s, "world error='" + snapshot.world.error + "' expected substring '" +
                                    (s.positional.empty() ? std::string() : s.positional[0]) + "'");
                break;
            }
        }
        else if(s.action == "select" || s.action == "enter" || s.action == "refresh" ||
                s.action == "cancel" || s.action == "disconnect")
        {
            const bool expect_reject = has_flag(s, "expect_reject");
            bool accepted = false;
            if(s.action == "select")
            {
                if(s.positional.empty())
                {
                    exit_code = fail(s, "usage: select <role_id> [expect_reject]");
                    break;
                }
                accepted = controller.select_role(std::stol(s.positional[0]));
            }
            else if(s.action == "enter")
            {
                accepted = controller.enter_selected_role();
            }
            else if(s.action == "refresh")
            {
                accepted = controller.refresh_roles();
            }
            else if(s.action == "cancel")
            {
                accepted = controller.cancel();
            }
            else
            {
                accepted = controller.disconnect();
            }
            if(accepted == expect_reject)
            {
                exit_code = fail(s, expect_reject ? "expected rejection but was accepted"
                                                  : "intent rejected");
                break;
            }
            trace.observe(controller.get_snapshot());
        }
        else if(s.action == "sleep")
        {
            const uint32_t ms = static_cast<uint32_t>(std::stoul(s.positional.at(0)));
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
            while(std::chrono::steady_clock::now() < deadline)
            {
                trace.observe(controller.get_snapshot());
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        else
        {
            exit_code = fail(s, "unknown action");
            break;
        }
    }

    trace.observe(controller.get_snapshot());
    controller.shutdown();
    runtime.stop();
    runtime.deinit();
    if(exit_code == 0)
    {
        std::cout << "PASS" << std::endl;
    }
    return exit_code;
}
