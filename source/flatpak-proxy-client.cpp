#include "headers/flatpak-proxy-client.h"
#include <cassert>
#include <filesystem>
#include <iostream>

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
    Filter *filter = new Filter(name, name_is_subtree, policy);
    this->add_filter(filter);
}


void FlatpakProxy::add_call_rule(std::string name, bool name_is_subtree, std::string rule) {
    Filter *filter = new Filter(name, name_is_subtree, FILTER_TYPE_CALL, rule);
    this->add_filter(filter);
}


void FlatpakProxy::add_broadcast_rule(std::string name, bool name_is_subtree, std::string rule) {
    Filter *filter = new Filter(name, name_is_subtree, FILTER_TYPE_BROADCAST, rule);
    this->add_filter(filter);
}


bool FlatpakProxy::start() {
    std::filesystem::remove(socket_path);

    GError *error = nullptr;
    GSocketAddress *address = g_unix_socket_address_new(socket_path.c_str());
    GSocketListener *listener = G_SOCKET_LISTENER(parent->gobj());

    gboolean ok = g_socket_listener_add_address(
            listener,
            address,
            G_SOCKET_TYPE_STREAM,
            G_SOCKET_PROTOCOL_DEFAULT,
            nullptr,
            nullptr,
            &error
    );
    g_object_unref(address);

    if (!ok) {
        std::cerr << "Ошибка добавления адреса: " << error->message << "\n";
        g_error_free(error);
        return false;
    }
    parent->signal_incoming().connect(
            sigc::mem_fun(*this, &FlatpakProxy::incoming_connection)
    );
    parent->start();
    return true;
}

void FlatpakProxy::stop() {
    std::filesystem::remove(socket_path);
    parent->stop();
}

bool FlatpakProxy::incoming_connection(
        const Glib::RefPtr<Gio::SocketConnection> &connection,
        const Glib::RefPtr<Glib::Object> &source_object
) {
    connection->get_socket()->set_blocking(false);

    FlatpakProxyClient* client = new FlatpakProxyClient(this, connection);

    GError* error = nullptr;
    GIOStream* stream = g_dbus_address_get_stream_sync(
            dbus_address.c_str(),
            nullptr,
            &error
    );

    if (!stream) {
        std::cerr << "Connection error to D-Bus: " << (error ? error->message : "unknown") << std::endl;
        g_clear_error(&error);
        delete client;
        return false;
    }

    GSocketConnection* bus_conn = G_SOCKET_CONNECTION(stream);
    g_socket_set_blocking(g_socket_connection_get_socket(bus_conn), FALSE);

    client->bus_side->connection = bus_conn; // GSocketConnection*
    g_object_ref(bus_conn); // если потом будешь хранить

    // запускаем чтение
    client->client_side->start_reading();
    client->bus_side->start_reading();


    return true;
}

FlatpakProxy::~FlatpakProxy() {
    if (Glib::RefPtr<Gio::SocketService> service = parent) {
        if (service->is_active()) {
            ::unlink(socket_path.c_str());
        }
    }
    assert(clients.empty());
    for (auto &[_, v]: filters) {
        for (auto f: v) {
            delete f;
        }
    }
    filters.clear();
}

FlatpakProxy::FlatpakProxy(std::string dbus_address, std::string socket_path) {
    this->dbus_address = dbus_address;
    this->socket_path = socket_path;
    log_messages = filter = sloppy_names = false;
    parent = Gio::SocketService::create();
    add_policy("org.freedesktop.DBus", false, FLATPAK_POLICY_TALK);
    clients = {};
}


void ProxySide::init_side(FlatpakProxyClient *client, bool is_bus_side){
    got_first_byte = is_bus_side;
    this->client = client;
    header_buffer.size = 16;
    header_buffer.pos = 0;
    current_read_buffer = &header_buffer;
    expected_replies.clear();
}


void ProxySide::free_side(){
    if (connection) {
        g_object_unref(connection);
        connection = nullptr;
    }

    extra_input_data.clear();

    buffers.clear();
    control_messages.clear();

    if (in_source) {
        g_source_destroy(in_source);
        in_source = nullptr;
    }

    if (out_source) {
        g_source_destroy(out_source);
        out_source = nullptr;
    }

    expected_replies.clear();
}
//todo side_in_cb
void ProxySide::start_reading() {
    GSocket* socket = g_socket_connection_get_socket(connection);
    in_source = g_socket_create_source(socket, G_IO_IN, nullptr);
    g_source_set_callback(in_source, G_SOURCE_FUNC(side_in_cb), this, nullptr);
    g_source_attach(in_source, nullptr);
    g_source_unref(in_source);
}

void ProxySide::stop_reading() {
    if (in_source) {
        g_source_destroy(in_source);
        in_source = nullptr;
    }
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


FlatpakProxyClient::FlatpakProxyClient(FlatpakProxy* proxy,
        Glib::RefPtr<Gio::SocketConnection> client_conn):
        proxy(proxy),
        auth_state(AUTH_WAITING_FOR_BEGIN),
        auth_requests(0),
        auth_replies(0),
        hello_serial(0),
        last_fake_serial(0xFFFFFFFF - 65536),
        client_side(std::make_unique<ProxySide>()),
        bus_side(std::make_unique<ProxySide>()){
    client_side->init_side(this,false);
    client_side->init_side(this,true);

    client_side->connection = G_SOCKET_CONNECTION(g_object_ref(client_conn->gobj()));
    proxy->clients.push_back(this);
}

FlatpakProxyClient::~FlatpakProxyClient() {
    if (proxy) {
        proxy->clients.remove(this);
    }

    client_side->free_side();
    bus_side->free_side();

    // todo replace GDBusMessage
    for (auto &[_, msg]: rewrite_reply) {
        g_object_unref(msg);
    }
    rewrite_reply.clear();
    get_owner_reply.clear();
    unique_id_policy.clear();
    unique_id_owned_names.clear();
}
//todo
void FlatpakProxyClient::add_unique_id_owned_name(){

}
void FlatpakProxyClient::update_unique_id_policy(){

}
void FlatpakProxyClient::get_max_policy(){

}
void FlatpakProxyClient::get_max_policy_and_matched(){

}
void FlatpakProxyClient::init_side(){

}
void FlatpakProxyClient::init(){

}