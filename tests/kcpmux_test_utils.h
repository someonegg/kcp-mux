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

    kcpmux_addr_t *get()
    {
        return &addr;
    }
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

        void replace(int64_t now_ms, uint64_t delay_ms)
        {
            deadline_ms = delay_ms > (uint64_t)INT64_MAX - (uint64_t)now_ms
                ? INT64_MAX
                : now_ms + (int64_t)delay_ms;
            history.push_back({now_ms, delay_ms, deadline_ms});
            armed = true;
        }

        void consume()
        {
            armed = false;
        }

        void reset()
        {
            history.clear();
            armed = false;
            deadline_ms = 0;
        }
    } timer_probe;

    std::vector<std::vector<uint8_t>> sent_packets;
    std::vector<unsigned> sent_iov_counts;
    std::vector<std::vector<std::vector<uint8_t>>> sent_iov_fragments;
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
    void reset()
    {
        sent_packets.clear();
        sent_iov_counts.clear();
        sent_iov_fragments.clear();
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

static void test_set_timer(uint64_t wake_after_ms, void *user_data)
{
    TestContext *ctx = (TestContext *)user_data;
    ctx->timer_ms = wake_after_ms;
    ctx->timer_probe.replace(ctx->current_time_ms, wake_after_ms);
}

static int test_write_socketv(
    const kcpmux_iovec_t *iov,
    unsigned iovcnt,
    const kcpmux_addr_t *addr,
    void *user_data)
{
    std::vector<uint8_t> packet;
    std::vector<std::vector<uint8_t>> fragments;
    (void)addr;  // Unused
    TestContext *ctx = (TestContext *)user_data;
    ctx->sent_iov_counts.push_back(iovcnt);
    for (unsigned i = 0; i < iovcnt; i++) {
        fragments.emplace_back(iov[i].data, iov[i].data + iov[i].len);
        packet.insert(packet.end(), iov[i].data, iov[i].data + iov[i].len);
    }
    ctx->sent_iov_fragments.push_back(std::move(fragments));
    ctx->sent_packets.push_back(std::move(packet));
    return 1;
}

static int64_t test_monotonic_time_ms(void *user_data)
{
    TestContext *ctx = (TestContext *)user_data;
    return ctx ? ctx->current_time_ms : 0;
}

static int test_conn_connect_notify(
    kcpmux_conn_t *conn,
    const kcpmux_proto_ext_t *proto_ext,
    kcpmux_proto_ext_t *resp_proto_ext,
    void *user_data)
{
    (void)conn;        // Unused
    (void)proto_ext;   // Unused
    resp_proto_ext->len = 0;
    TestContext *ctx = (TestContext *)user_data;
    ctx->conn_notify_count++;
    return ctx->conn_notify_result;
}

static void test_conn_state_changed(
    kcpmux_conn_t *conn,
    uint8_t old_state,
    uint8_t new_state,
    void *user_data)
{
    (void)conn;  // Unused
    TestContext *ctx = (TestContext *)user_data;
    ctx->conn_state_changes.emplace_back(old_state, new_state);
}

static void test_conn_close_notify(kcpmux_conn_t *conn, int reason, void *user_data)
{
    TestContext *ctx = (TestContext *)user_data;
    ctx->conn_close_count++;
    ctx->conn_close_reason = reason;
    ctx->conn_close_state = conn->state;
}

static int test_stream_create_notify(kcpmux_stream_t *stream, void *user_data)
{
    (void)stream;       // Unused
    TestContext *ctx = (TestContext *)user_data;
    ctx->stream_notify_count++;
    return ctx->stream_notify_result;
}

static void test_stream_state_changed(
    kcpmux_stream_t *stream,
    uint8_t old_state,
    uint8_t new_state,
    void *user_data)
{
    (void)stream;  // Unused
    TestContext *ctx = (TestContext *)user_data;
    ctx->stream_state_changes.emplace_back(old_state, new_state);
}

static void test_stream_read_notify(kcpmux_stream_t *stream, void *user_data)
{
    (void)stream;  // Unused
    TestContext *ctx = (TestContext *)user_data;
    ctx->read_notify_count++;
}

static void test_stream_write_notify(kcpmux_stream_t *stream, void *user_data)
{
    (void)stream;  // Unused
    TestContext *ctx = (TestContext *)user_data;
    ctx->write_notify_count++;
}

static void test_stream_close_notify(kcpmux_stream_t *stream, int reason, void *user_data)
{
    TestContext *ctx = (TestContext *)user_data;
    ctx->stream_close_count++;
    ctx->stream_close_reason = reason;
    ctx->stream_close_state = stream->state;
}

// ============================================================================
// Factory functions
// ============================================================================

static inline kcpmux_engine_t *create_test_engine(TestContext *ctx)
{
    kcpmux_engine_callbacks_t callbacks{};
    callbacks.set_timer = test_set_timer;
    callbacks.write_socketv = test_write_socketv;
    callbacks.monotonic_time_ms = test_monotonic_time_ms;
    callbacks.conn_connect_notify = test_conn_connect_notify;

    kcpmux_engine_t *engine = kcpmux_engine_create(nullptr, nullptr, nullptr, &callbacks, ctx, nullptr);
    if (engine) engine->next_connection_id = 1; // Deterministic test IDs.
    return engine;
}

static inline kcpmux_conn_callbacks_t create_conn_callbacks(TestContext *ctx)
{
    kcpmux_conn_callbacks_t callbacks{};
    callbacks.conn_state_changed = test_conn_state_changed;
    callbacks.conn_close_notify = test_conn_close_notify;
    callbacks.stream_create_notify = test_stream_create_notify;
    (void)ctx;  // Reserved for future use
    return callbacks;
}

static inline kcpmux_stream_callbacks_t create_stream_callbacks(TestContext *ctx)
{
    kcpmux_stream_callbacks_t callbacks{};
    callbacks.stream_state_changed = test_stream_state_changed;
    callbacks.stream_read_notify = test_stream_read_notify;
    callbacks.stream_write_notify = test_stream_write_notify;
    callbacks.stream_close_notify = test_stream_close_notify;
    (void)ctx;  // Reserved for future use
    return callbacks;
}

// Only for legacy single-connection fixtures. Multi-connection tests must use
// the explicit public address + local ID lookup.
static inline kcpmux_conn_t *find_only_conn(kcpmux_engine_t *engine, const kcpmux_addr_t *addr)
{
    kcpmux_conn_t *found = nullptr;
    for (int i = 0; i < engine->conn_map->size; i++) {
        list_head *node;
        list_for_each(node, &engine->conn_map->hashtable[i]) {
            auto *conn = list_entry(node, kcpmux_conn_t, hash_node);
            if (conn->peer_addr.addrlen == addr->addrlen &&
                memcmp(conn->peer_addr.addr, addr->addr, addr->addrlen) == 0) {
                EXPECT_EQ(found, nullptr) << "Single-connection fixture is ambiguous";
                found = kcpmux_engine_get_conn_by_id(engine, addr, conn->local_connection_id);
            }
        }
    }
    return found;
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
    void setup()
    {
        client_engine = create_test_engine(&client_ctx);
        server_engine = create_test_engine(&server_ctx);
        ASSERT_NE(client_engine, nullptr);
        ASSERT_NE(server_engine, nullptr);
    }

    // Teardown both engines
    void teardown()
    {
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
    void deliver_client_to_server()
    {
        for (const auto& packet : client_ctx.sent_packets) {
            kcpmux_engine_input(
                server_engine,
                packet.data(),
                (unsigned)packet.size(),
                client_addr.get());
        }
        client_ctx.sent_packets.clear();
    }

    // Deliver packets from server to client
    void deliver_server_to_client()
    {
        for (const auto& packet : server_ctx.sent_packets) {
            kcpmux_engine_input(
                client_engine,
                packet.data(),
                (unsigned)packet.size(),
                server_addr.get());
        }
        server_ctx.sent_packets.clear();
    }

    // Deliver packets bidirectionally
    void deliver_all()
    {
        deliver_client_to_server();
        deliver_server_to_client();
    }

    // Advance time for both contexts
    void advance_time(int64_t delta_ms)
    {
        client_ctx.current_time_ms += delta_ms;
        server_ctx.current_time_ms += delta_ms;
    }
};

// Run both engines around one bidirectional packet-delivery pass.
static inline void pump_kcp(DualEngineContext &ctx)
{
    kcpmux_engine_update(ctx.client_engine);
    kcpmux_engine_update(ctx.server_engine);
    ctx.deliver_all();
    kcpmux_engine_update(ctx.client_engine);
    kcpmux_engine_update(ctx.server_engine);
}

struct ConnectedPair {
    kcpmux_conn_t *client = nullptr;
    kcpmux_conn_t *server = nullptr;
};

struct StreamPair {
    kcpmux_stream_t *client = nullptr;
    kcpmux_stream_t *server = nullptr;
    uint32_t stream_id = 0;
};

class DualEngineTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ctx.setup();
    }

    void TearDown() override
    {
        ctx.teardown();
    }

    ConnectedPair connect_pair(const kcpmux_conn_config_t *config = nullptr)
    {
        ConnectedPair pair;
        kcpmux_conn_callbacks_t client_callbacks = create_conn_callbacks(&ctx.client_ctx);
        pair.client = kcpmux_conn_connect(
            ctx.client_engine,
            ctx.server_addr.get(),
            config,
            nullptr,
            &client_callbacks,
            &ctx.client_ctx);
        if (!pair.client) return pair;

        ctx.deliver_all();
        pair.server = kcpmux_engine_get_conn_by_id(
            ctx.server_engine, ctx.client_addr.get(), kcpmux_conn_get_peer_id(pair.client));
        if (!pair.server) return pair;

        kcpmux_conn_callbacks_t server_callbacks = create_conn_callbacks(&ctx.server_ctx);
        kcpmux_conn_set_callbacks(pair.server, &server_callbacks, &ctx.server_ctx);
        return pair;
    }

    StreamPair open_stream_pair(
        const ConnectedPair &connection,
        const kcpmux_stream_config_t *config = nullptr,
        uint8_t bootstrap = 0x7f)
    {
        StreamPair pair;
        if (!connection.client || !connection.server) return pair;

        kcpmux_stream_callbacks_t client_callbacks = create_stream_callbacks(&ctx.client_ctx);
        pair.client = kcpmux_stream_create(
            connection.client,
            config,
            &client_callbacks,
            &ctx.client_ctx);
        if (!pair.client) return pair;

        pair.stream_id = kcpmux_stream_id(pair.client);
        if (kcpmux_stream_send(pair.client, &bootstrap, 1, 1) != 1) return pair;
        pump_kcp(ctx);

        pair.server = kcpmux_conn_get_stream_by_id(connection.server, pair.stream_id);
        if (!pair.server) return pair;
        kcpmux_stream_callbacks_t server_callbacks = create_stream_callbacks(&ctx.server_ctx);
        kcpmux_stream_set_callbacks(pair.server, &server_callbacks, &ctx.server_ctx);
        pump_kcp(ctx);
        return pair;
    }

    DualEngineContext ctx;
};

