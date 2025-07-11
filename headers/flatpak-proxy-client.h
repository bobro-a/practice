#pragma once

#include <unordered_map>
#include <string>
#include <vector>

class Filter{
public:
    Filter(){

    }
    std::string name;
private:
};

class FlatpakPolicy {

}

class FlatpakProxy{
public:
    FlatpakProxy();
    ~FlatpakProxy();
private:
    bool log_messages;
//    GList         *clients;
    std::string socket_path;
    std::string dbus_address;
    bool       filter;
    bool       sloppy_names;

    std::unordered_map<std::string,std::vector<Filter*>> filters;
    void set_filter(bool filter);
    void set_sloppy_names(bool sloopy_names);
    void set_log_messages(bool log);
    void add_filter(Filter* filter);
    void add_policy(string name, bool name_is_subtree,FlatpakPolicy policy);
};

