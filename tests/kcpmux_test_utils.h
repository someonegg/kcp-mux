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
    struct TimerEvent {
        int64_t callback_time_ms;
        uint64_t delay_ms;
        int64_t deadline_ms;
    };

    struct TimerProbe {
        std::vector<TimerEvent> history;
        bool armed = false;
        int64_t deadline_ms = 0;

        void replace(int64_t now_ms, uint64_t delay_ms) {
            deadline_ms = delay_ms > (uint64_t)INT64_MAX - (uint64_t)now_ms
                ? INT64_MAX
                : now_ms + (int64_t)delay_ms;
            history.push_back({now_ms, delay_ms, deadline_ms});
            armed = true;
        }

        void consume() { armed = false; }

        void reset() {
            history.clear();
            armed = false;
            deadline_ms = 0;
        }
    } timer_probe;

    std::vector<std::vector<uint8_t>> sent_packets;
    int64_t current_time_ms = 0;
    uint64_t timer_ms = 0;

    // Connection notification records
    int conn_notify_count = 0;
    int conn_notify_result = KCPMUX_ACK_RESULT_OK;

    // Stream notification records
    int stream_notify_count = 0;
    int stream_notify_result = 0;

    // State change records
    std::vector<std::pair<uint8_t, uint8_t>> conn_state_changes;
    std::vector<std::pair<uint8_t, uint8_t>> stream_state_changes;

    // Data notification records
    int read_notify_count = 0;
    int write_notify_count = 0;

    // Close notification records
    int conn_close_count = 0;
    int conn_close_reason = 0;
    uint8_t conn_close_state = KCPMUX_CONN_STATE_ERROR;
    int stream_close_count = 0;
    int stream_close_reason = 0;
    uint8_t stream_close_state = KCPMUX_STREAM_STATE_ERROR;

    // Reset all members to initial state
    void reset() {
        sent_packets.clear();
        current_time_ms = 0;
        timer_ms = 0;
        timer_probe.reset();
        conn_notify_count = 0;
        conn_notify_result = KCPMUX_ACK_RESULT_OK;
        stream_notify_count = 0;
        stream_notify_result = 0;
        conn_state_changes.clear();
        stream_state_changes.clear();
        read_notify_count = 0;
        write_notify_count = 0;
        conn_close_count = 0;
        conn_close_reason = 0;
        conn_close_state = KCPMUX_CONN_STATE_ERROR;
        stream_close_count = 0;
        stream_close_reason = 0;
        stream_close_state = KCPMUX_STREAM_STATE_ERROR;
    }
};

// ============================================================================
// Callback functions
// ============================================================================

static void test_set_timer(uint64_t wake_after_ms, void *user_data) {
    TestContext *ctx = (TestContext *)user_data;
    ctx->timer_ms = wake_after_ms;
    ctx->timer_probe.replace(ctx->current_time_ms, wake_after_ms);
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
    TestContext *ctx = (TestContext *)user_data;
    ctx->conn_close_count++;
    ctx->conn_close_reason = reason;
    ctx->conn_close_state = conn->state;
}

