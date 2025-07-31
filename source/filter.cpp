#include "../headers/flatpak-proxy-client.h"

Filter::Filter(const std::string& name, bool name_is_subtree, FlatpakPolicy policy) :
    name(name),
    name_is_subtree(name_is_subtree),
    policy(policy),
    types(FILTER_TYPE_ALL),
    path_is_subtree(false) {
}

Filter::Filter(const std::string& name, bool name_is_subtree, FilterTypeMask types, const std::string& rule) :
    name(name),
    name_is_subtree(name_is_subtree),
    policy(FLATPAK_POLICY_TALK),
    types(types),
    path_is_subtree(false) {
    
    size_t at_pos = rule.find('@');
    size_t method_end = (at_pos != std::string::npos) ? at_pos : rule.size();
    
    if (at_pos != std::string::npos && at_pos + 1 < rule.size()) {
        path = rule.substr(at_pos + 1);
        if (path.ends_with("/*")) {
            path_is_subtree = true;
            path.resize(path.size() - 2);
        }
    }
    
    if (method_end > 0) {
        if (rule[0] == '*') {
        } else {
            interface = rule.substr(0, method_end);
            size_t dot = interface.rfind('.');
            if (dot != std::string::npos) {
                std::string potential_member = interface.substr(dot + 1);
                if (potential_member != "*") {
                    member = potential_member;
                }
                interface.resize(dot);
            }
        }
    }
}