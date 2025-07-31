#include "../headers/flatpak-proxy-client.h"
#include "../headers/utils.h"
#include <gio/gunixconnection.h>
#include <atomic>

// Глобальный счетчик для отладки
static std::atomic<int> g_buffer_count(0);
static std::atomic<int> g_buffer_id_counter(0);

Buffer::Buffer(size_t size, Buffer *old) : 
    size((size == 0) ? 16 : size),
    pos(0), 
    sent(0), 
    send_credentials(false), 
    refcount(1) {
    
    g_buffer_count++;
    buffer_id = ++g_buffer_id_counter;
    
    // Инициализируем данные
    data.resize(this->size);
    std::fill(data.begin(), data.end(), 0);
    
    // Копирование из старого буфера, если он передан
    if (old && old->pos > 0 && old->pos <= old->size) {
        // Копируем только если наш размер достаточен
        if (this->size >= old->pos) {
            pos = old->pos;
            sent = old->sent;
            std::copy_n(old->data.begin(), pos, data.begin());
            
            // Перемещаем control messages
            control_messages = std::move(old->control_messages);
            old->control_messages.clear();
        }
    }
    
    std::cerr << "BUFFER[" << buffer_id << "]: created - size=" << this->size 
              << " pos=" << pos << " addr=" << this
              << " total_buffers=" << g_buffer_count << "\n";
}

Buffer::~Buffer() {
    g_buffer_count--;
    
    std::cerr << "BUFFER[" << buffer_id << "]: destroyed - size=" << size 
              << " addr=" << this
              << " total_buffers=" << g_buffer_count << "\n";
    
    // Очищаем control messages
    for (auto *msg : control_messages) {
        if (msg) g_object_unref(msg);
    }
    control_messages.clear();
    
    // Обнуляем данные для обнаружения use-after-free
    buffer_id = -buffer_id;  // Отрицательный ID = удаленный буфер
    size = 0;
    pos = 0;
    sent = 0;
    refcount = -999999;  // Маркер удаленного объекта
    data.clear();
}

void Buffer::ref() {
    if (refcount <= 0 || refcount >= 1000) {
        std::cerr << "BUFFER[" << buffer_id << "] ERROR: ref() on invalid buffer with refcount=" 
                  << refcount << " addr=" << this << "\n";
        abort();  // Критическая ошибка - аварийное завершение
        return;
    }
    refcount++;
    std::cerr << "BUFFER[" << buffer_id << "]: ref - refcount=" << refcount 
              << " addr=" << this << "\n";
}

void Buffer::unref() {
    if (refcount <= 0 || refcount >= 1000) {
        std::cerr << "BUFFER[" << buffer_id << "] ERROR: unref() on invalid buffer with refcount=" 
                  << refcount << " addr=" << this << "\n";
        abort();  // Критическая ошибка - аварийное завершение
        return;
    }
    
    refcount--;
    std::cerr << "BUFFER[" << buffer_id << "]: unref - refcount=" << refcount 
              << " addr=" << this << "\n";
    
    if (refcount == 0) {
        std::cerr << "BUFFER[" << buffer_id << "]: deleting buffer addr=" << this << "\n";
        delete this;
    }
}