// ============================================================================
// Protocol message builders
// ============================================================================

// Helper functions to write integers in network byte order
static inline void write_u16(uint8_t *buf, uint16_t val)
{
    buf[0] = (uint8_t)(val >> 8);
    buf[1] = (uint8_t)(val & 0xff);
}

static inline void write_u64(uint8_t *buf, uint64_t val)
{
    for (unsigned i = 0; i < 8; ++i) buf[i] = (uint8_t)(val >> (56 - 8 * i));
}

static inline void write_u32(uint8_t *buf, uint32_t val)
{
    buf[0] = (uint8_t)(val >> 24);
    buf[1] = (uint8_t)((val >> 16) & 0xff);
    buf[2] = (uint8_t)((val >> 8) & 0xff);
    buf[3] = (uint8_t)(val & 0xff);
}

static inline uint64_t read_u64(const uint8_t *buf)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value = (value << 8) | buf[i];
    return value;
}

static inline uint32_t read_u32(const uint8_t *buf)
{
    return ((uint32_t)buf[0] << 24) |
           ((uint32_t)buf[1] << 16) |
           ((uint32_t)buf[2] << 8) |
           buf[3];
}

static inline void write_common(uint8_t *buf, uint8_t type, uint64_t destination_id)
{
    buf[0] = type;
    write_u64(buf + 1, destination_id);
}

