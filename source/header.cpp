#include "headers/flatpak-proxy-client.h"
#include "headers/utils.h"


std::string debug_str(std::string s, Header *header) {
    if (!header->path.empty())
        s += "\n\tPath: " + header->path;
    if (!header->interface.empty())
        s += "\n\tInterface: " + header->interface;
    if (!header->member.empty())
        s += "\n\tMember: " + header->member;
    if (!header->error_name.empty())
        s += "\n\tError name: " + header->error_name;
    if (!header->destination.empty())
        s += "\n\tDestination: " + header->destination;
    if (!header->sender.empty())
        s += "\n\tSender: " + header->sender;
    return s;
}

std::string get_signature(Buffer *buffer, uint32_t *offset, uint32_t end_offset) {
    if (*offset >= end_offset) return FALSE;
    size_t len = buffer->data[(*offset)++];
    if (*offset + len + 1 > end_offset || buffer->data[*offset + len] != 0) return FALSE;
    std::string str(buffer->data.begin() + *offset, buffer->data.begin() + *offset + len);
    offset += len + 1;
    return str;
}

std::string get_string(Buffer *buffer, Header *header, uint32_t *offset, uint32_t end_offset) {
    *offset = align_by_4(*offset);
    if (*offset + 4 >= end_offset)
        throw std::runtime_error("String header would align past boundary");
    uint32_t len = read_uint32(header, &buffer->data[*offset]);
    *offset += 4;

    if (*offset + len + 1 > end_offset || *offset + len + 1 > buffer->data.size())
        throw std::runtime_error("String would align past boundary");
//    if (buffer->data[(*offset) + len] != 0) todo remove all such lines, as this is a condition for the nul terminal.
    std::string str(buffer->data.begin() + *offset, buffer->data.begin() + *offset + len);
    offset += len + 1;
    return str;


}

