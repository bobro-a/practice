#include "headers/flatpak-proxy-client.h"
#include <cassert>

void FlatpakProxy::set_filter(bool filter) {
    this->filter = filter;
}

void FlatpakProxy::set_sloppy_names(bool sloopy_names) {
    this->sloppy_names = sloopy_names;
}

void FlatpakProxy::set_log_messages(bool log) {
    this->log_messages = log;
}

void FlatpakProxy::add_filter(Filter *filter) {
    this->filters[filter->name].push_back(filter);
}

void
FlatpakProxy::add_policy(std::string name,
                         bool name_is_subtree,
                         FlatpakPolicy policy) {
    Filter *filter = new Filter(name, name_is_subtree, policy); //todo clear
    this->add_filter(filter);
}


void FlatpakProxy::add_call_rule(std::string name, bool name_is_subtree, std::string rule) {
    Filter *filter = new Filter(name, name_is_subtree, FILTER_TYPE_CALL, rule);//todo clear
    this->add_filter(filter);
}


void FlatpakProxy::add_broadcast_rule(std::string name, bool name_is_subtree, std::string rule) {
    Filter *filter = new Filter(name, name_is_subtree, FILTER_TYPE_BROADCAST, rule);//todo clear
    this->add_filter(filter);
}

FlatpakProxy::~FlatpakProxy() {
    assert(clients->empty());
    filters.clear();
    //todo will finish
}
FlatpakProxy::FlatpakProxy(std::string dbus_address,std::string socket_path){

}

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