static inline std::vector<uint8_t> build_conn_connect(
    uint64_t source_id = 1,
    const uint8_t *ext_data = nullptr,
    uint16_t ext_len = 0)
{
    std::vector<uint8_t> buf(KCPMUX_PROTO_CONNECT_LEN + ext_len);
    write_common(buf.data(), KCPMUX_MSG_CONN_CONNECT, 0);
    write_u64(buf.data() + KCPMUX_PROTO_COMMON_LEN, source_id);
    buf[17] = KCPMUX_VERSION;
    write_u16(buf.data() + 18, ext_len);
    if (ext_data && ext_len) memcpy(buf.data() + KCPMUX_PROTO_CONNECT_LEN, ext_data, ext_len);
    return buf;
}

static inline std::vector<uint8_t> build_conn_connect_ack(
    uint8_t result, uint64_t destination_id,
    const uint8_t *ext_data = nullptr, uint16_t ext_len = 0,
    uint64_t source_id = 0x1000000000000001ULL)
{
    std::vector<uint8_t> buf(KCPMUX_PROTO_CONNECT_ACK_LEN + ext_len);
    write_common(buf.data(), KCPMUX_MSG_CONN_CONNECT_ACK, destination_id);
    write_u64(buf.data() + KCPMUX_PROTO_COMMON_LEN, source_id);
    buf[17] = KCPMUX_VERSION;
    buf[18] = result;
    write_u16(buf.data() + 19, ext_len);
    if (ext_data && ext_len) memcpy(buf.data() + KCPMUX_PROTO_CONNECT_ACK_LEN, ext_data, ext_len);
    return buf;
}