void Header::parse(Buffer *buffer) {
    buffer->ref();
    try {
        if (buffer->size < 16) {
            throw std::runtime_error("Buffer too small: " + std::to_string(buffer->size));
        }
        if (buffer->data[3] != 1) {
            throw std::runtime_error("Wrong protocol version: " + std::to_string(buffer->data[3]));
        }
        buffer->data[0] == 'B' ? big_endian = true : buffer->data[0] == 'l' ? big_endian = false :
                                                     throw std::runtime_error("Invalid endianess marker: " +
                                                                              std::to_string(buffer->data[0]));
        type = buffer->data[1];
        flags = buffer->data[2];

        length = read_uint32(this, &buffer->data[4]);
        serial = read_uint32(this, &buffer->data[8]);
        if (serial == 0)
            throw std::runtime_error("No serial");
        uint32_t array_len = read_uint32(this, &buffer->data[12]);
        uint32_t header_len = align_by_8(12 + 4 + array_len);
        assert(buffer->size >= header_len);
        if (header_len > buffer->size)
            throw std::runtime_error("Header len " + std::to_string(header_len) + " bigger than buffer size (" +
                                     std::to_string(buffer->size) + ")");
        uint32_t offset = 12 + 4;
        uint32_t end_offset = offset + array_len;

        std::string header_str;
        uint8_t header_type;
        std::string signature_temp;

        while (offset < end_offset) {
            offset = align_by_8(offset);
            if (offset >= end_offset)
                throw std::runtime_error("Struct would align past boundary " + debug_str(header_str, this));
            header_type = buffer->data[offset++];
            if (offset >= end_offset)
                throw std::runtime_error("Went past boundary after parsing header_type " + debug_str(header_str, this));
            signature_temp = get_signature(buffer, &offset, end_offset);
            if (signature_temp.empty())
                throw std::runtime_error("Could not parse signature " + debug_str(header_str, this));
            switch (header_type) {
                case G_DBUS_MESSAGE_HEADER_FIELD_INVALID:
                    throw std::runtime_error("Field is invalid " + debug_str(header_str, this));
                case G_DBUS_MESSAGE_HEADER_FIELD_PATH:
                    if (signature_temp != "o")
                        throw std::runtime_error(
                                "Signature is invalid for path (" + signature_temp + ")" + debug_str(header_str, this));
                    path = get_string(buffer, this, &offset, end_offset);
                    break;
                case G_DBUS_MESSAGE_HEADER_FIELD_INTERFACE:
                    if (signature_temp != "s")
                        throw std::runtime_error(
                                "Signature is invalid for interface (" + signature_temp + ")" + debug_str(header_str, this));
                    interface = get_string(buffer, this, &offset, end_offset);
                    break;
                case G_DBUS_MESSAGE_HEADER_FIELD_MEMBER:
                    if (signature_temp != "s")
                        throw std::runtime_error(
                                "Signature is invalid for member (" + signature_temp + ")" + debug_str(header_str, this));
                    member = get_string(buffer, this, &offset, end_offset);
                    break;
                case G_DBUS_MESSAGE_HEADER_FIELD_ERROR_NAME:
                    if (signature_temp != "s")
                        throw std::runtime_error(
                                "Signature is invalid for error (" + signature_temp + ")" + debug_str(header_str, this));
                    error_name = get_string(buffer, this, &offset, end_offset);
                    break;
                case G_DBUS_MESSAGE_HEADER_FIELD_REPLY_SERIAL:
                    if (offset + 4 > end_offset)
                        throw std::runtime_error(
                                "Header too small to fit reply serial " + debug_str(header_str, this));
                    has_reply_serial = true;
                    reply_serial = read_uint32(this, &buffer->data[offset]);
                    offset += 4;
                    break;
                case G_DBUS_MESSAGE_HEADER_FIELD_DESTINATION:
                    if (signature_temp != "s")
                        throw std::runtime_error(
                                "Signature is invalid for destination (" + signature_temp + ")" +
                                debug_str(header_str, this));
                    destination = get_string(buffer, this, &offset, end_offset);
                    break;
                case G_DBUS_MESSAGE_HEADER_FIELD_SENDER:
                    if (signature_temp != "s")
                        throw std::runtime_error(
                                "Signature is invalid for sender (" + signature_temp + ")" + debug_str(header_str, this));
                    sender = get_string(buffer, this, &offset, end_offset);
                    break;
                case G_DBUS_MESSAGE_HEADER_FIELD_SIGNATURE:
                    if (signature_temp != "g")
                        throw std::runtime_error(
                                "Signature is invalid for signature (" + signature_temp + ")" + debug_str(header_str, this));
                    signature = get_signature(buffer, &offset, end_offset);
                    if (signature.empty())
                        throw std::runtime_error(
                                "Could not parse signature in signature field " + debug_str(header_str, this));
                    break;
                case G_DBUS_MESSAGE_HEADER_FIELD_NUM_UNIX_FDS:
                    if (offset + 4 > end_offset)
                        throw std::runtime_error(
                                "Header too small to fit Unix FD " + debug_str(header_str, this));
                    unix_fds = read_uint32(this, &buffer->data[offset]);
                    offset += 4;
                    break;
                default:
                    throw std::runtime_error(
                            "Unknown header field (" + std::to_string(header_type) + ")" + debug_str(header_str, this));
            }
            switch (type) {
                case G_DBUS_MESSAGE_TYPE_METHOD_CALL:
                    if (path.empty() || member.empty())
                        throw std::runtime_error(
                                "Method call is missing path or member " + debug_str(header_str, this));
                    break;
                case G_DBUS_MESSAGE_TYPE_METHOD_RETURN:
                    if (!has_reply_serial)
                        throw std::runtime_error(
                                "Method return has no reply serial " + debug_str(header_str, this));
                    break;
                case G_DBUS_MESSAGE_TYPE_ERROR:
                    if (error_name.empty() || !has_reply_serial)
                        throw std::runtime_error(
                                "Error is missing error name or reply serial " + debug_str(header_str, this));
                    break;
                case G_DBUS_MESSAGE_TYPE_SIGNAL:
                    if (path.empty() ||
                        interface.empty() ||
                        member.empty())
                        throw std::runtime_error(
                                "Signal is missing path, interface or member " + debug_str(header_str, this));
                    if (path == "/org/freedesktop/DBus/Local" ||
                        interface == "org.freedesktop.DBus.Local")
                        throw std::runtime_error(
                                "Signal is to D-Bus Local path or interface " + debug_str(header_str, this));
                    break;
                default:
                    throw std::runtime_error(
                            "Unknown message type (" + std::to_string(type) + ")" + debug_str(header_str, this));
            }
        }
    } catch (std::exception &ex) {
        std::cerr << ex.what() << "\n";
    }
};

