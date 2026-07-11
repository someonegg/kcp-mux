#include "kcpmux_e2e_utils.h"
#include <chrono>
#include <cstdio>

namespace kcpmux_e2e {

// ============================================================================
// Time utilities
// ============================================================================

int64_t monotonic_time_ms() {
    auto now = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch());
    return ms.count();
}

// ============================================================================
// KCPMUX Callbacks
// ============================================================================

void e2e_set_timer(uint64_t wake_after_ms, void *user_data) {
    E2EEndpoint *ep = static_cast<E2EEndpoint *>(user_data);
    int64_t now = monotonic_time_ms();
    ep->next_wakeup_ms.store(now + (int64_t)wake_after_ms);
}

int e2e_write_socket(const uint8_t *buf, unsigned size, const kcpmux_addr_t *addr, void *user_data) {
    E2EEndpoint *ep = static_cast<E2EEndpoint *>(user_data);
    if (ep->udp_socket < 0) return 0;

    struct sockaddr_in dest{};
    dest.sin_family = AF_INET;
    if (addr && addr->addrlen >= 6) {
        // addr format: IP(4) + Port(2)
        uint32_t ip = ((uint32_t)addr->addr[0] << 24) | ((uint32_t)addr->addr[1] << 16) |
                      ((uint32_t)addr->addr[2] << 8) | addr->addr[3];
        uint16_t port = ((uint16_t)addr->addr[4] << 8) | addr->addr[5];
        dest.sin_addr.s_addr = htonl(ip);
        dest.sin_port = htons(port);
    } else {
        dest = ep->peer_addr;
    }

    ssize_t sent = sendto(ep->udp_socket, (const char *)buf, size, 0,
                          (struct sockaddr *)&dest, sizeof(dest));
    if (sent == size) {
        ep->total_bytes_sent += sent;
        return 1;
    }

    // For UDP, sendto should be atomic - either send all or fail.
    return 0;
}

int64_t e2e_monotonic_time_ms(void *user_data) {
    (void)user_data;
    return monotonic_time_ms();
}

int e2e_conn_connect_notify(kcpmux_conn_t *conn, const kcpmux_proto_ext_t *proto_ext,
                            kcpmux_proto_ext_t *resp_proto_ext, void *user_data) {
    E2EEndpoint *ep = static_cast<E2EEndpoint *>(user_data);

    std::lock_guard<std::mutex> lock(ep->mutex);

    // Store received extension data
    if (proto_ext && proto_ext->data && proto_ext->len > 0) {
        ep->conn_proto_ext_received.assign(proto_ext->data, proto_ext->data + proto_ext->len);
    }

    // Fill response extension if configured
    if (resp_proto_ext && !ep->conn_proto_ext_response.empty()) {
        resp_proto_ext->data = ep->conn_proto_ext_response.data();
        resp_proto_ext->len = (unsigned)ep->conn_proto_ext_response.size();
    }

    ep->accepted_conns.push_back(conn);
    if (!ep->conn) {
        ep->conn = conn;
        // Set callbacks for accepted connection
        kcpmux_conn_callbacks_t callbacks{};
        callbacks.conn_state_changed = e2e_conn_state_changed;
        callbacks.conn_close_notify = e2e_conn_close_notify;
        callbacks.stream_create_notify = e2e_stream_create_notify;
        kcpmux_conn_set_callbacks(conn, &callbacks, user_data);

        // Set peer_addr for server-side connection
        const kcpmux_addr_t *peer = kcpmux_conn_get_peer_addr(conn);
        if (peer && peer->addrlen >= 6) {
            uint32_t ip = ((uint32_t)peer->addr[0] << 24) | ((uint32_t)peer->addr[1] << 16) |
                          ((uint32_t)peer->addr[2] << 8) | peer->addr[3];
            uint16_t port = ((uint16_t)peer->addr[4] << 8) | peer->addr[5];
            ep->peer_addr.sin_family = AF_INET;
            ep->peer_addr.sin_addr.s_addr = htonl(ip);
            ep->peer_addr.sin_port = htons(port);
        }
    }

    return ep->conn_notify_result;
}

