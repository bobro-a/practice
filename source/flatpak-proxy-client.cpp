#include "headers/flatpak-proxy-client.h"

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
    this->filters[filter.name].push_back(filter);
}

void
FlatpakProxy::add_policy(string name,
                         bool name_is_subtree,
                         FlatpakPolicy policy) {

}