Header::~Header() {
    if (buffer) {
        delete buffer;
    }
};

bool Header::client_message_generates_reply() {
    switch (type) {
        case G_DBUS_MESSAGE_TYPE_METHOD_CALL:
            return (flags & G_DBUS_MESSAGE_FLAGS_NO_REPLY_EXPECTED) == 0;
        case G_DBUS_MESSAGE_TYPE_SIGNAL:
        case G_DBUS_MESSAGE_TYPE_METHOD_RETURN:
        case G_DBUS_MESSAGE_TYPE_ERROR:
        default:
            return false;
    }
}


void Header::print_outgoing(){
    switch(type) {
        case G_DBUS_MESSAGE_TYPE_METHOD_CALL:
            //todo debug: check if you can't print the values right away, because they're empty.
            std::cout << "C" << serial <<
                      ": -> " << (!destination.empty() ? destination : "(no dest)") <<
                      " call " << (!interface.empty() ? interface : "") <<
                      "." << (!member.empty() ? member : "") <<
                      " at " << (!path.empty() ? path : "") << "\n";
            break;
        case G_DBUS_MESSAGE_TYPE_METHOD_RETURN:
            std::cout << "C" << serial << ": -> " <<
                      (!destination.empty() ? destination : "(no dest)") <<
                      " return from B" << reply_serial << "\n";
            break;
        case G_DBUS_MESSAGE_TYPE_ERROR:
            std::cout << "C" << serial << ": -> " <<
            (!destination.empty() ? destination : "(no dest)") <<
            " return error "<<(!error_name.empty()?error_name:"(no error)")<<
            " from B"<<reply_serial<<"\n";
            break;
        case G_DBUS_MESSAGE_TYPE_SIGNAL:
            std::cout << "C" << serial <<
                      ": -> " << (!destination.empty() ? destination : "all") <<
                      " signal " << (!interface.empty() ? interface : "") <<
                      "." << (!member.empty() ? member : "") <<
                      " at " << (!path.empty() ? path : "") << "\n";
            break;
        default:
            std::cout<<"unknown message type\n";
    }
}

void Header::print_incoming(){
    switch (type){
        case G_DBUS_MESSAGE_TYPE_METHOD_CALL:
            std::cout<<"B"<<serial<<
            ": <- "<<(!sender.empty()?sender:"(no sender)")<<
            " call "<<(!interface.empty() ? interface : "")<<
            "."<<(!member.empty() ? member : "")<<
            " at "<<(!path.empty() ? path : "")<<"\n";
            break;

        case G_DBUS_MESSAGE_TYPE_METHOD_RETURN:
            std::cout<<"B"<<serial<<
            ": <- "<<(!sender.empty()?sender:"(no sender)")<<
            " return from C"<<reply_serial<<"\n";
            break;
        case G_DBUS_MESSAGE_TYPE_ERROR:
            std::cout<<"B"<<serial<<
            ": <- "<<(!sender.empty()?sender:"(no sender)")<<
            " return error "<<(!error_name.empty()?error_name:"(no error)")<<
            " from C"<<reply_serial<<"\n";
            break;
        case G_DBUS_MESSAGE_TYPE_SIGNAL:
            std::cout<<"B"<<serial<<
            ": <- "<<(!sender.empty()?sender:"(no sender)")<<
            " signal "<<(!interface.empty()?interface:"")<<
            "."<<(!member.empty()?member:"")<<
            " at "<<(!path.empty()?path:"")<<"\n";
            break;
        default:
            std::cout<<"unknown message type\n";
    }
}