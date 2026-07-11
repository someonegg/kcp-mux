#ifndef __KCPMUX_TEST_UTILS_H__
#define __KCPMUX_TEST_UTILS_H__

#include <gtest/gtest.h>
#include <cstdint>
#include <cstring>
#include <vector>
#include <functional>

extern "C" {
#include "kcpmux/kcpmux.h"
#include "kcpmux_engine.h"
#include "kcpmux_conn.h"
#include "kcpmux_stream.h"
#include "kcpmux_protocol.h"
}

// ============================================================================
// kcpmux_test namespace
// ============================================================================

namespace kcpmux_test {

// ============================================================================
// TestAddr - Test address helper for IP:Port
// ============================================================================

struct TestAddr {
    uint8_t data[6];  // IP(4) + Port(2)
    kcpmux_addr_t addr;

    TestAddr(uint32_t ip, uint16_t port) {
        data[0] = (ip >> 24) & 0xff;
        data[1] = (ip >> 16) & 0xff;
        data[2] = (ip >> 8) & 0xff;
        data[3] = ip & 0xff;
        data[4] = (port >> 8) & 0xff;
        data[5] = port & 0xff;
        addr.addr = data;
        addr.addrlen = 6;
    }

    kcpmux_addr_t* get() { return &addr; }
    const kcpmux_addr_t* get() const { return &addr; }
};

// ============================================================================
// TestContext - Single engine test context with packet capture,
//               callback records, state change tracking
// ============================================================================

struct TestContext {
    std::vector<std::vector<uint8_t>> sent_packets;
    int64_t current_time_ms = 0;
    uint64_t timer_ms = 0;

    // Connection notification records
    int conn_notify_count = 0;
    int conn_notify_result = KCPMUX_ACK_RESULT_OK;

    // Stream notification records
    int stream_notify_count = 0;

    // State change records
    std::vector<std::pair<uint8_t, uint8_t>> conn_state_changes;
    std::vector<std::pair<uint8_t, uint8_t>> stream_state_changes;

    // Data notification records
    int read_notify_count = 0;
    int write_notify_count = 0;

    // Close notification records
    int conn_close_count = 0;
    int conn_close_reason = 0;
    int stream_close_count = 0;
    int stream_close_reason = 0;

