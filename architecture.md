```mermaid 
classDiagram
    class FlatpakProxy{
        +FlatpakProxy()
        +~FlatpakProxy()
        -set_filter()
        -set_sloppy_names()
        -set_log_messages()
        -add_filter()
        -add_policy()
        -add_call_rule()
        -add_broadcast_rule()
        -finalizer()
        -set_property()
        -get_property()
        -proxy_start()
        -proxy_stop()
        -socket parent
        -bool log_messages
        -list clients
        -string socket_path
        -string dbus_address
        -bool filters
        -bool sloopy_names
        -unordered_map filters
    }
    class Buffer{
        +size_t size
        +size_t pos
        +size_t sent
        +int refcount;
        +bool send_credentials
        +char data
        +list control_messages
    }

    class FlatpakProxyClient{
        +object parent
        +FlatpakProxy *proxy
        +AuthState auth_state
        +size_t auth_requests
        +size_t auth_replies
        +vector<uint8_t> auth_buffer
        +ProxySide client_side
        +ProxySide bus_side
        +uint32_t hello_serial
        +uint32_t last_fake_serial
        +unordered_map<int, int> rewrite_reply
        +unordered_map<int, int> get_owner_reply
        +unordered_map<int, int> unique_id_policy
        +unordered_map<int, int> unique_id_owned_names
        +init_side()
        +init_client()
        -client_new()
        +get_max_policy_and_matched()
        -get_max_policy()
        +update_unique_id_policy()
        +add_unique_id_owned_name()
    }
    class Header{
        +Buffer* buffer
        +bool big_endian
        +uint8_t type
        +uint8_t flags
        +uint32_t lenght
        +uint32_t serial
        +string path
        +string interface
        +string member
        +string error_names
        +string destination
        +string sender
        +string signature
        +bool has_reply_serial
        +uint32_t reply_serial
        +uint32_t unix_fds
        -free()
        +debug_str()
        +parse_header()
        +print_outgoing()
        +print_incoming()   
    }
    class ProxySide{
        +bool got_first_byte
        +bool closed
        +FlatpakProxyClient *client
        +socket connection
        +vector<uint8_t> extra_input_data
        +Buffer *current_read_buffer
        +Buffer header_buffer
        +unordered_map<uint32_t,shared_ptr<>> expected_replies
        }
    class Filter{
        +Filter()
        +string name
        +bool name_is_subtree
        +FlatpakPolicy policy
        -FilterTypeMask types;
        -string path;
        -bool path_is_subtree;
        -string interface;
        -string member;
    }
    class FilterTypeMask { <<enumeration>> 
    FILTER_TYPE_CALL
    FILTER_TYPE_BROADCAST
    FILTER_TYPE_ALL 
    } 

    class FlatpakPolicy { <<enumeration>> 
    FLATPAK_POLICY_NONE
    FLATPAK_POLICY_SEE
    FLATPAK_POLICY_TALK FLATPAK_POLICY_OWN 
    } 

    FlatpakProxyClient *-- ProxySide
    FlatpakProxyClient --> FlatpakProxy
    Header --> Buffer
    ProxySide --> FlatpakProxyClient
    ProxySide --> Buffer
    ProxySide *-- Buffer
    Filter --> FilterTypeMask
    Filter --> FlatpakPolicy
  ```