void e2e_conn_state_changed(kcpmux_conn_t *conn, uint8_t old_state, uint8_t new_state, void *user_data) {
    (void)conn;
    (void)old_state;
    E2EEndpoint *ep = static_cast<E2EEndpoint *>(user_data);
    ep->conn_state.store(new_state);
}

void e2e_conn_close_notify(kcpmux_conn_t *conn, int reason, void *user_data) {
    (void)conn;
    E2EEndpoint *ep = static_cast<E2EEndpoint *>(user_data);
    ep->conn_close_reason.store(reason);
}

void e2e_stream_create_notify(kcpmux_stream_t *stream, void *user_data) {
    E2EEndpoint *ep = static_cast<E2EEndpoint *>(user_data);

    std::lock_guard<std::mutex> lock(ep->mutex);
    ep->accepted_streams.push_back(stream);
    if (!ep->stream) {
        ep->stream = stream;
    }
    // Set callbacks for all accepted streams
    kcpmux_stream_callbacks_t callbacks{};
    callbacks.stream_state_changed = e2e_stream_state_changed;
    callbacks.stream_read_notify = e2e_stream_read_notify;
    callbacks.stream_write_notify = e2e_stream_write_notify;
    callbacks.stream_close_notify = e2e_stream_close_notify;
    kcpmux_stream_set_callbacks(stream, &callbacks, user_data);
    ep->stream_state.store(KCPMUX_STREAM_STATE_OPEN);

}

void e2e_stream_state_changed(kcpmux_stream_t *stream, uint8_t old_state, uint8_t new_state, void *user_data) {
    (void)stream;
    (void)old_state;
    E2EEndpoint *ep = static_cast<E2EEndpoint *>(user_data);
    ep->stream_state.store(new_state);
}

void e2e_stream_read_notify(kcpmux_stream_t *stream, void *user_data) {
    E2EEndpoint *ep = static_cast<E2EEndpoint *>(user_data);
    uint32_t stream_id = kcpmux_stream_id(stream);

    // Read all available data
    uint8_t buf[4096];
    int ret;
    while ((ret = kcpmux_stream_recv(stream, buf, sizeof(buf))) > 0) {
        std::lock_guard<std::mutex> lock(ep->mutex);
        auto &data = ep->received_data[stream_id];
        data.insert(data.end(), buf, buf + ret);
        ep->total_bytes_received += ret;
    }
}

void e2e_stream_write_notify(kcpmux_stream_t *stream, void *user_data) {
    E2EEndpoint *ep = static_cast<E2EEndpoint *>(user_data);
    (void)stream;

    // Try to send pending data
    std::lock_guard<std::mutex> lock(ep->mutex);
    while (ep->pending_send_offset < ep->pending_send_data.size()) {
        size_t remaining = ep->pending_send_data.size() - ep->pending_send_offset;
        size_t chunk_size = std::min(remaining, (size_t)4096);
        int ret = kcpmux_stream_send(ep->stream,
                                    ep->pending_send_data.data() + ep->pending_send_offset,
                                    (unsigned)chunk_size, 1);
        if (ret > 0) {
            ep->pending_send_offset += ret;
        } else {
            // Still blocked or error
            break;
        }
    }

    // Clear pending data if all sent
    if (ep->pending_send_offset >= ep->pending_send_data.size()) {
        ep->pending_send_data.clear();
        ep->pending_send_offset = 0;
    }
}

void e2e_stream_close_notify(kcpmux_stream_t *stream, int reason, void *user_data) {
    (void)stream;
    E2EEndpoint *ep = static_cast<E2EEndpoint *>(user_data);
    ep->stream_close_reason.store(reason);
}