    // Reset all members to initial state
    void reset() {
        sent_packets.clear();
        current_time_ms = 0;
        timer_ms = 0;
        conn_notify_count = 0;
        conn_notify_result = KCPMUX_ACK_RESULT_OK;
        stream_notify_count = 0;
        conn_state_changes.clear();
        stream_state_changes.clear();
        read_notify_count = 0;
        write_notify_count = 0;
        conn_close_count = 0;
        conn_close_reason = 0;
        stream_close_count = 0;
        stream_close_reason = 0;
    }
};

// ============================================================================
// Callback functions
// ============================================================================

static void test_set_timer(uint64_t wake_after_ms, void *user_data) {
    TestContext *ctx = (TestContext *)user_data;
    ctx->timer_ms = wake_after_ms;
}

static int test_write_socket(const uint8_t *buf, unsigned size,
                             const kcpmux_addr_t *addr, void *user_data) {
    (void)addr;  // Unused
    TestContext *ctx = (TestContext *)user_data;
    ctx->sent_packets.emplace_back(buf, buf + size);
    return 1;
}

static int64_t test_monotonic_time_ms(void *user_data) {
    TestContext *ctx = (TestContext *)user_data;
    return ctx ? ctx->current_time_ms : 0;
}

static int test_conn_connect_notify(kcpmux_conn_t *conn,
                                    const kcpmux_proto_ext_t *proto_ext,
                                    kcpmux_proto_ext_t *resp_proto_ext,
                                    void *user_data) {
    (void)conn;        // Unused
    (void)proto_ext;   // Unused
    (void)resp_proto_ext;  // Unused
    TestContext *ctx = (TestContext *)user_data;
    ctx->conn_notify_count++;
    return ctx->conn_notify_result;
}

static void test_conn_state_changed(kcpmux_conn_t *conn, uint8_t old_state,
                                    uint8_t new_state, void *user_data) {
    (void)conn;  // Unused
    TestContext *ctx = (TestContext *)user_data;
    ctx->conn_state_changes.emplace_back(old_state, new_state);
}

static void test_conn_close_notify(kcpmux_conn_t *conn, int reason, void *user_data) {
    (void)conn;     // Unused
    TestContext *ctx = (TestContext *)user_data;
    ctx->conn_close_count++;
    ctx->conn_close_reason = reason;
}

static void test_stream_create_notify(kcpmux_stream_t *stream, void *user_data) {
    (void)stream;       // Unused
    TestContext *ctx = (TestContext *)user_data;
    ctx->stream_notify_count++;
}

static void test_stream_state_changed(kcpmux_stream_t *stream, uint8_t old_state,
                                      uint8_t new_state, void *user_data) {
    (void)stream;  // Unused
    TestContext *ctx = (TestContext *)user_data;
    ctx->stream_state_changes.emplace_back(old_state, new_state);
}

static void test_stream_read_notify(kcpmux_stream_t *stream, void *user_data) {
    (void)stream;  // Unused
    TestContext *ctx = (TestContext *)user_data;
    ctx->read_notify_count++;
}

static void test_stream_write_notify(kcpmux_stream_t *stream, void *user_data) {
    (void)stream;  // Unused
    TestContext *ctx = (TestContext *)user_data;
    ctx->write_notify_count++;
}

static void test_stream_close_notify(kcpmux_stream_t *stream, int reason, void *user_data) {
    (void)stream;   // Unused
    TestContext *ctx = (TestContext *)user_data;
    ctx->stream_close_count++;
    ctx->stream_close_reason = reason;
}

// ============================================================================
// Factory functions
// ============================================================================

static inline kcpmux_engine_t *create_test_engine(TestContext *ctx) {
    kcpmux_engine_callbacks_t callbacks{};
    callbacks.set_timer = test_set_timer;
    callbacks.write_socket = test_write_socket;
    callbacks.monotonic_time_ms = test_monotonic_time_ms;
    callbacks.conn_connect_notify = test_conn_connect_notify;

    return kcpmux_engine_create(nullptr, nullptr, nullptr, &callbacks, ctx, nullptr);
}

static inline kcpmux_conn_callbacks_t create_conn_callbacks(TestContext *ctx) {
    kcpmux_conn_callbacks_t callbacks{};
    callbacks.conn_state_changed = test_conn_state_changed;
    callbacks.conn_close_notify = test_conn_close_notify;
    callbacks.stream_create_notify = test_stream_create_notify;
    (void)ctx;  // Reserved for future use
    return callbacks;
}

static inline kcpmux_stream_callbacks_t create_stream_callbacks(TestContext *ctx) {
    kcpmux_stream_callbacks_t callbacks{};
    callbacks.stream_state_changed = test_stream_state_changed;
    callbacks.stream_read_notify = test_stream_read_notify;
    callbacks.stream_write_notify = test_stream_write_notify;
    callbacks.stream_close_notify = test_stream_close_notify;
    (void)ctx;  // Reserved for future use
    return callbacks;
}

// ============================================================================
// DualEngineContext - Dual engine test context with setup/teardown,
//                     packet routing methods
// ============================================================================

struct DualEngineContext {
    TestContext client_ctx;
    TestContext server_ctx;
    kcpmux_engine_t *client_engine = nullptr;
    kcpmux_engine_t *server_engine = nullptr;
    TestAddr client_addr{0x7F000001, 10001};  // 127.0.0.1:10001
    TestAddr server_addr{0x7F000001, 10002};  // 127.0.0.1:10002

    // Setup both engines
    void setup() {
        client_engine = create_test_engine(&client_ctx);
        server_engine = create_test_engine(&server_ctx);
        ASSERT_NE(client_engine, nullptr);
        ASSERT_NE(server_engine, nullptr);
    }

    // Teardown both engines
    void teardown() {
        if (client_engine) {
            kcpmux_engine_destroy(client_engine);
            client_engine = nullptr;
        }
        if (server_engine) {
            kcpmux_engine_destroy(server_engine);
            server_engine = nullptr;
        }
    }

    // Deliver packets from client to server
    void deliver_client_to_server() {
        for (const auto& packet : client_ctx.sent_packets) {
            kcpmux_engine_input(server_engine, packet.data(), (unsigned)packet.size(), client_addr.get());
        }
        client_ctx.sent_packets.clear();
    }

    // Deliver packets from server to client
    void deliver_server_to_client() {
        for (const auto& packet : server_ctx.sent_packets) {
            kcpmux_engine_input(client_engine, packet.data(), (unsigned)packet.size(), server_addr.get());
        }
        server_ctx.sent_packets.clear();
    }

    // Deliver packets bidirectionally
    void deliver_all() {
        deliver_client_to_server();
        deliver_server_to_client();
    }