static inline std::vector<uint8_t> build_conn_close(uint8_t reason, uint64_t destination_id = 1)
{
    std::vector<uint8_t> buf(KCPMUX_PROTO_CONN_CLOSE_LEN);
    write_common(buf.data(), KCPMUX_MSG_CONN_CLOSE, destination_id);
    buf[KCPMUX_PROTO_COMMON_LEN] = reason;
    return buf;
}

static inline std::vector<uint8_t> build_conn_close_ack(uint8_t reason, uint64_t destination_id = 1)
{
    auto buf = build_conn_close(reason, destination_id);
    buf[0] = KCPMUX_MSG_CONN_CLOSE_ACK;
    return buf;
}

static inline std::vector<uint8_t> build_conn_keepalive(
    uint32_t time, uint32_t seq, uint64_t destination_id = 1)
{
    std::vector<uint8_t> buf(KCPMUX_PROTO_KEEPALIVE_LEN);
    write_common(buf.data(), KCPMUX_MSG_CONN_KEEPALIVE, destination_id);
    write_u32(buf.data() + KCPMUX_PROTO_COMMON_LEN, time);
    write_u32(buf.data() + KCPMUX_PROTO_COMMON_LEN + 4, seq);
    return buf;
}

static inline std::vector<uint8_t> build_stream_close(
    uint32_t stream_id, uint8_t reason, uint64_t destination_id = 1)
{
    std::vector<uint8_t> buf(KCPMUX_PROTO_STREAM_CLOSE_LEN);
    write_common(buf.data(), KCPMUX_MSG_STREAM_CLOSE, destination_id);
    write_u32(buf.data() + KCPMUX_PROTO_COMMON_LEN, stream_id);
    buf[KCPMUX_PROTO_STREAM_HEADER_LEN] = reason;
    return buf;
}

static inline std::vector<uint8_t> build_stream_close_ack(
    uint32_t stream_id, uint8_t reason, uint64_t destination_id = 1)
{
    auto buf = build_stream_close(stream_id, reason, destination_id);
    buf[0] = KCPMUX_MSG_STREAM_CLOSE_ACK;
    return buf;
}

static inline std::vector<uint8_t> build_stream_payload(
    uint32_t stream_id, const uint8_t *payload, unsigned payload_len,
    uint64_t destination_id = 1)
{
    std::vector<uint8_t> buf(KCPMUX_PROTO_STREAM_HEADER_LEN + payload_len);
    write_common(buf.data(), KCPMUX_MSG_STREAM_PAYLOAD, destination_id);
    write_u32(buf.data() + KCPMUX_PROTO_COMMON_LEN, stream_id);
    if (payload && payload_len) memcpy(buf.data() + KCPMUX_PROTO_STREAM_HEADER_LEN, payload, payload_len);
    return buf;
}

}  // namespace kcpmux_test

#endif  // __KCPMUX_TEST_UTILS_H__