static int test_stream_create_notify(kcpmux_stream_t *stream, void *user_data) {
    (void)stream;       // Unused
    TestContext *ctx = (TestContext *)user_data;
    ctx->stream_notify_count++;
    return ctx->stream_notify_result;
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
    TestContext *ctx = (TestContext *)user_data;
    ctx->stream_close_count++;
    ctx->stream_close_reason = reason;
    ctx->stream_close_state = stream->state;
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

static inline uint32_t read_u24(const uint8_t *buf) {
    return ((uint32_t)buf[0] << 16) |
           ((uint32_t)buf[1] << 8) |
           buf[2];
}

static inline uint32_t read_u32(const uint8_t *buf) {
    return ((uint32_t)buf[0] << 24) |
           ((uint32_t)buf[1] << 16) |
           ((uint32_t)buf[2] << 8) |
           buf[3];
}

static inline void write_common(uint8_t *buf, uint8_t type,
                                uint32_t generation_id) {
    buf[0] = type;
    write_u24(buf + 1, generation_id);
}

// Build CONN_CONNECT message: common(4) + version(1) + ext_len(2) + ext(N)
static inline std::vector<uint8_t> build_conn_connect(
        uint32_t generation_id = 1, const uint8_t *ext_data = nullptr,
        uint16_t ext_len = 0) {
    unsigned msg_len = 7 + ext_len;
    std::vector<uint8_t> buf(msg_len);
    write_common(buf.data(), KCPMUX_MSG_CONN_CONNECT, generation_id);
    buf[4] = KCPMUX_VERSION;
    write_u16(buf.data() + 5, ext_len);
    if (ext_data && ext_len > 0) {
        memcpy(buf.data() + 7, ext_data, ext_len);
    }
    return buf;
}

// Build CONN_CONNECT_ACK: common(4) + version(1) + result(1) + ext_len(2) + ext(N)
static inline std::vector<uint8_t> build_conn_connect_ack(
        uint8_t result, uint32_t generation_id,
        const uint8_t *ext_data = nullptr, uint16_t ext_len = 0) {
    unsigned msg_len = 8 + ext_len;
    std::vector<uint8_t> buf(msg_len);
    write_common(buf.data(), KCPMUX_MSG_CONN_CONNECT_ACK, generation_id);
    buf[4] = KCPMUX_VERSION;
    buf[5] = result;
    write_u16(buf.data() + 6, ext_len);
    if (ext_data && ext_len > 0) {
        memcpy(buf.data() + 8, ext_data, ext_len);
    }
    return buf;
}

// Build CONN_CLOSE message: common(4) + reason(1)
static inline std::vector<uint8_t> build_conn_close(
        uint8_t reason, uint32_t generation_id = 1) {
    std::vector<uint8_t> buf(5);
    write_common(buf.data(), KCPMUX_MSG_CONN_CLOSE, generation_id);
    buf[4] = reason;
    return buf;
}

// Build CONN_CLOSE_ACK message: common(4) + reason(1)
static inline std::vector<uint8_t> build_conn_close_ack(
        uint8_t reason, uint32_t generation_id = 1) {
    std::vector<uint8_t> buf(5);
    write_common(buf.data(), KCPMUX_MSG_CONN_CLOSE_ACK, generation_id);
    buf[4] = reason;
    return buf;
}

// Build CONN_KEEPALIVE message: common(4) + time(4) + seq(4)
static inline std::vector<uint8_t> build_conn_keepalive(
        uint32_t time, uint32_t seq, uint32_t generation_id = 1) {
    std::vector<uint8_t> buf(12);
    write_common(buf.data(), KCPMUX_MSG_CONN_KEEPALIVE, generation_id);
    write_u32(buf.data() + 4, time);
    write_u32(buf.data() + 8, seq);
    return buf;
}

// Build STREAM_CLOSE message: common(4) + stream_id(4) + reason(1)
static inline std::vector<uint8_t> build_stream_close(
        uint32_t stream_id, uint8_t reason, uint32_t generation_id = 1) {
    std::vector<uint8_t> buf(9);
    write_common(buf.data(), KCPMUX_MSG_STREAM_CLOSE, generation_id);
    write_u32(buf.data() + 4, stream_id);
    buf[8] = reason;
    return buf;
}

// Build STREAM_CLOSE_ACK message: common(4) + stream_id(4) + reason(1)
static inline std::vector<uint8_t> build_stream_close_ack(
        uint32_t stream_id, uint8_t reason, uint32_t generation_id = 1) {
    std::vector<uint8_t> buf(9);
    write_common(buf.data(), KCPMUX_MSG_STREAM_CLOSE_ACK, generation_id);
    write_u32(buf.data() + 4, stream_id);
    buf[8] = reason;
    return buf;
}

static inline std::vector<uint8_t> build_stream_payload(
        uint32_t stream_id, const uint8_t *payload, unsigned payload_len,
        uint32_t generation_id = 1) {
    std::vector<uint8_t> buf(8 + payload_len);
    write_common(buf.data(), KCPMUX_MSG_STREAM_PAYLOAD, generation_id);
    write_u32(buf.data() + 4, stream_id);
    if (payload && payload_len > 0) {
        memcpy(buf.data() + 8, payload, payload_len);
    }
    return buf;
}

}  // namespace kcpmux_test

#endif  // __KCPMUX_TEST_UTILS_H__
