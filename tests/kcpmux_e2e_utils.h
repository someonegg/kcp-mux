#ifndef __KCPMUX_E2E_UTILS_H__
#define __KCPMUX_E2E_UTILS_H__

#include <gtest/gtest.h>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <set>
#include <thread>
#include <vector>
#include <map>
#include <array>
#include <limits>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
typedef int socklen_t;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

extern "C" {
#include "kcpmux/kcpmux.h"
}

namespace kcpmux_e2e {

// ============================================================================
// Platform compatibility
// ============================================================================

#ifdef _WIN32
inline int close_socket(int fd) { return closesocket(fd); }
inline void init_sockets() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
}
inline void cleanup_sockets() { WSACleanup(); }
#else
inline int close_socket(int fd) { return close(fd); }
inline void init_sockets() {}
inline void cleanup_sockets() {}
#endif

// ============================================================================
// Time utilities
// ============================================================================

int64_t monotonic_time_ms();

// ============================================================================
// E2EEndpoint - Single endpoint (client or server)
// ============================================================================

struct E2EEndpoint {
    struct PendingSend {
        std::vector<uint8_t> data;
        size_t offset = 0;
    };

    // Network
    int udp_socket = -1;
    uint16_t port = 0;
    struct sockaddr_in local_addr{};
    struct sockaddr_in peer_addr{};

    // KCPMUX
    kcpmux_engine_t *engine = nullptr;

    // Threading
    std::thread worker;
    std::atomic<bool> running{false};
    std::atomic<int64_t> next_wakeup_ms{std::numeric_limits<int64_t>::max()};

    // Thread-safe state
    std::mutex mutex;

    // Accepted connections/streams (for server)
    std::vector<kcpmux_conn_t *> accepted_conns;
    std::vector<kcpmux_stream_t *> accepted_streams;
    std::map<uint32_t, kcpmux_stream_t *> active_streams;

    // Received data per stream (stream_id -> data)
    std::map<uint32_t, std::vector<uint8_t>> received_data;

    // Pending send data (for handling write block)
    std::map<uint32_t, PendingSend> pending_sends;

    // Connection state tracking
    kcpmux_conn_t *conn = nullptr;
    std::atomic<int> conn_state{0};
    std::atomic<int> conn_close_reason{-1};

    // Stream state tracking
    kcpmux_stream_t *stream = nullptr;
    std::atomic<int> stream_state{-1};
    std::atomic<int> stream_close_reason{-1};

    // Callback configuration
    int conn_notify_result = KCPMUX_ACK_RESULT_OK;

    // Extension data for connection
    std::vector<uint8_t> conn_proto_ext_to_send;      // Extension to send when connecting
    std::vector<uint8_t> conn_proto_ext_received;     // Extension received from peer
    std::vector<uint8_t> conn_proto_ext_response;     // Extension to respond (server side)

    // Statistics
    std::atomic<uint64_t> total_bytes_sent{0};
    std::atomic<uint64_t> total_bytes_received{0};

    // Lifecycle
    bool init(uint16_t bind_port);
    void start();
    void stop();
    void cleanup();

    // Actions (thread-safe, queued for worker thread)
    void connect_to(const struct sockaddr_in &addr);
    void connect_to_with_ext(const struct sockaddr_in &addr, const uint8_t *ext, size_t ext_len);
    void create_stream();
    void send_data(const uint8_t *data, size_t len);
    void send_data_with_retry(const uint8_t *data, size_t len);  // Handles write block
    void send_data_on_stream_with_retry(uint32_t stream_id,
                                        const uint8_t *data, size_t len);
    void close_conn();
    void close_stream();
    void queue_action(std::function<void()> action);
    void queue_stream_read(uint32_t stream_id);
    void queue_stream_write(uint32_t stream_id);

    // Query pending send state
    bool has_pending_send();
    bool has_active_stream(uint32_t stream_id);
    kcpmux_stream_t *get_primary_stream();
    void reset_primary_stream();

    // Queries (thread-safe)
    void clear_received_data(uint32_t stream_id);
    size_t get_received_data(uint32_t stream_id, uint8_t *buf, size_t max_len);
    size_t get_received_data_size(uint32_t stream_id);
    std::vector<uint8_t> get_conn_proto_ext_received();

private:
    void worker_func();
    void wake_worker();
    void process_due_timer(int64_t now);
    void process_readable_streams();
    void flush_pending_send(uint32_t stream_id);

    // Pending actions
    std::mutex action_mutex;
    std::vector<std::function<void()>> pending_actions;
    std::set<uint32_t> pending_read_stream_ids;
    bool read_action_queued = false;
    void process_actions();
};

// ============================================================================
// E2EContext - Client-Server test context
// ============================================================================

struct E2EContext {
    E2EEndpoint client;
    E2EEndpoint server;

    void setup();
    void teardown();

    // Wait helpers
    bool wait_until(std::function<bool()> condition, int timeout_ms = 5000);
    bool wait_conn_state(E2EEndpoint &ep, int state, int timeout_ms = 5000);
    bool wait_stream_state(E2EEndpoint &ep, int state, int timeout_ms = 5000);
    bool wait_data_received(E2EEndpoint &ep, uint32_t stream_id, size_t min_bytes, int timeout_ms = 5000);
    bool wait_pending_send_complete(E2EEndpoint &ep, int timeout_ms = 5000);
};

// ============================================================================
// KCPMUX Callbacks for E2E
// ============================================================================

void e2e_set_timer(uint64_t wake_after_ms, void *user_data);
int e2e_write_socket(const uint8_t *buf, unsigned size, const kcpmux_addr_t *addr, void *user_data);
int64_t e2e_monotonic_time_ms(void *user_data);
int e2e_conn_connect_notify(kcpmux_conn_t *conn, const kcpmux_proto_ext_t *proto_ext,
                            kcpmux_proto_ext_t *resp_proto_ext, void *user_data);
void e2e_conn_state_changed(kcpmux_conn_t *conn, uint8_t old_state, uint8_t new_state, void *user_data);
void e2e_conn_close_notify(kcpmux_conn_t *conn, int reason, void *user_data);
int e2e_stream_create_notify(kcpmux_stream_t *stream, void *user_data);
void e2e_stream_state_changed(kcpmux_stream_t *stream, uint8_t old_state, uint8_t new_state, void *user_data);
void e2e_stream_read_notify(kcpmux_stream_t *stream, void *user_data);
void e2e_stream_write_notify(kcpmux_stream_t *stream, void *user_data);
void e2e_stream_close_notify(kcpmux_stream_t *stream, int reason, void *user_data);

}  // namespace kcpmux_e2e

#endif  // __KCPMUX_E2E_UTILS_H__