    // Advance time for both contexts
    void advance_time(int64_t delta_ms) {
        client_ctx.current_time_ms += delta_ms;
        server_ctx.current_time_ms += delta_ms;
    }
};

// ============================================================================
// Protocol message builders
// ============================================================================

// Helper functions to write integers in network byte order
static inline void write_u16(uint8_t *buf, uint16_t val) {
    buf[0] = (uint8_t)(val >> 8);
    buf[1] = (uint8_t)(val & 0xff);
}

static inline void write_u24(uint8_t *buf, uint32_t val) {
    buf[0] = (uint8_t)(val >> 16);
    buf[1] = (uint8_t)((val >> 8) & 0xff);
    buf[2] = (uint8_t)(val & 0xff);
}

static inline void write_u32(uint8_t *buf, uint32_t val) {
    buf[0] = (uint8_t)(val >> 24);
    buf[1] = (uint8_t)((val >> 16) & 0xff);
    buf[2] = (uint8_t)((val >> 8) & 0xff);
    buf[3] = (uint8_t)(val & 0xff);
}

// Build CONN_CONNECT message: type(1) + version(1) + ext_len(2) + ext(N)
static inline std::vector<uint8_t> build_conn_connect(
        const uint8_t *ext_data = nullptr, uint16_t ext_len = 0) {
    unsigned msg_len = 4 + ext_len;
    std::vector<uint8_t> buf(msg_len);
    buf[0] = KCPMUX_MSG_CONN_CONNECT;
    buf[1] = KCPMUX_VERSION;
    write_u16(buf.data() + 2, ext_len);
    if (ext_data && ext_len > 0) {
        memcpy(buf.data() + 4, ext_data, ext_len);
    }
    return buf;
}

// Build CONN_CONNECT_ACK message: type(1) + version(1) + result(1) + ext_len(2) + ext(N)
static inline std::vector<uint8_t> build_conn_connect_ack(
        uint8_t result, const uint8_t *ext_data = nullptr, uint16_t ext_len = 0) {
    unsigned msg_len = 5 + ext_len;
    std::vector<uint8_t> buf(msg_len);
    buf[0] = KCPMUX_MSG_CONN_CONNECT_ACK;
    buf[1] = KCPMUX_VERSION;
    buf[2] = result;
    write_u16(buf.data() + 3, ext_len);
    if (ext_data && ext_len > 0) {
        memcpy(buf.data() + 5, ext_data, ext_len);
    }
    return buf;
}

// Build CONN_CLOSE message: type(1) + reason(1)
static inline std::vector<uint8_t> build_conn_close(uint8_t reason) {
    std::vector<uint8_t> buf(2);
    buf[0] = KCPMUX_MSG_CONN_CLOSE;
    buf[1] = reason;
    return buf;
}

// Build CONN_CLOSE_ACK message: type(1) + reason(1)
static inline std::vector<uint8_t> build_conn_close_ack(uint8_t reason) {
    std::vector<uint8_t> buf(2);
    buf[0] = KCPMUX_MSG_CONN_CLOSE_ACK;
    buf[1] = reason;
    return buf;
}

// Build CONN_KEEPALIVE message: type(1) + time(4) + seq(4)
static inline std::vector<uint8_t> build_conn_keepalive(uint32_t time, uint32_t seq) {
    std::vector<uint8_t> buf(9);
    buf[0] = KCPMUX_MSG_CONN_KEEPALIVE;
    write_u32(buf.data() + 1, time);
    write_u32(buf.data() + 5, seq);
    return buf;
}

// Build STREAM_CLOSE message: type(1) + stream_id(3) + reason(1)
static inline std::vector<uint8_t> build_stream_close(uint32_t stream_id, uint8_t reason) {
    std::vector<uint8_t> buf(5);
    buf[0] = KCPMUX_MSG_STREAM_CLOSE;
    write_u24(buf.data() + 1, stream_id);
    buf[4] = reason;
    return buf;
}

// Build STREAM_CLOSE_ACK message: type(1) + stream_id(3) + reason(1)
static inline std::vector<uint8_t> build_stream_close_ack(uint32_t stream_id, uint8_t reason) {
    std::vector<uint8_t> buf(5);
    buf[0] = KCPMUX_MSG_STREAM_CLOSE_ACK;
    write_u24(buf.data() + 1, stream_id);
    buf[4] = reason;
    return buf;
}

}  // namespace kcpmux_test

#endif  // __KCPMUX_TEST_UTILS_H__
