#include "contract_validation.hpp"
#include "validation/wire_rules.hpp"
#include <jsoncons/json.hpp>
#include <jsoncons/json_cursor.hpp>
#include <string>
#include <set>
#include <vector>

namespace roc::vehicle::detail {
namespace {
using Json = jsoncons::json;
}
bool valid_task_contract(std::string_view body, std::string_view definition) {
    try {
        return wire::task(Json::parse(body), definition);
    } catch (const std::exception&) { return false; }
}
bool valid_device_contract(std::string_view body, std::string_view definition) {
    try {
        return wire::device(Json::parse(body), definition);
    } catch (const std::exception&) { return false; }
}
bool valid_road_contract(std::string_view body, int version) {
    try {
        const auto document=Json::parse(body,jsoncons::json_options{}.max_nesting_depth(32));
        return wire::road(document, version);
    } catch (const std::exception&) { return false; }
}
bool wire_rules_self_test() {
    return valid_device_contract(R"({"library_version":"0.2.1-rc2"})", "heartbeatPayload") &&
        !valid_device_contract(R"({"library_version":"bad version"})", "heartbeatPayload") &&
        valid_road_contract(R"({"schema_version":1,"coordinate_mode":"metric","nodes":[],"edges":[]})",1) &&
        !valid_road_contract(R"({"schema_version":3,"coordinate_mode":"metric","nodes":[],"edges":[]})",1);
}
bool bounded_road_document(std::string_view body, std::size_t maximum_bytes) {
    if (body.empty() || body.size()>maximum_bytes) return false;
    try {
        struct Frame {
            bool array{false}; std::string name; std::size_t count{0};
            std::set<std::string> keys;
        };
        std::vector<Frame> stack;
        std::size_t total_points=0;
        jsoncons::json_string_cursor cursor(body,jsoncons::json_options{}.max_nesting_depth(32));
        using Event=jsoncons::staj_event_type;
        for (; !cursor.done(); cursor.next()) {
            const auto type=cursor.current().event_type();
            if (type==Event::key) {
                if (stack.empty() || stack.back().array) return false;
                auto key=cursor.current().get<std::string>();
                if (key.size()>64 || !stack.back().keys.insert(key).second ||
                    stack.back().keys.size()>16) return false;
                stack.back().name=std::move(key);
            } else if (type==Event::end_object || type==Event::end_array) {
                if (stack.empty()) return false;
                stack.pop_back();
            } else {
                if (!stack.empty() && stack.back().array) {
                    auto& frame=stack.back(); ++frame.count;
                    const auto limit=frame.name=="nodes" ? 10000U : frame.name=="edges" ? 50000U :
                        frame.name=="trajectories" ? 100000U : frame.name=="points" ? 10000U : 200000U;
                    if (frame.count>limit || (frame.name=="points" && ++total_points>200000U)) return false;
                }
                if (type==Event::begin_object || type==Event::begin_array) {
                    auto name=stack.empty() ? std::string{} : stack.back().name;
                    stack.push_back({type==Event::begin_array,std::move(name),0,{}});
                    if (stack.size()>32) return false;
                }
            }
        }
        return stack.empty();
    } catch (const std::exception&) { return false; }
}
}
