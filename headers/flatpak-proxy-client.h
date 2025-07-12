#pragma once

#include <unordered_map>
#include <string>
#include <vector>
#include <list>
#include <glibmm.h>
#include <giomm/init.h>
#include <giomm/socketservice.h>


typedef enum {
    FILTER_TYPE_CALL = 1 << 0,
    FILTER_TYPE_BROADCAST = 1 << 1,
    FILTER_TYPE_ALL=FILTER_TYPE_CALL|FILTER_TYPE_BROADCAST,
} FilterTypeMask;

typedef enum {
    FLATPAK_POLICY_NONE,
    FLATPAK_POLICY_SEE,
    FLATPAK_POLICY_TALK,
    FLATPAK_POLICY_OWN
} FlatpakPolicy;

class Filter {
public:
    Filter(std::string name, bool name_is_subtree, FlatpakPolicy policy);

    Filter(std::string name, bool name_is_subtree, FilterTypeMask types, std::string rule);

    std::string name;
    bool name_is_subtree;
    FlatpakPolicy policy;
private:
    FilterTypeMask types;
    std::string path;
    bool path_is_subtree;
    std::string interface;
    std::string member;
};

class FlatpakProxyClient{

};

class FlatpakProxy {
public:
    FlatpakProxy(std::string dbus_address,std::string socket_path);

    ~FlatpakProxy();

private:
    bool log_messages;
    Glib::RefPtr<Gio::SocketService> parent;
    std::list<FlatpakProxyClient*> *clients;
    std::string socket_path;
    std::string dbus_address;
    bool filter;
    bool sloppy_names;

    std::unordered_map<std::string, std::vector<Filter *>> filters;

    void set_filter(bool filter);

    void set_sloppy_names(bool sloopy_names);

    void set_log_messages(bool log);

    void add_filter(Filter *filter);

    void add_policy(std::string name, bool name_is_subtree, FlatpakPolicy policy);

    void add_call_rule(std::string name, bool name_is_subtree, std::string rule);

    void add_broadcast_rule(std::string name, bool name_is_subtree, std::string rule);

};