// ============================================================================
// E2EEndpoint Implementation
// ============================================================================

bool E2EEndpoint::init(uint16_t bind_port) {
    init_sockets();

    udp_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_socket < 0) return false;

    // Set SO_REUSEADDR for faster test reruns
    int reuse = 1;
    setsockopt(udp_socket, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));

    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    local_addr.sin_port = htons(bind_port);  // 0 = let OS assign

    if (bind(udp_socket, (struct sockaddr *)&local_addr, sizeof(local_addr)) < 0) {
        close_socket(udp_socket);
        udp_socket = -1;
        return false;
    }

    // Get actual port assigned by OS
    socklen_t addr_len = sizeof(local_addr);
    if (getsockname(udp_socket, (struct sockaddr *)&local_addr, &addr_len) < 0) {
        close_socket(udp_socket);
        udp_socket = -1;
        return false;
    }
    port = ntohs(local_addr.sin_port);

    // Create engine
    kcpmux_engine_callbacks_t engine_callbacks{};
    engine_callbacks.set_timer = e2e_set_timer;
    engine_callbacks.write_socket = e2e_write_socket;
    engine_callbacks.monotonic_time_ms = e2e_monotonic_time_ms;
    engine_callbacks.conn_connect_notify = e2e_conn_connect_notify;

    engine = kcpmux_engine_create(nullptr, nullptr, nullptr, &engine_callbacks, this, nullptr);
    return engine != nullptr;
}

void E2EEndpoint::start() {
    running.store(true);
    worker = std::thread(&E2EEndpoint::worker_func, this);
}

void E2EEndpoint::stop() {
    running.store(false);
    if (worker.joinable()) {
        worker.join();
    }
}

void E2EEndpoint::cleanup() {
    if (engine) {
        kcpmux_engine_destroy(engine);
        engine = nullptr;
    }
    if (udp_socket >= 0) {
        close_socket(udp_socket);
        udp_socket = -1;
    }
    conn = nullptr;
    stream = nullptr;
    accepted_conns.clear();
    accepted_streams.clear();
    received_data.clear();
    cleanup_sockets();
}

void E2EEndpoint::queue_action(std::function<void()> action) {
    std::lock_guard<std::mutex> lock(action_mutex);
    pending_actions.push_back(std::move(action));
}

void E2EEndpoint::process_actions() {
    std::vector<std::function<void()>> actions;
    {
        std::lock_guard<std::mutex> lock(action_mutex);
        actions.swap(pending_actions);
    }
    for (auto &action : actions) {
        action();
    }
}

void E2EEndpoint::worker_func() {
    uint8_t recv_buf[65536];

    while (running.load()) {
        // Calculate select timeout
        int64_t now = monotonic_time_ms();
        int64_t wakeup = next_wakeup_ms.load();
        int timeout_ms = 10;  // Default 10ms
        if (wakeup > now) {
            timeout_ms = std::min((int)(wakeup - now), 100);
        }

        // Select on socket
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(udp_socket, &readfds);

        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;

        int ret = select(udp_socket + 1, &readfds, nullptr, nullptr, &tv);

        // Receive UDP packets
        if (ret > 0 && FD_ISSET(udp_socket, &readfds)) {
            struct sockaddr_in from_addr{};
            socklen_t from_len = sizeof(from_addr);
            ssize_t recv_len = recvfrom(udp_socket, (char *)recv_buf, sizeof(recv_buf), 0,
                                        (struct sockaddr *)&from_addr, &from_len);
            if (recv_len > 0) {
                // Convert sockaddr to kcpmux_addr
                uint8_t addr_buf[6];
                uint32_t ip = ntohl(from_addr.sin_addr.s_addr);
                uint16_t port = ntohs(from_addr.sin_port);
                addr_buf[0] = (ip >> 24) & 0xff;
                addr_buf[1] = (ip >> 16) & 0xff;
                addr_buf[2] = (ip >> 8) & 0xff;
                addr_buf[3] = ip & 0xff;
                addr_buf[4] = (port >> 8) & 0xff;
                addr_buf[5] = port & 0xff;
                kcpmux_addr_t addr{addr_buf, 6};

                kcpmux_engine_input(engine, recv_buf, (unsigned)recv_len, &addr);
            }
        }

        // Process pending actions
        process_actions();

        // Update engine
        kcpmux_engine_update(engine);
    }
}