bool Buffer::read(ProxySide *side, GSocket *socket) {
    // Проверка на удаленный объект
    if (refcount <= 0 || refcount >= 1000 || buffer_id < 0) {
        std::cerr << "BUFFER[" << buffer_id << "] ERROR: read() on deleted/invalid buffer"
                  << " refcount=" << refcount << " addr=" << this << "\n";
        return false;
    }
    
    std::cerr << "BUFFER[" << buffer_id << "]: read() start - size=" << size 
              << " pos=" << pos << " addr=" << this << "\n";
    
    if (size == 0) {
        std::cerr << "BUFFER[" << buffer_id << "] ERROR: Buffer size is 0\n";
        return false;
    }
    
    if (pos >= size) {
        std::cerr << "BUFFER[" << buffer_id << "] ERROR: Buffer full - pos=" << pos 
                  << " size=" << size << "\n";
        return false;
    }
    
    FlatpakProxyClient *client = side->client.get();
    if (!client) {
        std::cerr << "BUFFER[" << buffer_id << "] ERROR: No client\n";
        return false;
    }
    
    size_t received = 0;

    if (client->auth_state == AUTH_WAITING_FOR_BACKLOG && side == &client->client_side) {
        std::cerr << "BUFFER[" << buffer_id << "]: read() - waiting for backlog\n";
        return false;
    }

    if (!side->extra_input_data.empty() && client->auth_state == AUTH_COMPLETE) {
        received = std::min(size - pos, side->extra_input_data.size());
        
        if (received > 0) {
            std::copy_n(side->extra_input_data.begin(), received, data.begin() + pos);
            
            if (received < side->extra_input_data.size()) {
                side->extra_input_data.erase(side->extra_input_data.begin(), 
                                           side->extra_input_data.begin() + received);
            } else {
                side->extra_input_data.clear();
            }
            std::cerr << "BUFFER[" << buffer_id << "]: read() from extra_input_data - received=" 
                      << received << "\n";
        }
    } else if (side->extra_input_data.empty()) {
        GInputVector vec;
        vec.buffer = data.data() + pos;
        vec.size = size - pos;

        GSocketControlMessage **messages = nullptr;
        int num_messages = 0;
        int flags = 0;
        GError *error = nullptr;

        gssize res = g_socket_receive_message(
            socket, nullptr, &vec, 1,
            &messages, &num_messages, &flags,
            nullptr, &error
        );

        if (res < 0 && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK)) {
            g_error_free(error);
            std::cerr << "BUFFER[" << buffer_id << "]: read() - would block\n";
            return false;
        }
        
        if (res <= 0) {
            if (res != 0 && error) {
                std::cerr << "BUFFER[" << buffer_id << "] Socket error: " << error->message << "\n";
                g_error_free(error);
            }
            std::cerr << "BUFFER[" << buffer_id << "]: read() - socket closed/error\n";
            side->side_closed();
            return false;
        }
        
        received = static_cast<size_t>(res);
        std::cerr << "BUFFER[" << buffer_id << "]: read() from socket - received=" << received << "\n";
        
        for (int i = 0; i < num_messages; ++i) {
            control_messages.push_back(messages[i]);
        }
        g_free(messages);
    }
    
    if (received > 0 && pos + received <= size) {
        pos += received;
        std::cerr << "BUFFER[" << buffer_id << "]: read() done - new pos=" << pos << "\n";
        return true;
    }
    
    return false;
}

bool Buffer::write(ProxySide *side, GSocket *socket) {
    // Проверка на удаленный объект
    if (refcount <= 0 || refcount >= 1000 || buffer_id < 0) {
        std::cerr << "BUFFER[" << buffer_id << "] ERROR: write() on deleted/invalid buffer"
                  << " refcount=" << refcount << " addr=" << this << "\n";
        return false;
    }
    
    GError *error = nullptr;
    
    if (send_credentials && G_IS_UNIX_CONNECTION(side->connection)) {
        assert(size == 1);
        if (!g_unix_connection_send_credentials(G_UNIX_CONNECTION(side->connection),
                                              nullptr, &error)) {
            if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK)) {
                g_error_free(error);
                return false;
            }
            std::cerr << "Error sending credentials: " << error->message << "\n";
            g_error_free(error);
            side->side_closed();
            return false;
        }
        sent = 1;
        return true;
    }

    // Проверяем границы перед отправкой
    if (sent >= pos) {
        return true; // Всё уже отправлено
    }

    std::vector<GSocketControlMessage *> messages(control_messages.begin(), control_messages.end());
    GOutputVector vec;
    vec.buffer = const_cast<void *>(reinterpret_cast<const void *>(data.data() + sent));
    vec.size = pos - sent;
    
    gssize res = g_socket_send_message(
        socket,
        nullptr,
        &vec, 1,
        messages.empty() ? nullptr : messages.data(),
        static_cast<int>(messages.size()),
        G_SOCKET_MSG_NONE,
        nullptr,
        &error
    );
    
    if (res < 0 && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK)) {
        g_error_free(error);
        return false;
    }
    
    if (res <= 0) {
        if (res < 0) {
            std::cerr << "Error writing to socket: " << error->message << "\n";
            g_error_free(error);
        }
        side->side_closed();
        return false;
    }

    for (auto msg : control_messages) {
        g_object_unref(msg);
    }
    control_messages.clear();

    sent += static_cast<size_t>(res);
    return true;
}