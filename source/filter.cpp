#include "headers/flatpak-proxy-client.h"

Filter::Filter(std::string name, bool name_is_subtree, FlatpakPolicy policy) {
    this->name = name;
    this->name_is_subtree = name_is_subtree;
    this->policy = policy;
    this->types = FILTER_TYPE_ALL;
}

Filter::Filter(std::string name, bool name_is_subtree, FilterTypeMask types, std::string rule) {
    this->name = name;
    this->name_is_subtree = name_is_subtree;
    this->policy = FLATPAK_POLICY_TALK;
    this->types = types;
    size_t pos = 0;
    pos = rule.find('@');
    if (pos != std::string::npos && pos + 1 < rule.size()) {
        this->path = rule.substr(pos + 1);
        if (this->path.ends_with("/*")) {
            this->path_is_subtree = true;
            this->path[this->path.size() - 2] = 0;
        }
    }
    size_t method_end = (pos == std::string::npos ? pos : rule.size());
    if (method_end) {
        if (rule[0] == '*') {

        } else {
            this->interface = rule.substr(0, method_end);
            size_t dot = this->interface.rfind('.');
            if (dot != std::string::npos) {
                std::string member = this->interface.substr(dot + 1);
                if (member != "*") {
                    this->member = member;
                }
                this->interface.resize(dot);
            }
        }
    }
}