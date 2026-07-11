#include "kcpmux_test_utils.h"

using namespace kcpmux_test;

namespace {

// ============================================================================
// Engine Tests
// ============================================================================

TEST(kcpmux_lifecycle, engine_create_destroy) {
    TestContext ctx;
    kcpmux_engine_t *engine = create_test_engine(&ctx);

    ASSERT_NE(engine, nullptr);
    EXPECT_GT(ctx.timer_ms, 0u);

    kcpmux_engine_destroy(engine);
}

TEST(kcpmux_lifecycle, engine_create_null_callbacks) {
    kcpmux_engine_t *engine = kcpmux_engine_create(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    EXPECT_EQ(engine, nullptr);
}

TEST(kcpmux_lifecycle, engine_config_default_values) {
    kcpmux_conn_config_t conn_config;
    kcpmux_conn_config_init(&conn_config);

    EXPECT_EQ(conn_config.ctrl_timeout_ms, KCPMUX_DEFAULT_CONTROL_TIMEOUT_MS);
    EXPECT_EQ(conn_config.connect_retries, KCPMUX_DEFAULT_CONNECT_RETRIES);
    EXPECT_EQ(conn_config.keepalive_interval_ms, KCPMUX_DEFAULT_KEEPALIVE_INTERVAL_MS);
    EXPECT_EQ(conn_config.keepalive_timeout_ms, KCPMUX_DEFAULT_KEEPALIVE_TIMEOUT_MS);

    kcpmux_stream_config_t stream_config;
    kcpmux_stream_config_init(&stream_config);

    EXPECT_EQ(stream_config.ctrl_timeout_ms, KCPMUX_DEFAULT_SCONTROL_TIMEOUT_MS);
    EXPECT_EQ(stream_config.close_retries, KCPMUX_DEFAULT_SCLOSE_RETRIES);
}

// ============================================================================
// Connection Normal Flow Tests (Dual Engine)
// ============================================================================

TEST(kcpmux_lifecycle, conn_connect_success) {
    DualEngineContext ctx;
    ctx.setup();

    // Client initiates connection
    kcpmux_conn_callbacks_t client_callbacks = create_conn_callbacks(&ctx.client_ctx);
    kcpmux_conn_t *client_conn = kcpmux_conn_connect(
        ctx.client_engine, ctx.server_addr.get(), nullptr, nullptr,
        &client_callbacks, &ctx.client_ctx);
    ASSERT_NE(client_conn, nullptr);
    EXPECT_EQ(kcpmux_conn_get_state(client_conn), KCPMUX_CONN_STATE_CONNECTING);

    // Deliver CONN_CONNECT to server
    ctx.deliver_client_to_server();

    // Server should have accepted and created connection
    kcpmux_conn_t *server_conn = kcpmux_engine_get_conn_by_addr(
        ctx.server_engine, ctx.client_addr.get());
    ASSERT_NE(server_conn, nullptr);
    EXPECT_EQ(kcpmux_conn_get_state(server_conn), KCPMUX_CONN_STATE_CONNECTED);

    // Deliver CONN_CONNECT_ACK to client
    ctx.deliver_server_to_client();

    // Client should be connected
    EXPECT_EQ(kcpmux_conn_get_state(client_conn), KCPMUX_CONN_STATE_CONNECTED);

    ctx.teardown();
}

TEST(kcpmux_lifecycle, conn_close_initiator) {
    DualEngineContext ctx;
    ctx.setup();

    // Establish connection
    kcpmux_conn_callbacks_t client_callbacks = create_conn_callbacks(&ctx.client_ctx);
    kcpmux_conn_t *client_conn = kcpmux_conn_connect(
        ctx.client_engine, ctx.server_addr.get(), nullptr, nullptr,
        &client_callbacks, &ctx.client_ctx);
    ctx.deliver_all();

    kcpmux_conn_t *server_conn = kcpmux_engine_get_conn_by_addr(
        ctx.server_engine, ctx.client_addr.get());
    kcpmux_conn_callbacks_t server_callbacks = create_conn_callbacks(&ctx.server_ctx);
    kcpmux_conn_set_callbacks(server_conn, &server_callbacks, &ctx.server_ctx);

    // Client closes connection
    ctx.client_ctx.conn_state_changes.clear();
    kcpmux_conn_close(client_conn);
    EXPECT_EQ(kcpmux_conn_get_state(client_conn), KCPMUX_CONN_STATE_CLOSING);

    // Deliver CONN_CLOSE to server
    ctx.deliver_client_to_server();
    EXPECT_EQ(kcpmux_conn_get_state(server_conn), KCPMUX_CONN_STATE_CLOSED);

    // Deliver CONN_CLOSE_ACK to client
    ctx.deliver_server_to_client();
    EXPECT_EQ(kcpmux_conn_get_state(client_conn), KCPMUX_CONN_STATE_CLOSED);

    ctx.teardown();
}

TEST(kcpmux_lifecycle, conn_state_callbacks) {
    DualEngineContext ctx;
    ctx.setup();

    // Establish connection with callbacks
    kcpmux_conn_callbacks_t client_callbacks = create_conn_callbacks(&ctx.client_ctx);
    ctx.client_ctx.conn_state_changes.clear();

    kcpmux_conn_t *client_conn = kcpmux_conn_connect(
        ctx.client_engine, ctx.server_addr.get(), nullptr, nullptr,
        &client_callbacks, &ctx.client_ctx);
    (void)client_conn;

    // Initial state change: INIT -> CONNECTING
    ASSERT_GE(ctx.client_ctx.conn_state_changes.size(), 1u);
    EXPECT_EQ(ctx.client_ctx.conn_state_changes[0].first, KCPMUX_CONN_STATE_INIT);
    EXPECT_EQ(ctx.client_ctx.conn_state_changes[0].second, KCPMUX_CONN_STATE_CONNECTING);

    // Complete handshake
    ctx.deliver_all();

    // Should have state change: CONNECTING -> CONNECTED
    bool found_connected = false;
    for (auto &change : ctx.client_ctx.conn_state_changes) {
        if (change.first == KCPMUX_CONN_STATE_CONNECTING &&
            change.second == KCPMUX_CONN_STATE_CONNECTED) {
            found_connected = true;
            break;
        }
    }
    EXPECT_TRUE(found_connected);

    ctx.teardown();
}

// ============================================================================
// Connection Error Handling Tests
// ============================================================================

TEST(kcpmux_lifecycle, conn_connect_rejected) {
    DualEngineContext ctx;
    ctx.setup();

    // Server will reject connections
    ctx.server_ctx.conn_notify_result = KCPMUX_ACK_RESULT_ERROR;

    // Client initiates connection
    kcpmux_conn_callbacks_t client_callbacks = create_conn_callbacks(&ctx.client_ctx);
    kcpmux_conn_t *client_conn = kcpmux_conn_connect(
        ctx.client_engine, ctx.server_addr.get(), nullptr, nullptr,
        &client_callbacks, &ctx.client_ctx);

    // Deliver CONN_CONNECT to server (server rejects)
    ctx.deliver_client_to_server();

    // Deliver reject ACK to client
    ctx.deliver_server_to_client();

    // Client should be in ERROR state
    EXPECT_EQ(kcpmux_conn_get_state(client_conn), KCPMUX_CONN_STATE_ERROR);
    EXPECT_EQ(ctx.client_ctx.conn_close_count, 1);
    EXPECT_EQ(ctx.client_ctx.conn_close_reason, KCPMUX_CLOSE_REASON_REJECTED);

    ctx.teardown();
}

TEST(kcpmux_lifecycle, conn_duplicate_connect) {
    TestContext ctx;
    kcpmux_engine_t *engine = create_test_engine(&ctx);
    ASSERT_NE(engine, nullptr);

    TestAddr addr(0x7f000001, 12345);

    kcpmux_conn_t *conn1 = kcpmux_conn_connect(engine, addr.get(), nullptr, nullptr, nullptr, nullptr);
    ASSERT_NE(conn1, nullptr);

    kcpmux_conn_t *conn2 = kcpmux_conn_connect(engine, addr.get(), nullptr, nullptr, nullptr, nullptr);
    EXPECT_EQ(conn2, nullptr);

    kcpmux_engine_destroy(engine);
}

TEST(kcpmux_lifecycle, conn_get_by_addr) {
    TestContext ctx;
    kcpmux_engine_t *engine = create_test_engine(&ctx);
    ASSERT_NE(engine, nullptr);

    TestAddr addr(0x7f000001, 12345);

    kcpmux_conn_t *conn = kcpmux_conn_connect(engine, addr.get(), nullptr, nullptr, nullptr, nullptr);
    ASSERT_NE(conn, nullptr);

    kcpmux_conn_t *found = kcpmux_engine_get_conn_by_addr(engine, addr.get());
    EXPECT_EQ(found, conn);

    TestAddr other_addr(0x7f000002, 12345);
    kcpmux_conn_t *not_found = kcpmux_engine_get_conn_by_addr(engine, other_addr.get());
    EXPECT_EQ(not_found, nullptr);

    kcpmux_engine_destroy(engine);
}

// ============================================================================
// Connection Timeout Tests
// ============================================================================

TEST(kcpmux_lifecycle, conn_connect_timeout_retry) {
    TestContext ctx;
    ctx.current_time_ms = 1000;  // Start at 1000ms

    // Create engine with custom config for faster timeout
    kcpmux_conn_config_t conn_config;
    kcpmux_conn_config_init(&conn_config);
    conn_config.ctrl_timeout_ms = 100;     // 100ms timeout
    conn_config.connect_retries = 2;       // 2 retries

    kcpmux_engine_t *engine = create_test_engine(&ctx);
    ASSERT_NE(engine, nullptr);

    TestAddr addr(0x7f000001, 12345);
    kcpmux_conn_callbacks_t callbacks = create_conn_callbacks(&ctx);

    // Client initiates connection
    kcpmux_conn_t *conn = kcpmux_conn_connect(
        engine, addr.get(), &conn_config, nullptr, &callbacks, &ctx);
    ASSERT_NE(conn, nullptr);
    EXPECT_EQ(kcpmux_conn_get_state(conn), KCPMUX_CONN_STATE_CONNECTING);

    // Initial CONN_CONNECT should be sent
    EXPECT_EQ(ctx.sent_packets.size(), 1u);
    ctx.sent_packets.clear();

    // Advance time past first timeout (100ms)
    ctx.current_time_ms += 101;
    kcpmux_engine_update(engine);

    // First retry should be sent
    EXPECT_EQ(ctx.sent_packets.size(), 1u);
    EXPECT_EQ(kcpmux_conn_get_state(conn), KCPMUX_CONN_STATE_CONNECTING);
    ctx.sent_packets.clear();

    // Advance time past second timeout (100ms)
    ctx.current_time_ms += 101;
    kcpmux_engine_update(engine);

    // Second retry should be sent
    EXPECT_EQ(ctx.sent_packets.size(), 1u);
    EXPECT_EQ(kcpmux_conn_get_state(conn), KCPMUX_CONN_STATE_CONNECTING);
    ctx.sent_packets.clear();

    // Advance time past third timeout (retries exhausted)
    ctx.current_time_ms += 101;
    kcpmux_engine_update(engine);

    // Connection should be in ERROR state
    EXPECT_EQ(kcpmux_conn_get_state(conn), KCPMUX_CONN_STATE_ERROR);
    EXPECT_EQ(ctx.conn_close_count, 1);  // close_notify should be called
    EXPECT_EQ(ctx.conn_close_reason, KCPMUX_CLOSE_REASON_TIMEOUT);

    kcpmux_engine_destroy(engine);
}

TEST(kcpmux_lifecycle, conn_keepalive_timeout) {
    DualEngineContext ctx;
    ctx.client_ctx.current_time_ms = 1000;
    ctx.server_ctx.current_time_ms = 1000;
    ctx.setup();

    // Create custom config with short keepalive timeout
    kcpmux_conn_config_t conn_config;
    kcpmux_conn_config_init(&conn_config);
    conn_config.keepalive_timeout_ms = 500;   // 500ms keepalive timeout
    conn_config.keepalive_interval_ms = 100;  // 100ms keepalive interval

    // Client initiates connection
    kcpmux_conn_callbacks_t client_callbacks = create_conn_callbacks(&ctx.client_ctx);
    kcpmux_conn_t *client_conn = kcpmux_conn_connect(
        ctx.client_engine, ctx.server_addr.get(), &conn_config, nullptr,
        &client_callbacks, &ctx.client_ctx);
    ASSERT_NE(client_conn, nullptr);

    // Complete handshake
    ctx.deliver_all();
    EXPECT_EQ(kcpmux_conn_get_state(client_conn), KCPMUX_CONN_STATE_CONNECTED);

    // Advance time past keepalive timeout without any data exchange
    ctx.client_ctx.current_time_ms += 501;
    ctx.server_ctx.current_time_ms += 501;
    kcpmux_engine_update(ctx.client_engine);

    // Client connection should be in ERROR state due to keepalive timeout
    EXPECT_EQ(kcpmux_conn_get_state(client_conn), KCPMUX_CONN_STATE_ERROR);
    EXPECT_EQ(ctx.client_ctx.conn_close_count, 1);
    EXPECT_EQ(ctx.client_ctx.conn_close_reason, KCPMUX_CLOSE_REASON_TIMEOUT);

    ctx.teardown();
}

TEST(kcpmux_lifecycle, conn_idle_timeout) {
    DualEngineContext ctx;
    ctx.client_ctx.current_time_ms = 1000;
    ctx.server_ctx.current_time_ms = 1000;
    ctx.setup();

    // Create custom config with short idle timeout
    kcpmux_conn_config_t conn_config;
    kcpmux_conn_config_init(&conn_config);
    conn_config.idle_timeout_ms = 500;          // 500ms idle timeout
    conn_config.keepalive_timeout_ms = 10000;   // Long keepalive timeout (won't trigger)
    conn_config.keepalive_interval_ms = 100;    // 100ms keepalive interval

    // Client initiates connection
    kcpmux_conn_callbacks_t client_callbacks = create_conn_callbacks(&ctx.client_ctx);
    kcpmux_conn_t *client_conn = kcpmux_conn_connect(
        ctx.client_engine, ctx.server_addr.get(), &conn_config, nullptr,
        &client_callbacks, &ctx.client_ctx);
    ASSERT_NE(client_conn, nullptr);

    // Complete handshake
    ctx.deliver_all();
    EXPECT_EQ(kcpmux_conn_get_state(client_conn), KCPMUX_CONN_STATE_CONNECTED);

    // Simulate keepalive exchange to prevent keepalive timeout
    // But no payload data is exchanged
    for (int i = 0; i < 4; i++) {
        ctx.advance_time(100);
        kcpmux_engine_update(ctx.client_engine);
        kcpmux_engine_update(ctx.server_engine);
        ctx.deliver_all();
    }

    // Advance time past idle timeout
    ctx.advance_time(200);
    kcpmux_engine_update(ctx.client_engine);

    // Client connection should close due to idle timeout
    uint8_t state = kcpmux_conn_get_state(client_conn);
    EXPECT_TRUE(state == KCPMUX_CONN_STATE_CLOSING ||
                state == KCPMUX_CONN_STATE_CLOSED);
    EXPECT_EQ(ctx.client_ctx.conn_close_count, 1);
    EXPECT_EQ(ctx.client_ctx.conn_close_reason, KCPMUX_CLOSE_REASON_IDLE);

    ctx.teardown();
}

TEST(kcpmux_lifecycle, conn_close_timeout) {
    DualEngineContext ctx;
    ctx.client_ctx.current_time_ms = 1000;
    ctx.server_ctx.current_time_ms = 1000;
    ctx.setup();

    // Create custom config with short close timeout
    kcpmux_conn_config_t conn_config;
    kcpmux_conn_config_init(&conn_config);
    conn_config.ctrl_timeout_ms = 100;    // 100ms timeout
    conn_config.close_retries = 1;        // 1 retry

    // Establish connection
    kcpmux_conn_callbacks_t client_callbacks = create_conn_callbacks(&ctx.client_ctx);
    kcpmux_conn_t *client_conn = kcpmux_conn_connect(
        ctx.client_engine, ctx.server_addr.get(), &conn_config, nullptr,
        &client_callbacks, &ctx.client_ctx);
    ctx.deliver_all();
    EXPECT_EQ(kcpmux_conn_get_state(client_conn), KCPMUX_CONN_STATE_CONNECTED);

    // Client closes connection
    ctx.client_ctx.conn_close_count = 0;
    kcpmux_conn_close(client_conn);
    EXPECT_EQ(kcpmux_conn_get_state(client_conn), KCPMUX_CONN_STATE_CLOSING);

    // Don't deliver the CLOSE message (simulate packet loss)
    ctx.client_ctx.sent_packets.clear();

    // Advance time past first timeout
    ctx.advance_time(101);
    kcpmux_engine_update(ctx.client_engine);

    // First retry should be sent
    EXPECT_EQ(kcpmux_conn_get_state(client_conn), KCPMUX_CONN_STATE_CLOSING);
    ctx.client_ctx.sent_packets.clear();

    // Advance time past second timeout (retries exhausted)
    ctx.advance_time(101);
    kcpmux_engine_update(ctx.client_engine);

    // Connection should be CLOSED (close timeout doesn't go to ERROR)
    EXPECT_EQ(kcpmux_conn_get_state(client_conn), KCPMUX_CONN_STATE_CLOSED);
    EXPECT_EQ(ctx.client_ctx.conn_close_count, 1);
    EXPECT_EQ(ctx.client_ctx.conn_close_reason, KCPMUX_CLOSE_REASON_NORMAL);

    ctx.teardown();
}

// ============================================================================
// Stream Timeout Tests
// ============================================================================

TEST(kcpmux_lifecycle, stream_close_timeout) {
    DualEngineContext ctx;
    ctx.client_ctx.current_time_ms = 1000;
    ctx.server_ctx.current_time_ms = 1000;
    ctx.setup();

    // Create custom stream config with short timeout
    kcpmux_stream_config_t stream_config;
    kcpmux_stream_config_init(&stream_config);
    stream_config.ctrl_timeout_ms = 100;   // 100ms timeout
    stream_config.close_retries = 1;       // 1 retry

    // Establish connection and stream
    kcpmux_conn_callbacks_t client_callbacks = create_conn_callbacks(&ctx.client_ctx);
    kcpmux_conn_t *client_conn = kcpmux_conn_connect(
        ctx.client_engine, ctx.server_addr.get(), nullptr, nullptr,
        &client_callbacks, &ctx.client_ctx);
    ctx.deliver_all();

    // Set server-side callbacks to accept incoming streams
    kcpmux_conn_t *server_conn = kcpmux_engine_get_conn_by_addr(
        ctx.server_engine, ctx.client_addr.get());
    kcpmux_conn_callbacks_t server_callbacks = create_conn_callbacks(&ctx.server_ctx);
    kcpmux_conn_set_callbacks(server_conn, &server_callbacks, &ctx.server_ctx);

    kcpmux_stream_callbacks_t stream_callbacks = create_stream_callbacks(&ctx.client_ctx);
    kcpmux_stream_t *stream = kcpmux_stream_create(
        client_conn, &stream_config, &stream_callbacks, &ctx.client_ctx);
    EXPECT_EQ(kcpmux_stream_get_state(stream), KCPMUX_STREAM_STATE_OPEN);

    // Send bootstrap payload so close uses protocol close path (not immediate local close).
    uint8_t bootstrap = 0x7f;
    ASSERT_EQ(kcpmux_stream_send(stream, &bootstrap, 1, 1), 1);
    ctx.deliver_all();

    // Close stream
    ctx.client_ctx.stream_close_count = 0;
    kcpmux_stream_close(stream);
    EXPECT_EQ(kcpmux_stream_get_state(stream), KCPMUX_STREAM_STATE_CLOSING);

    // Don't deliver the CLOSE message
    ctx.client_ctx.sent_packets.clear();

    // Advance time past first timeout
    ctx.advance_time(101);
    kcpmux_engine_update(ctx.client_engine);

    // First retry
    EXPECT_EQ(kcpmux_stream_get_state(stream), KCPMUX_STREAM_STATE_CLOSING);
    ctx.client_ctx.sent_packets.clear();

    // Advance time past second timeout (retries exhausted)
    ctx.advance_time(101);
    kcpmux_engine_update(ctx.client_engine);

    // Stream should be CLOSED
    EXPECT_EQ(kcpmux_stream_get_state(stream), KCPMUX_STREAM_STATE_CLOSED);
    EXPECT_EQ(ctx.client_ctx.stream_close_count, 1);
    EXPECT_EQ(ctx.client_ctx.stream_close_reason, KCPMUX_CLOSE_REASON_NORMAL);

    ctx.teardown();
}

// ============================================================================
// Stream Normal Flow Tests (Dual Engine)
// ============================================================================

TEST(kcpmux_lifecycle, stream_create_success) {
    DualEngineContext ctx;
    ctx.setup();

    // Establish connection
    kcpmux_conn_callbacks_t client_callbacks = create_conn_callbacks(&ctx.client_ctx);
    kcpmux_conn_t *client_conn = kcpmux_conn_connect(
        ctx.client_engine, ctx.server_addr.get(), nullptr, nullptr,
        &client_callbacks, &ctx.client_ctx);
    ctx.deliver_all();

    kcpmux_conn_t *server_conn = kcpmux_engine_get_conn_by_addr(
        ctx.server_engine, ctx.client_addr.get());
    kcpmux_conn_callbacks_t server_callbacks = create_conn_callbacks(&ctx.server_ctx);
    kcpmux_conn_set_callbacks(server_conn, &server_callbacks, &ctx.server_ctx);

    // Client creates stream
    kcpmux_stream_callbacks_t stream_callbacks = create_stream_callbacks(&ctx.client_ctx);
    kcpmux_stream_t *client_stream = kcpmux_stream_create(
        client_conn, nullptr, &stream_callbacks, &ctx.client_ctx);
    ASSERT_NE(client_stream, nullptr);
    EXPECT_EQ(kcpmux_stream_get_state(client_stream), KCPMUX_STREAM_STATE_OPEN);

    // First payload will trigger stream auto-create on server.
    const char data[] = "hello";
    EXPECT_GT(kcpmux_stream_send(client_stream, (const uint8_t *)data, sizeof(data), 1), 0);
    ctx.deliver_client_to_server();
    kcpmux_engine_update(ctx.server_engine);

    // Server should have stream
    uint32_t stream_id = kcpmux_stream_id(client_stream);
    kcpmux_stream_t *server_stream = kcpmux_conn_get_stream_by_id(server_conn, stream_id);
    ASSERT_NE(server_stream, nullptr);
    EXPECT_EQ(kcpmux_stream_get_state(server_stream), KCPMUX_STREAM_STATE_OPEN);

    ctx.teardown();
}

TEST(kcpmux_lifecycle, stream_close_initiator) {
    DualEngineContext ctx;
    ctx.setup();

    // Establish connection and stream
    kcpmux_conn_callbacks_t client_callbacks = create_conn_callbacks(&ctx.client_ctx);
    kcpmux_conn_t *client_conn = kcpmux_conn_connect(
        ctx.client_engine, ctx.server_addr.get(), nullptr, nullptr,
        &client_callbacks, &ctx.client_ctx);
    ctx.deliver_all();

    kcpmux_conn_t *server_conn = kcpmux_engine_get_conn_by_addr(
        ctx.server_engine, ctx.client_addr.get());
    kcpmux_conn_callbacks_t server_callbacks = create_conn_callbacks(&ctx.server_ctx);
    kcpmux_conn_set_callbacks(server_conn, &server_callbacks, &ctx.server_ctx);

    kcpmux_stream_callbacks_t client_stream_callbacks = create_stream_callbacks(&ctx.client_ctx);
    kcpmux_stream_t *client_stream = kcpmux_stream_create(
        client_conn, nullptr, &client_stream_callbacks, &ctx.client_ctx);

    // Send bootstrap payload to trigger server-side auto-create.
    uint8_t bootstrap = 0x7f;
    int ret = kcpmux_stream_send(client_stream, &bootstrap, 1, 1);
    ASSERT_EQ(ret, 1);

    ctx.deliver_all();

    uint32_t stream_id = kcpmux_stream_id(client_stream);
    kcpmux_stream_t *server_stream = kcpmux_conn_get_stream_by_id(server_conn, stream_id);
    kcpmux_stream_callbacks_t server_stream_callbacks = create_stream_callbacks(&ctx.server_ctx);
    kcpmux_stream_set_callbacks(server_stream, &server_stream_callbacks, &ctx.server_ctx);

    // Client closes stream
    kcpmux_stream_close(client_stream);
    EXPECT_EQ(kcpmux_stream_get_state(client_stream), KCPMUX_STREAM_STATE_CLOSING);

    // Deliver STREAM_CLOSE to server
    ctx.deliver_client_to_server();
    EXPECT_EQ(kcpmux_stream_get_state(server_stream), KCPMUX_STREAM_STATE_CLOSED);

    // Deliver STREAM_CLOSE_ACK to client
    ctx.deliver_server_to_client();
    EXPECT_EQ(kcpmux_stream_get_state(client_stream), KCPMUX_STREAM_STATE_CLOSED);

    ctx.teardown();
}

TEST(kcpmux_lifecycle, stream_state_callbacks) {
    DualEngineContext ctx;
    ctx.setup();

    // Establish connection
    kcpmux_conn_callbacks_t client_callbacks = create_conn_callbacks(&ctx.client_ctx);
    kcpmux_conn_t *client_conn = kcpmux_conn_connect(
        ctx.client_engine, ctx.server_addr.get(), nullptr, nullptr,
        &client_callbacks, &ctx.client_ctx);
    ctx.deliver_all();

    // Set server-side callbacks to accept incoming streams
    kcpmux_conn_t *server_conn = kcpmux_engine_get_conn_by_addr(
        ctx.server_engine, ctx.client_addr.get());
    kcpmux_conn_callbacks_t server_callbacks = create_conn_callbacks(&ctx.server_ctx);
    kcpmux_conn_set_callbacks(server_conn, &server_callbacks, &ctx.server_ctx);

    // Create stream with callbacks
    ctx.client_ctx.stream_state_changes.clear();
    kcpmux_stream_callbacks_t stream_callbacks = create_stream_callbacks(&ctx.client_ctx);
    kcpmux_stream_t *client_stream = kcpmux_stream_create(
        client_conn, nullptr, &stream_callbacks, &ctx.client_ctx);
    ASSERT_NE(client_stream, nullptr);

    // Send bootstrap payload so close uses protocol close path.
    uint8_t bootstrap = 0x7f;
    ASSERT_EQ(kcpmux_stream_send(client_stream, &bootstrap, 1, 1), 1);
    ctx.deliver_all();

    // Close state change: OPEN -> CLOSING
    size_t before = ctx.client_ctx.stream_state_changes.size();
    kcpmux_stream_close(client_stream);
    ASSERT_EQ(ctx.client_ctx.stream_state_changes.size(), before + 1);
    auto change = ctx.client_ctx.stream_state_changes.back();
    EXPECT_EQ(change.first, KCPMUX_STREAM_STATE_OPEN);
    EXPECT_EQ(change.second, KCPMUX_STREAM_STATE_CLOSING);

    ctx.teardown();
}

TEST(kcpmux_lifecycle, stream_close_initiator_without_payload_immediate_close) {
    DualEngineContext ctx;
    ctx.setup();

    kcpmux_conn_callbacks_t client_callbacks = create_conn_callbacks(&ctx.client_ctx);
    kcpmux_conn_t *client_conn = kcpmux_conn_connect(
        ctx.client_engine, ctx.server_addr.get(), nullptr, nullptr,
        &client_callbacks, &ctx.client_ctx);
    ctx.deliver_all();

    kcpmux_stream_callbacks_t stream_callbacks = create_stream_callbacks(&ctx.client_ctx);
    kcpmux_stream_t *stream = kcpmux_stream_create(
        client_conn, nullptr, &stream_callbacks, &ctx.client_ctx);
    ASSERT_NE(stream, nullptr);
    EXPECT_EQ(stream->is_initiator, 1);
    EXPECT_EQ(stream->stats.up_sent_bytes, 0u);

    ctx.client_ctx.stream_close_count = 0;
    ctx.client_ctx.sent_packets.clear();
    kcpmux_stream_close(stream);

    EXPECT_EQ(kcpmux_stream_get_state(stream), KCPMUX_STREAM_STATE_CLOSED);
    EXPECT_EQ(ctx.client_ctx.stream_close_count, 1);
    EXPECT_EQ(ctx.client_ctx.sent_packets.size(), 0u);

    ctx.teardown();
}

TEST(kcpmux_lifecycle, stream_close_acceptor_without_payload_keeps_protocol_close) {
    DualEngineContext ctx;
    ctx.setup();

    kcpmux_conn_callbacks_t client_callbacks = create_conn_callbacks(&ctx.client_ctx);
    kcpmux_conn_t *client_conn = kcpmux_conn_connect(
        ctx.client_engine, ctx.server_addr.get(), nullptr, nullptr,
        &client_callbacks, &ctx.client_ctx);
    ctx.deliver_all();

    kcpmux_conn_t *server_conn = kcpmux_engine_get_conn_by_addr(
        ctx.server_engine, ctx.client_addr.get());
    kcpmux_conn_callbacks_t server_callbacks = create_conn_callbacks(&ctx.server_ctx);
    kcpmux_conn_set_callbacks(server_conn, &server_callbacks, &ctx.server_ctx);

    kcpmux_stream_callbacks_t client_stream_callbacks = create_stream_callbacks(&ctx.client_ctx);
    kcpmux_stream_t *client_stream = kcpmux_stream_create(
        client_conn, nullptr, &client_stream_callbacks, &ctx.client_ctx);
    ASSERT_NE(client_stream, nullptr);

    // Bootstrap payload triggers server-side auto-create. Server stream has up_sent_bytes == 0.
    uint8_t bootstrap = 0x7f;
    ASSERT_EQ(kcpmux_stream_send(client_stream, &bootstrap, 1, 1), 1);
    ctx.deliver_all();

    uint32_t stream_id = kcpmux_stream_id(client_stream);
    kcpmux_stream_t *server_stream = kcpmux_conn_get_stream_by_id(server_conn, stream_id);
    ASSERT_NE(server_stream, nullptr);
    EXPECT_EQ(server_stream->is_initiator, 0);
    EXPECT_EQ(server_stream->stats.up_sent_bytes, 0u);

    ctx.server_ctx.sent_packets.clear();
    kcpmux_stream_close(server_stream);

    EXPECT_EQ(kcpmux_stream_get_state(server_stream), KCPMUX_STREAM_STATE_CLOSING);
    ASSERT_EQ(ctx.server_ctx.sent_packets.size(), 1u);
    EXPECT_EQ(ctx.server_ctx.sent_packets[0][0], KCPMUX_MSG_STREAM_CLOSE);

    ctx.teardown();
}

// ============================================================================
// Stream Error Handling Tests
// ============================================================================

TEST(kcpmux_lifecycle, stream_create_on_non_connected) {
    TestContext ctx;
    kcpmux_engine_t *engine = create_test_engine(&ctx);
    ASSERT_NE(engine, nullptr);

    TestAddr addr(0x7f000001, 12345);
    kcpmux_conn_t *conn = kcpmux_conn_connect(engine, addr.get(), nullptr, nullptr, nullptr, nullptr);
    ASSERT_NE(conn, nullptr);

    // Connection is in CONNECTING state, not CONNECTED
    EXPECT_EQ(kcpmux_conn_get_state(conn), KCPMUX_CONN_STATE_CONNECTING);

    // Try to create stream
    kcpmux_stream_t *stream = kcpmux_stream_create(conn, nullptr, nullptr, nullptr);
    EXPECT_EQ(stream, nullptr);

    kcpmux_engine_destroy(engine);
}

// ============================================================================
// Cascade Close Tests
// ============================================================================

TEST(kcpmux_lifecycle, conn_close_cascades_streams) {
    DualEngineContext ctx;
    ctx.setup();

    // Establish connection and streams
    kcpmux_conn_callbacks_t client_callbacks = create_conn_callbacks(&ctx.client_ctx);
    kcpmux_conn_t *client_conn = kcpmux_conn_connect(
        ctx.client_engine, ctx.server_addr.get(), nullptr, nullptr,
        &client_callbacks, &ctx.client_ctx);
    ctx.deliver_all();

    // Set server-side callbacks to accept incoming streams
    kcpmux_conn_t *server_conn = kcpmux_engine_get_conn_by_addr(
        ctx.server_engine, ctx.client_addr.get());
    kcpmux_conn_callbacks_t server_callbacks = create_conn_callbacks(&ctx.server_ctx);
    kcpmux_conn_set_callbacks(server_conn, &server_callbacks, &ctx.server_ctx);

    kcpmux_stream_callbacks_t stream_callbacks = create_stream_callbacks(&ctx.client_ctx);

    // Create multiple streams
    kcpmux_stream_t *stream1 = kcpmux_stream_create(
        client_conn, nullptr, &stream_callbacks, &ctx.client_ctx);
    ctx.deliver_all();

    kcpmux_stream_t *stream2 = kcpmux_stream_create(
        client_conn, nullptr, &stream_callbacks, &ctx.client_ctx);
    ctx.deliver_all();

    EXPECT_EQ(kcpmux_stream_get_state(stream1), KCPMUX_STREAM_STATE_OPEN);
    EXPECT_EQ(kcpmux_stream_get_state(stream2), KCPMUX_STREAM_STATE_OPEN);

    // Close connection
    kcpmux_conn_close(client_conn);
    ctx.deliver_all();

    // All streams should be closed
    uint8_t s1_state = kcpmux_stream_get_state(stream1);
    uint8_t s2_state = kcpmux_stream_get_state(stream2);
    EXPECT_TRUE(s1_state == KCPMUX_STREAM_STATE_CLOSING ||
                s1_state == KCPMUX_STREAM_STATE_CLOSED);
    EXPECT_TRUE(s2_state == KCPMUX_STREAM_STATE_CLOSING ||
                s2_state == KCPMUX_STREAM_STATE_CLOSED);

    ctx.teardown();
}

TEST(kcpmux_lifecycle, engine_destroy_cascades_all) {
    TestContext ctx;
    kcpmux_engine_t *engine = create_test_engine(&ctx);
    ASSERT_NE(engine, nullptr);

    TestAddr addr1(0x7f000001, 12345);
    TestAddr addr2(0x7f000001, 12346);

    // Create connections
    kcpmux_conn_connect(engine, addr1.get(), nullptr, nullptr, nullptr, nullptr);
    kcpmux_conn_connect(engine, addr2.get(), nullptr, nullptr, nullptr, nullptr);

    // Destroy engine should not crash and cleanup all
    kcpmux_engine_destroy(engine);

    // If we reach here without crash, test passes
    SUCCEED();
}

}  // namespace
