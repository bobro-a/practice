#include <vector>
#include <fstream>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <glibmm.h>

#include "flatpak-proxy.h"


static const char *argv0;
static std::list<std::shared_ptr<FlatpakProxy>> proxies;
static int sync_fd = -1;

static void
usage(int ecode, std::ostream *out) {
    *out << "usage: " << argv0 << " [OPTIONS...] [ADDRESS PATH [OPTIONS...] ...]\n\n";
    *out << "Options:\n"
            "    --help                       Print this help\n"
            "    --version                    Print version\n"
            "    --fd=FD                      Stop when FD is closed\n"
            "    --args=FD                    Read arguments from FD\n\n"
            "Proxy Options:\n"
            "    --filter                     Enable filtering\n"
            "    --log                        Turn on logging\n"
            "    --sloppy-names               Report name changes for unique names\n"
            "    --see=NAME                   Set 'see' policy for NAME\n"
            "    --talk=NAME                  Set 'talk' policy for NAME\n"
            "    --own=NAME                   Set 'own' policy for NAME\n"
            "    --call=NAME=RULE             Set RULE for calls on NAME\n"
            "    --broadcast=NAME=RULE        Set RULE for broadcasts from NAME\n";
    exit(ecode);
}

static bool
start_proxy(std::vector<std::string> &args, size_t &args_i) {
    if (args_i >= args.size() || args[args_i][0] == '-') {
        std::cerr << "No bus address given\n";
        return false;
    }

    std::string bus_address = args[args_i++];

    if (args_i >= args.size() || args[args_i][0] == '-') {
        std::cerr << "No socket path given\n";
        return FALSE;
    }

    std::string socket_path = args[args_i++];

    FlatpakProxy *proxy = flatpak_proxy_new(bus_address.c_str(), socket_path.c_str());

    while (args_i < args.size()) {
        std::string temp_arg = args[args_i];

        if (temp_arg[0] != '-')
            break;

        if (temp_arg.starts_with("--see=") ||
            temp_arg.starts_with("--talk=") ||
            temp_arg.starts_with("--own=")) {
            FlatpakPolicy policy = FLATPAK_POLICY_SEE;
            std::string name = temp_arg.substr(temp_arg.find('=') + 1);
            bool wildcard = false;

            if (temp_arg[2] == 't')
                policy = FLATPAK_POLICY_TALK;
            else if (temp_arg[2] == 'o')
                policy = FLATPAK_POLICY_OWN;

            if (name.ends_with(".*")) {
                name[name.size() - 2] = 0;
                wildcard = true;
            }

            if (name[0] == ':' || !g_dbus_is_name(name.c_str())) {
                std::cerr << "'" << name << "' is not a valid dbus name\n";
                return false;
            }

            flatpak_proxy_add_policy(proxy, name.c_str(), wildcard, policy);
            ++args_i;
        } else if (temp_arg.starts_with("--call=") ||
                   temp_arg.starts_with("--broadcast=")) {
            std::string rest = temp_arg.substr(temp_arg.find('=') + 1);
            size_t name_end = rest.find('=');
            bool wildcard = false;

            if (name_end == std::string::npos) {
                std::cerr << "'" << rest << "' is not a valid name + rule\n";
                return false;
            }

            std::string name = rest.substr(0, name_end);
            std::string rule = rest.substr(name_end + 1);

            if (name.ends_with(".*")) {
                name[name.size() - 2] = 0;
                wildcard = true;
            }

            if (temp_arg.starts_with("--call="))
                flatpak_proxy_add_call_rule(proxy, name.c_str(), wildcard, rule.c_str());
            else
                flatpak_proxy_add_broadcast_rule(proxy, name.c_str(), wildcard, rule.c_str());

            ++args_i;
        } else if (temp_arg == "--log") {
            flatpak_proxy_set_log_messages(proxy, TRUE);
            ++args_i;
        } else if (temp_arg == "--filter") {
            flatpak_proxy_set_filter(proxy, TRUE);
            ++args_i;
        } else if (temp_arg == "--sloppy-names") {
            flatpak_proxy_set_sloppy_names(proxy, TRUE);
            ++args_i;
        } else {
            if (!parse_generic_args(args, args_i))
                return false;
        }
    }
    GError *error=NULL;
    if (!flatpak_proxy_start(proxy, &error)) {
        std::cerr<<"Failed to start proxy for "<<bus_address<<": "<<error->message<<"\n";
        return false;
    }
    proxies.push_front(std::shared_ptr<FlatpakProxy>(proxy, g_object_unref));
    return true;
}

static bool sync_closed_cb() {
    for (auto &proxy: proxies) {
        flatpak_proxy_stop(proxy.get());
    }
    exit(0);
    return true;
}

int main(int argc, char *argv[]) {
    std::vector<std::string> args(argv + 1, argv + argc);
    int args_i;
    argv0 = argv[0];
    setlocale(LC_ALL, "");

    if (argc == 1) usage(EXIT_FAILURE, &std::cerr);
}