void E2EEndpoint::connect_to(const struct sockaddr_in &addr) {
    connect_to_with_ext(addr, nullptr, 0);
}

void E2EEndpoint::connect_to_with_ext(const struct sockaddr_in &addr, const uint8_t *ext, size_t ext_len) {
    peer_addr = addr;

    // Store extension data for later use
    if (ext && ext_len > 0) {
        conn_proto_ext_to_send.assign(ext, ext + ext_len);
    } else {
        conn_proto_ext_to_send.clear();
    }

    // Convert to kcpmux_addr using std::array for safe lambda capture
    std::array<uint8_t, 6> addr_buf;
    uint32_t ip = ntohl(addr.sin_addr.s_addr);
    uint16_t port_num = ntohs(addr.sin_port);
    addr_buf[0] = (ip >> 24) & 0xff;
    addr_buf[1] = (ip >> 16) & 0xff;
    addr_buf[2] = (ip >> 8) & 0xff;
    addr_buf[3] = ip & 0xff;
    addr_buf[4] = (port_num >> 8) & 0xff;
    addr_buf[5] = port_num & 0xff;

    queue_action([this, addr_buf]() {
        kcpmux_addr_t kcpmux_addr{const_cast<uint8_t *>(addr_buf.data()), 6};
        kcpmux_conn_callbacks_t callbacks{};
        callbacks.conn_state_changed = e2e_conn_state_changed;
        callbacks.conn_close_notify = e2e_conn_close_notify;
        callbacks.stream_create_notify = e2e_stream_create_notify;

        kcpmux_proto_ext_t *proto_ext_ptr = nullptr;
        kcpmux_proto_ext_t proto_ext{};
        if (!conn_proto_ext_to_send.empty()) {
            proto_ext.data = conn_proto_ext_to_send.data();
            proto_ext.len = (unsigned)conn_proto_ext_to_send.size();
            proto_ext_ptr = &proto_ext;
        }

        conn = kcpmux_conn_connect(engine, &kcpmux_addr, nullptr, proto_ext_ptr, &callbacks, this);
    });
}

void E2EEndpoint::create_stream() {
    queue_action([this]() {
        if (!conn) return;
        kcpmux_stream_callbacks_t callbacks{};
        callbacks.stream_state_changed = e2e_stream_state_changed;
        callbacks.stream_read_notify = e2e_stream_read_notify;
        callbacks.stream_write_notify = e2e_stream_write_notify;
        callbacks.stream_close_notify = e2e_stream_close_notify;
        stream = kcpmux_stream_create(conn, nullptr, &callbacks, this);
        stream_state.store(KCPMUX_STREAM_STATE_OPEN);
    });
}

void E2EEndpoint::send_data(const uint8_t *data, size_t len) {
    std::vector<uint8_t> data_copy(data, data + len);
    queue_action([this, data_copy]() {
        if (!stream) return;
        kcpmux_stream_send(stream, data_copy.data(), (unsigned)data_copy.size(), 1);
    });
}

