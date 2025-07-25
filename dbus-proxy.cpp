#include <fstream>
#include <cstdlib>
#include <memory>
#include <sys/stat.h>
#include <algorithm>
#include <glib.h>
#include <glib-unix.h>
#include <giomm.h>


#include "headers/flatpak-proxy-client.h"


static const char *argv0;
static std::list<FlatpakProxy *> proxies;
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

std::vector<uint8_t> fd_readall_bytes(int fd) {
    int maxreadlen = 4096;
    struct stat stbuf;
    if (TEMP_FAILURE_RETRY(fstat(fd, &stbuf)) != 0) {
        int errsv = errno;
        throw std::runtime_error(std::string("fstat failed: ") + strerror(errsv));
    }
    size_t buf_allocated = (S_ISREG(stbuf.st_mode) && stbuf.st_size > 0)
                           ? static_cast<size_t>(stbuf.st_size)
                           : 16;
    std::vector<uint8_t> buffer(buf_allocated);
    size_t buf_size = 0;
    while (true) {
        size_t readlen = std::min((int) (buf_allocated - buf_size), maxreadlen);
        ssize_t bytes_read;
        do
            bytes_read = read(fd, buffer.data() + buf_size, readlen);
        while (bytes_read == -1 && errno == EINTR);

        if (bytes_read == -1) {
            int errsv = errno;
            throw std::runtime_error(std::string("read failed: ") + strerror(errsv));
        }
        if (bytes_read == 0) break;
        buf_size += bytes_read;
        if (buf_allocated - buf_size < maxreadlen) {
            buf_allocated *= 2;
            buffer.resize(buf_allocated);
        }
    }
    buffer.resize(buf_size);
    return buffer;
}

void add_args(const std::vector<uint8_t> &data, std::vector<std::string> &args, size_t pos) {
    std::string text(data.begin(), data.end());
    std::istringstream stream(text);
    std::string token;

    while (stream >> token) {
        args.insert(args.begin() + pos, token);
        ++pos;
    }
}

bool parse_generic_args(std::vector<std::string> &args, size_t &args_i) {
    std::cout << "parse_generic_args\n";
    std::string arg = args[args_i];
    if (arg == "--help")
        usage(EXIT_SUCCESS, &std::cout);
    else if (arg == "--version") {
        std::cout << "xdg-dbus-proxy 0.1.6\n";
        exit(EXIT_SUCCESS);
    } else if (arg.starts_with("--fd=")) {
        std::string fd_s(arg.begin() + strlen("--fd="), arg.end());
        size_t parsed_chars = 0;
        int fd = std::stoi(fd_s, &parsed_chars);
        if (parsed_chars != fd_s.size() && fd < 0) {
            std::cerr << "Invalid --args fd " << fd_s << "\n";
            return false;
        }
        sync_fd = fd;
        ++args_i;
        return true;
    } else if (arg.starts_with("--args=")) {
        std::string fd_s(arg.begin() + strlen("--args="), arg.end());
        size_t parsed_chars = 0;
        int fd = std::stoi(fd_s, &parsed_chars);
        if (parsed_chars != fd_s.size() && fd < 0) {
            std::cerr << "Invalid --args fd " << fd_s << "\n";
            return false;
        }
        std::vector<uint8_t> data;
        try {
            data = fd_readall_bytes(fd);
        } catch (std::exception &ex) {
            std::cerr << "Failed to load --args: " << ex.what() << "\n";
            return false;
        }
        add_args(data, args, args_i);
        return true;
    } else {
        std::cerr << "Unknown argument " << arg << "\n";
        return false;
    }
}

static bool
start_proxy(std::vector<std::string> &args, size_t &args_i) {
    std::cout << "start-proxy\n";
    if (args_i >= args.size() || args[args_i][0] == '-') {
        std::cerr << "No bus address given\n";
        return false;
    }

    std::string bus_address = args[args_i++];

    if (args_i >= args.size() || args[args_i][0] == '-') {
        std::cerr << "No socket path given\n";
        return false;
    }

    std::string socket_path = args[args_i++];

    FlatpakProxy *proxy = new FlatpakProxy(bus_address.c_str(), socket_path.c_str());

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

            if (name.empty() || name[0] == ':') {
                std::cerr << "'" << name << "' is not a valid dbus name\n";
                return false;
            }

            proxy->add_policy(name, wildcard, policy);
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
                proxy->add_call_rule(name, wildcard, rule.c_str());
            else
                proxy->add_broadcast_rule(name, wildcard, rule.c_str());

            ++args_i;
        } else if (temp_arg == "--log") {
            proxy->set_log_messages(true);
            ++args_i;
        } else if (temp_arg == "--filter") {
            proxy->set_filter(true);
            ++args_i;
        } else if (temp_arg == "--sloppy-names") {
            proxy->set_sloppy_names(true);
            ++args_i;
        } else {
            if (!parse_generic_args(args, args_i))
                return false;
        }
    }
    GError *error = NULL;
//    std::cout << "Запуск прокси\n";
    if (!proxy->start()) {
        std::cerr << "Failed to start proxy for " << bus_address << ": " << error->message << "\n";
        return false;
    }
    proxies.push_front(proxy);
    return true;
}

gboolean sync_closed_cb(GIOChannel *, GIOCondition, gpointer) {
    for (auto &proxy: proxies) {
        proxy->stop();
        delete proxy;
    }
    exit(0);
    return true;
}

int main(int argc, char *argv[]) {
    Glib::init();
    Gio::init();

    std::vector<std::string> args(argv + 1, argv + argc);
    size_t args_i = 0;
    argv0 = argv[0];
    setlocale(LC_ALL, "");
//    std::cout << "main запустился\n";
    if (argc == 1) usage(EXIT_FAILURE, &std::cerr);

//    std::cout << "прошло условие\n";
    while (args_i < args.size()) {
        std::string &arg = args[args_i];
        std::cout << arg << "\n";
        if (arg[0] == '-') {
            if (!parse_generic_args(args, args_i)) {
                std::cout << "1\n";
                return EXIT_FAILURE;
            }
        } else {
            if (!start_proxy(args, args_i)) {
                std::cout << "2\n";
                return EXIT_FAILURE;
            }
        }
//        std::cout << "test\n";
    }

    if (proxies.empty()) {
        std::cerr << "No proxies specified\n";
        return EXIT_FAILURE;
    }
    if (sync_fd >= 0) {
//        std::cout << "условие сработало\n";
        ssize_t written = write(sync_fd, "x", 1);
        if (written == -1) {
            std::cerr << "Failed to write to sync-fd: " << strerror(errno) << "\n";
            return EXIT_FAILURE;
        }
        GIOChannel *sync_channel = g_io_channel_unix_new(sync_fd);
        g_io_add_watch(sync_channel,
                       static_cast<GIOCondition>(G_IO_ERR | G_IO_HUP),
                       sync_closed_cb,
                       NULL);
    }
    std::cout << "цикл\n";
    GMainLoop *main_loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(main_loop);
    g_main_loop_unref(main_loop);
    return EXIT_SUCCESS;
}