void E2EEndpoint::send_data_with_retry(const uint8_t *data, size_t len) {
    std::vector<uint8_t> data_copy(data, data + len);
    queue_action([this, data_copy]() {
        if (!stream) return;

        // Append to pending send buffer
        {
            std::lock_guard<std::mutex> lock(mutex);
            pending_send_data.insert(pending_send_data.end(), data_copy.begin(), data_copy.end());
        }

        // Try to send pending data
        std::lock_guard<std::mutex> lock(mutex);
        while (pending_send_offset < pending_send_data.size()) {
            size_t remaining = pending_send_data.size() - pending_send_offset;
            size_t chunk_size = std::min(remaining, (size_t)4096);
            int ret = kcpmux_stream_send(stream,
                                        pending_send_data.data() + pending_send_offset,
                                        (unsigned)chunk_size, 1);
            if (ret > 0) {
                pending_send_offset += ret;
            } else {
                // Blocked or error, will continue in write_notify
                break;
            }
        }

        // Clear pending data if all sent
        if (pending_send_offset >= pending_send_data.size()) {
            pending_send_data.clear();
            pending_send_offset = 0;
        }
    });
}

bool E2EEndpoint::has_pending_send() {
    std::lock_guard<std::mutex> lock(mutex);
    return pending_send_offset < pending_send_data.size();
}

void E2EEndpoint::close_conn() {
    queue_action([this]() {
        if (conn) {
            kcpmux_conn_close(conn);
        }
    });
}

void E2EEndpoint::close_stream() {
    queue_action([this]() {
        if (stream) {
            kcpmux_stream_close(stream);
        }
    });
}

void E2EEndpoint::clear_received_data(uint32_t stream_id) {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = received_data.find(stream_id);
    if (it == received_data.end()) return;
    it->second.clear();
}

size_t E2EEndpoint::get_received_data(uint32_t stream_id, uint8_t *buf, size_t max_len) {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = received_data.find(stream_id);
    if (it == received_data.end()) return 0;
    size_t copy_len = std::min(max_len, it->second.size());
    memcpy(buf, it->second.data(), copy_len);
    return copy_len;
}

size_t E2EEndpoint::get_received_data_size(uint32_t stream_id) {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = received_data.find(stream_id);
    if (it == received_data.end()) return 0;
    return it->second.size();
}

std::vector<uint8_t> E2EEndpoint::get_conn_proto_ext_received() {
    std::lock_guard<std::mutex> lock(mutex);
    return conn_proto_ext_received;
}

// ============================================================================
// E2EContext Implementation
// ============================================================================

void E2EContext::setup() {
    // Use port 0 to let OS assign available ports
    ASSERT_TRUE(server.init(0));
    ASSERT_TRUE(client.init(0));

    // Set peer addresses using dynamically assigned ports
    client.peer_addr.sin_family = AF_INET;
    client.peer_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    client.peer_addr.sin_port = htons(server.port);

    server.peer_addr.sin_family = AF_INET;
    server.peer_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    server.peer_addr.sin_port = htons(client.port);

    server.start();
    client.start();
}

void E2EContext::teardown() {
    client.stop();
    server.stop();
    client.cleanup();
    server.cleanup();
}

bool E2EContext::wait_until(std::function<bool()> condition, int timeout_ms) {
    auto start = std::chrono::steady_clock::now();
    while (!condition()) {
        auto elapsed = std::chrono::steady_clock::now() - start;
        if (std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() >= timeout_ms) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

bool E2EContext::wait_conn_state(E2EEndpoint &ep, int state, int timeout_ms) {
    return wait_until([&]() { return ep.conn_state.load() == state; }, timeout_ms);
}

bool E2EContext::wait_stream_state(E2EEndpoint &ep, int state, int timeout_ms) {
    return wait_until([&]() { return ep.stream_state.load() == state; }, timeout_ms);
}

bool E2EContext::wait_data_received(E2EEndpoint &ep, uint32_t stream_id, size_t min_bytes, int timeout_ms) {
    return wait_until([&]() { return ep.get_received_data_size(stream_id) >= min_bytes; }, timeout_ms);
}

bool E2EContext::wait_pending_send_complete(E2EEndpoint &ep, int timeout_ms) {
    return wait_until([&]() { return !ep.has_pending_send(); }, timeout_ms);
}

}  // namespace kcpmux_e2e
