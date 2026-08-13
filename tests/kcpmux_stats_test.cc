#include "kcpmux_test_utils.h"

using namespace kcpmux_test;

namespace {

// Helper: pump KCP to exchange packets between engines
static inline void pump_kcp(DualEngineContext &ctx)
{
    kcpmux_engine_update(ctx.client_engine);
    kcpmux_engine_update(ctx.server_engine);
    ctx.deliver_all();
    kcpmux_engine_update(ctx.client_engine);
    kcpmux_engine_update(ctx.server_engine);
}

}  // namespace

// ============================================================================
// Engine Stats Tests
// ============================================================================

TEST(kcpmux_stats_engine, initial_zero) {
    TestContext ctx;
    kcpmux_engine_t *engine = create_test_engine(&ctx);
    ASSERT_NE(engine, nullptr);

    kcpmux_engine_stats_t stats;
    kcpmux_engine_get_stats(engine, &stats);

    // All packet stats should be zero
    EXPECT_EQ(stats.tx_packets, 0u);
    EXPECT_EQ(stats.tx_bytes, 0u);
    EXPECT_EQ(stats.rx_packets, 0u);
    EXPECT_EQ(stats.rx_bytes, 0u);
    EXPECT_EQ(stats.tx_error_packets, 0u);

    // All connection/stream counts should be zero
    EXPECT_EQ(stats.conn_count, 0u);
    EXPECT_EQ(stats.stream_count, 0u);

    // All lifecycle counters should be zero
    EXPECT_EQ(stats.conn_created_total, 0u);
    EXPECT_EQ(stats.conn_closed_total, 0u);
    EXPECT_EQ(stats.conn_connected_total, 0u);
    EXPECT_EQ(stats.conn_rejected_total, 0u);
    EXPECT_EQ(stats.conn_idle_timeout_total, 0u);
    EXPECT_EQ(stats.conn_keepalive_timeout_total, 0u);

    EXPECT_EQ(stats.stream_created_total, 0u);
    EXPECT_EQ(stats.stream_closed_total, 0u);
    EXPECT_EQ(stats.stream_opened_total, 0u);

    // All api counters should be zero
    EXPECT_EQ(stats.api_conn_connect_calls, 0u);
    EXPECT_EQ(stats.api_conn_close_calls, 0u);
    EXPECT_EQ(stats.api_stream_create_calls, 0u);
    EXPECT_EQ(stats.api_stream_close_calls, 0u);
    EXPECT_EQ(stats.api_stream_send_calls, 0u);
    EXPECT_EQ(stats.api_stream_recv_calls, 0u);

    kcpmux_engine_destroy(engine);
}

TEST(kcpmux_stats_engine, tx_rx_packets) {
    DualEngineContext ctx;
    ctx.setup();

    // Client connects to server
    kcpmux_conn_callbacks_t client_callbacks = create_conn_callbacks(&ctx.client_ctx);
    kcpmux_conn_t *client_conn = kcpmux_conn_connect(
        ctx.client_engine,
        ctx.server_addr.get(),
        nullptr,
        nullptr,
        &client_callbacks,
        &ctx.client_ctx);
    ASSERT_NE(client_conn, nullptr);

    // Get client stats after connect (should have sent CONN_CONNECT)
    kcpmux_engine_stats_t client_stats;
    kcpmux_engine_get_stats(ctx.client_engine, &client_stats);
    EXPECT_GT(client_stats.tx_packets, 0u);
    EXPECT_GT(client_stats.tx_bytes, 0u);
    EXPECT_EQ(client_stats.rx_packets, 0u);

    // Deliver to server
    ctx.deliver_client_to_server();

    // Get server stats (should have received packet)
    kcpmux_engine_stats_t server_stats;
    kcpmux_engine_get_stats(ctx.server_engine, &server_stats);
    EXPECT_GT(server_stats.rx_packets, 0u);
    EXPECT_GT(server_stats.rx_bytes, 0u);
    EXPECT_GT(server_stats.tx_packets, 0u);  // Server sends ACK

    // Deliver ACK to client
    ctx.deliver_server_to_client();

    // Client should have received packet
    kcpmux_engine_get_stats(ctx.client_engine, &client_stats);
    EXPECT_GT(client_stats.rx_packets, 0u);
    EXPECT_GT(client_stats.rx_bytes, 0u);

    ctx.teardown();
}

TEST(kcpmux_stats_engine, conn_lifecycle) {
    DualEngineContext ctx;
    ctx.setup();

    kcpmux_engine_stats_t stats_before, stats_after;

    // Get initial stats
    kcpmux_engine_get_stats(ctx.client_engine, &stats_before);
    EXPECT_EQ(stats_before.conn_created_total, 0u);
    EXPECT_EQ(stats_before.conn_connected_total, 0u);
    EXPECT_EQ(stats_before.conn_closed_total, 0u);

    // Client connects
    kcpmux_conn_callbacks_t client_callbacks = create_conn_callbacks(&ctx.client_ctx);
    kcpmux_conn_t *client_conn = kcpmux_conn_connect(
        ctx.client_engine,
        ctx.server_addr.get(),
        nullptr,
        nullptr,
        &client_callbacks,
        &ctx.client_ctx);
    ASSERT_NE(client_conn, nullptr);

    // Check conn_created incremented
    kcpmux_engine_get_stats(ctx.client_engine, &stats_after);
    EXPECT_EQ(stats_after.conn_created_total, 1u);
    EXPECT_EQ(stats_after.conn_count, 1u);
    EXPECT_EQ(stats_after.conn_connected_total, 0u);  // Not yet connected

    // Complete handshake
    ctx.deliver_all();

    // Check conn_connected incremented
    kcpmux_engine_get_stats(ctx.client_engine, &stats_after);
    EXPECT_EQ(stats_after.conn_connected_total, 1u);

    // Check server stats
    kcpmux_engine_stats_t server_stats;
    kcpmux_engine_get_stats(ctx.server_engine, &server_stats);
    EXPECT_EQ(server_stats.conn_created_total, 1u);
    EXPECT_EQ(server_stats.conn_connected_total, 1u);
    EXPECT_EQ(server_stats.conn_count, 1u);

    // Close connection
    kcpmux_conn_close(client_conn);
    ctx.deliver_all();

    // Check conn_closed incremented
    kcpmux_engine_get_stats(ctx.client_engine, &stats_after);
    EXPECT_EQ(stats_after.conn_closed_total, 1u);
    EXPECT_EQ(stats_after.conn_count, 0u);

    ctx.teardown();
}

TEST(kcpmux_stats_engine, conn_rejected) {
    DualEngineContext ctx;
    ctx.setup();

    // Server will reject connections
    ctx.server_ctx.conn_notify_result = KCPMUX_ACK_RESULT_ERROR;

    kcpmux_engine_stats_t stats_before, stats_after;
    kcpmux_engine_get_stats(ctx.client_engine, &stats_before);
    EXPECT_EQ(stats_before.conn_rejected_total, 0u);

    // Client connects
    kcpmux_conn_callbacks_t client_callbacks = create_conn_callbacks(&ctx.client_ctx);
    kcpmux_conn_t *client_conn = kcpmux_conn_connect(
        ctx.client_engine,
        ctx.server_addr.get(),
        nullptr,
        nullptr,
        &client_callbacks,
        &ctx.client_ctx);
    ASSERT_NE(client_conn, nullptr);

    // Deliver and get rejected
    ctx.deliver_all();

    // Check conn_rejected incremented on client
    kcpmux_engine_get_stats(ctx.client_engine, &stats_after);
    EXPECT_EQ(stats_after.conn_rejected_total, 1u);

    // Check conn_rejected incremented on server
    kcpmux_engine_stats_t server_stats;
    kcpmux_engine_get_stats(ctx.server_engine, &server_stats);
    EXPECT_EQ(server_stats.conn_rejected_total, 1u);

    ctx.teardown();
}

TEST(kcpmux_stats_engine, stream_lifecycle) {
    DualEngineContext ctx;
    ctx.setup();

    // Establish connection first
    kcpmux_conn_callbacks_t client_callbacks = create_conn_callbacks(&ctx.client_ctx);
    kcpmux_conn_t *client_conn = kcpmux_conn_connect(
        ctx.client_engine,
        ctx.server_addr.get(),
        nullptr,
        nullptr,
        &client_callbacks,
        &ctx.client_ctx);
    ctx.deliver_all();

    kcpmux_conn_t *server_conn = kcpmux_engine_get_conn_by_addr(
        ctx.server_engine,
        ctx.client_addr.get());
    kcpmux_conn_callbacks_t server_callbacks = create_conn_callbacks(&ctx.server_ctx);
    kcpmux_conn_set_callbacks(server_conn, &server_callbacks, &ctx.server_ctx);

    kcpmux_engine_stats_t stats;

    // Check initial stream stats
    kcpmux_engine_get_stats(ctx.client_engine, &stats);
    EXPECT_EQ(stats.stream_created_total, 0u);
    EXPECT_EQ(stats.stream_opened_total, 0u);
    EXPECT_EQ(stats.stream_closed_total, 0u);
    EXPECT_EQ(stats.stream_count, 0u);

    // Create stream
    kcpmux_stream_callbacks_t stream_callbacks = create_stream_callbacks(&ctx.client_ctx);
    kcpmux_stream_t *client_stream = kcpmux_stream_create(
        client_conn,
        nullptr,
        &stream_callbacks,
        &ctx.client_ctx);
    ASSERT_NE(client_stream, nullptr);

    // Send bootstrap payload to trigger server-side auto-create.
    uint8_t bootstrap = 0x7f;
    int ret = kcpmux_stream_send(client_stream, &bootstrap, 1, 1);
    ASSERT_EQ(ret, 1);

    // Check stream_created incremented
    kcpmux_engine_get_stats(ctx.client_engine, &stats);
    EXPECT_EQ(stats.stream_created_total, 1u);
    EXPECT_EQ(stats.stream_count, 1u);
    EXPECT_EQ(stats.stream_opened_total, 1u);

    // Close stream
    kcpmux_stream_close(client_stream);
    ctx.deliver_all();

    // Check stream_closed incremented
    kcpmux_engine_get_stats(ctx.client_engine, &stats);
    EXPECT_EQ(stats.stream_closed_total, 1u);
    EXPECT_EQ(stats.stream_count, 0u);

    ctx.teardown();
}

TEST(kcpmux_stats_engine, conn_keepalive_timeout) {
    DualEngineContext ctx;
    ctx.client_ctx.current_time_ms = 1000;
    ctx.server_ctx.current_time_ms = 1000;
    ctx.setup();

    // Create config with short keepalive timeout
    kcpmux_conn_config_t conn_config;
    kcpmux_conn_config_init(&conn_config);
    conn_config.keepalive_timeout_ms = 500;
    conn_config.keepalive_interval_ms = 100;

    // Connect
    kcpmux_conn_callbacks_t client_callbacks = create_conn_callbacks(&ctx.client_ctx);
    kcpmux_conn_t *client_conn = kcpmux_conn_connect(
        ctx.client_engine,
        ctx.server_addr.get(),
        &conn_config,
        nullptr,
        &client_callbacks,
        &ctx.client_ctx);
    ctx.deliver_all();
    EXPECT_EQ(kcpmux_conn_get_state(client_conn), KCPMUX_CONN_STATE_CONNECTED);

    kcpmux_engine_stats_t stats;
    kcpmux_engine_get_stats(ctx.client_engine, &stats);
    EXPECT_EQ(stats.conn_keepalive_timeout_total, 0u);

    // Advance time past keepalive timeout without any data exchange
    ctx.client_ctx.current_time_ms += 501;
    ctx.server_ctx.current_time_ms += 501;
    kcpmux_engine_update(ctx.client_engine);

    // Check keepalive_timeout incremented
    kcpmux_engine_get_stats(ctx.client_engine, &stats);
    EXPECT_EQ(stats.conn_keepalive_timeout_total, 1u);

    ctx.teardown();
}

TEST(kcpmux_stats_engine, conn_idle_timeout) {
    DualEngineContext ctx;
    ctx.client_ctx.current_time_ms = 1000;
    ctx.server_ctx.current_time_ms = 1000;
    ctx.setup();

    // Create config with short idle timeout
    kcpmux_conn_config_t conn_config;
    kcpmux_conn_config_init(&conn_config);
    conn_config.idle_timeout_ms = 500;
    conn_config.keepalive_timeout_ms = 10000;  // Long keepalive (won't trigger)
    conn_config.keepalive_interval_ms = 100;

    // Connect
    kcpmux_conn_callbacks_t client_callbacks = create_conn_callbacks(&ctx.client_ctx);
    kcpmux_conn_t *client_conn = kcpmux_conn_connect(
        ctx.client_engine,
        ctx.server_addr.get(),
        &conn_config,
        nullptr,
        &client_callbacks,
        &ctx.client_ctx);
    ctx.deliver_all();
    EXPECT_EQ(kcpmux_conn_get_state(client_conn), KCPMUX_CONN_STATE_CONNECTED);

    kcpmux_engine_stats_t stats;
    kcpmux_engine_get_stats(ctx.client_engine, &stats);
    EXPECT_EQ(stats.conn_idle_timeout_total, 0u);

    // Simulate keepalive exchange but no payload data
    for (int i = 0; i < 4; i++) {
        ctx.advance_time(100);
        kcpmux_engine_update(ctx.client_engine);
        kcpmux_engine_update(ctx.server_engine);
        ctx.deliver_all();
    }

    // Advance past idle timeout
    ctx.advance_time(200);
    kcpmux_engine_update(ctx.client_engine);

    // Check idle_timeout incremented
    kcpmux_engine_get_stats(ctx.client_engine, &stats);
    EXPECT_EQ(stats.conn_idle_timeout_total, 1u);

    ctx.teardown();
}

TEST(kcpmux_stats_engine, multiple_conns) {
    TestContext ctx;
    kcpmux_engine_t *engine = create_test_engine(&ctx);
    ASSERT_NE(engine, nullptr);

    TestAddr addr1(0x7f000001, 12345);
    TestAddr addr2(0x7f000001, 12346);
    TestAddr addr3(0x7f000001, 12347);

    kcpmux_engine_stats_t stats;

    // Create first connection
    kcpmux_conn_t
        *conn1 = kcpmux_conn_connect(engine, addr1.get(), nullptr, nullptr, nullptr, nullptr);
    ASSERT_NE(conn1, nullptr);

    kcpmux_engine_get_stats(engine, &stats);
    EXPECT_EQ(stats.conn_created_total, 1u);
    EXPECT_EQ(stats.conn_count, 1u);

    // Create second connection
    kcpmux_conn_t
        *conn2 = kcpmux_conn_connect(engine, addr2.get(), nullptr, nullptr, nullptr, nullptr);
    ASSERT_NE(conn2, nullptr);

    kcpmux_engine_get_stats(engine, &stats);
    EXPECT_EQ(stats.conn_created_total, 2u);
    EXPECT_EQ(stats.conn_count, 2u);

    // Create third connection
    kcpmux_conn_t
        *conn3 = kcpmux_conn_connect(engine, addr3.get(), nullptr, nullptr, nullptr, nullptr);
    ASSERT_NE(conn3, nullptr);

    kcpmux_engine_get_stats(engine, &stats);
    EXPECT_EQ(stats.conn_created_total, 3u);
    EXPECT_EQ(stats.conn_count, 3u);

    // TX stats should accumulate (each connect sends a packet)
    EXPECT_GE(stats.tx_packets, 3u);

    kcpmux_engine_destroy(engine);
}

TEST(kcpmux_stats_engine, api_calls) {
    DualEngineContext ctx;
    ctx.setup();

    kcpmux_engine_stats_t stats;

    // Initial: all API call counters should be zero
    kcpmux_engine_get_stats(ctx.client_engine, &stats);
    EXPECT_EQ(stats.api_conn_connect_calls, 0u);
    EXPECT_EQ(stats.api_conn_close_calls, 0u);
    EXPECT_EQ(stats.api_stream_create_calls, 0u);
    EXPECT_EQ(stats.api_stream_close_calls, 0u);
    EXPECT_EQ(stats.api_stream_send_calls, 0u);
    EXPECT_EQ(stats.api_stream_recv_calls, 0u);

    // Test api_conn_connect_calls
    kcpmux_conn_callbacks_t client_callbacks = create_conn_callbacks(&ctx.client_ctx);
    kcpmux_conn_t *client_conn = kcpmux_conn_connect(
        ctx.client_engine,
        ctx.server_addr.get(),
        nullptr,
        nullptr,
        &client_callbacks,
        &ctx.client_ctx);
    ASSERT_NE(client_conn, nullptr);

    kcpmux_engine_get_stats(ctx.client_engine, &stats);
    EXPECT_EQ(stats.api_conn_connect_calls, 1u);

    // Complete handshake
    ctx.deliver_all();
    EXPECT_EQ(kcpmux_conn_get_state(client_conn), KCPMUX_CONN_STATE_CONNECTED);

    // Setup server connection callbacks
    kcpmux_conn_t *server_conn = kcpmux_engine_get_conn_by_addr(
        ctx.server_engine,
        ctx.client_addr.get());
    ASSERT_NE(server_conn, nullptr);
    kcpmux_conn_callbacks_t server_callbacks = create_conn_callbacks(&ctx.server_ctx);
    kcpmux_conn_set_callbacks(server_conn, &server_callbacks, &ctx.server_ctx);

    // Test api_stream_create_calls
    kcpmux_stream_callbacks_t stream_callbacks = create_stream_callbacks(&ctx.client_ctx);
    kcpmux_stream_t *client_stream = kcpmux_stream_create(
        client_conn,
        nullptr,
        &stream_callbacks,
        &ctx.client_ctx);
    ASSERT_NE(client_stream, nullptr);

    kcpmux_engine_get_stats(ctx.client_engine, &stats);
    EXPECT_EQ(stats.api_stream_create_calls, 1u);

    // Send bootstrap payload to trigger server-side auto-create.
    uint8_t bootstrap = 0x7f;
    int ret = kcpmux_stream_send(client_stream, &bootstrap, 1, 1);
    ASSERT_EQ(ret, 1);

    // Complete stream handshake
    pump_kcp(ctx);
    uint32_t stream_id = kcpmux_stream_id(client_stream);
    kcpmux_stream_t *server_stream = kcpmux_conn_get_stream_by_id(server_conn, stream_id);
    ASSERT_NE(server_stream, nullptr);
    kcpmux_stream_callbacks_t server_stream_callbacks = create_stream_callbacks(&ctx.server_ctx);
    kcpmux_stream_set_callbacks(server_stream, &server_stream_callbacks, &ctx.server_ctx);
    pump_kcp(ctx);

    // Test api_stream_send_calls
    std::string data = "Hello";
    ret = kcpmux_stream_send(client_stream, (const uint8_t*)data.c_str(), data.size(), 1);
    EXPECT_GT(ret, 0);

    kcpmux_engine_get_stats(ctx.client_engine, &stats);
    EXPECT_EQ(stats.api_stream_send_calls, 2u);

    // Send again to verify accumulation
    ret = kcpmux_stream_send(client_stream, (const uint8_t*)data.c_str(), data.size(), 1);
    EXPECT_GT(ret, 0);

    kcpmux_engine_get_stats(ctx.client_engine, &stats);
    EXPECT_EQ(stats.api_stream_send_calls, 3u);

    pump_kcp(ctx);

    // Test api_stream_recv_calls on server
    kcpmux_engine_get_stats(ctx.server_engine, &stats);
    EXPECT_EQ(stats.api_stream_recv_calls, 0u);

    uint8_t recv_buf[256];
    ret = kcpmux_stream_recv(server_stream, recv_buf, sizeof(recv_buf));
    EXPECT_GT(ret, 0);

    kcpmux_engine_get_stats(ctx.server_engine, &stats);
    EXPECT_EQ(stats.api_stream_recv_calls, 1u);

    // Recv again (even if no data, counter should increment)
    ret = kcpmux_stream_recv(server_stream, recv_buf, sizeof(recv_buf));

    kcpmux_engine_get_stats(ctx.server_engine, &stats);
    EXPECT_EQ(stats.api_stream_recv_calls, 2u);

    // Test api_stream_close_calls
    kcpmux_stream_close(client_stream);

    kcpmux_engine_get_stats(ctx.client_engine, &stats);
    EXPECT_EQ(stats.api_stream_close_calls, 1u);

    pump_kcp(ctx);

    // Test api_conn_close_calls
    kcpmux_conn_close(client_conn);

    kcpmux_engine_get_stats(ctx.client_engine, &stats);
    EXPECT_EQ(stats.api_conn_close_calls, 1u);

    // Final verification: all counters should have expected values
    kcpmux_engine_get_stats(ctx.client_engine, &stats);
    EXPECT_EQ(stats.api_conn_connect_calls, 1u);
    EXPECT_EQ(stats.api_conn_close_calls, 1u);
    EXPECT_EQ(stats.api_stream_create_calls, 1u);
    EXPECT_EQ(stats.api_stream_close_calls, 1u);
    EXPECT_EQ(stats.api_stream_send_calls, 3u);
    EXPECT_EQ(stats.api_stream_recv_calls, 0u);  // recv was on server

    kcpmux_engine_get_stats(ctx.server_engine, &stats);
    EXPECT_EQ(stats.api_stream_recv_calls, 2u);

    ctx.teardown();
}

// ============================================================================
// Connection Stats Tests
// ============================================================================

TEST(kcpmux_stats_conn, initial_zero) {
    DualEngineContext ctx;
    ctx.setup();

    // Connect
    kcpmux_conn_callbacks_t client_callbacks = create_conn_callbacks(&ctx.client_ctx);
    kcpmux_conn_t *client_conn = kcpmux_conn_connect(
        ctx.client_engine,
        ctx.server_addr.get(),
        nullptr,
        nullptr,
        &client_callbacks,
        &ctx.client_ctx);
    ASSERT_NE(client_conn, nullptr);

    // Before any packet exchange, stats should reflect only the initial connect packet
    kcpmux_conn_stats_t stats;
    kcpmux_conn_get_stats(client_conn, &stats);

    // TX should have the CONN_CONNECT packet
    EXPECT_GT(stats.tx_packets, 0u);
    EXPECT_GT(stats.tx_bytes, 0u);
    // RX should be zero (no response yet)
    EXPECT_EQ(stats.rx_packets, 0u);
    EXPECT_EQ(stats.rx_bytes, 0u);
    // Handshake not complete yet
    EXPECT_EQ(stats.handshake_time_ms, 0u);

    ctx.teardown();
}

TEST(kcpmux_stats_conn, tx_rx_packets) {
    DualEngineContext ctx;
    ctx.setup();

    // Connect and complete handshake
    kcpmux_conn_callbacks_t client_callbacks = create_conn_callbacks(&ctx.client_ctx);
    kcpmux_conn_t *client_conn = kcpmux_conn_connect(
        ctx.client_engine,
        ctx.server_addr.get(),
        nullptr,
        nullptr,
        &client_callbacks,
        &ctx.client_ctx);
    ctx.deliver_all();

    kcpmux_conn_t *server_conn = kcpmux_engine_get_conn_by_addr(
        ctx.server_engine,
        ctx.client_addr.get());
    ASSERT_NE(server_conn, nullptr);

    kcpmux_conn_stats_t client_stats, server_stats;

    // Get client stats
    kcpmux_conn_get_stats(client_conn, &client_stats);
    EXPECT_GT(client_stats.tx_packets, 0u);
    EXPECT_GT(client_stats.tx_bytes, 0u);
    EXPECT_GT(client_stats.rx_packets, 0u);  // Received ACK
    EXPECT_GT(client_stats.rx_bytes, 0u);

    // Get server stats
    kcpmux_conn_get_stats(server_conn, &server_stats);
    // Server conn was created when processing CONN_CONNECT, so RX is 0
    EXPECT_EQ(server_stats.rx_packets, 0u);
    EXPECT_EQ(server_stats.rx_bytes, 0u);
    // But it sent ACK
    EXPECT_GT(server_stats.tx_packets, 0u);
    EXPECT_GT(server_stats.tx_bytes, 0u);

    ctx.teardown();
}

TEST(kcpmux_stats_conn, handshake_time) {
    DualEngineContext ctx;
    ctx.client_ctx.current_time_ms = 1000;
    ctx.server_ctx.current_time_ms = 1000;
    ctx.setup();

    // Client connects
    kcpmux_conn_callbacks_t client_callbacks = create_conn_callbacks(&ctx.client_ctx);
    kcpmux_conn_t *client_conn = kcpmux_conn_connect(
        ctx.client_engine,
        ctx.server_addr.get(),
        nullptr,
        nullptr,
        &client_callbacks,
        &ctx.client_ctx);

    // Advance time before handshake completes
    ctx.advance_time(50);  // 50ms delay

    // Complete handshake
    ctx.deliver_all();

    EXPECT_EQ(kcpmux_conn_get_state(client_conn), KCPMUX_CONN_STATE_CONNECTED);

    // Check handshake time
    kcpmux_conn_stats_t stats;
    kcpmux_conn_get_stats(client_conn, &stats);
    EXPECT_GE(stats.handshake_time_ms, 50u);

    ctx.teardown();
}

TEST(kcpmux_stats_conn, multiple_streams) {
    DualEngineContext ctx;
    ctx.setup();

    // Establish connection
    kcpmux_conn_callbacks_t client_callbacks = create_conn_callbacks(&ctx.client_ctx);
    kcpmux_conn_t *client_conn = kcpmux_conn_connect(
        ctx.client_engine,
        ctx.server_addr.get(),
        nullptr,
        nullptr,
        &client_callbacks,
        &ctx.client_ctx);
    ctx.deliver_all();

    kcpmux_conn_t *server_conn = kcpmux_engine_get_conn_by_addr(
        ctx.server_engine,
        ctx.client_addr.get());
    kcpmux_conn_callbacks_t server_callbacks = create_conn_callbacks(&ctx.server_ctx);
    kcpmux_conn_set_callbacks(server_conn, &server_callbacks, &ctx.server_ctx);

    kcpmux_conn_stats_t stats_before, stats_after;
    kcpmux_conn_get_stats(client_conn, &stats_before);

    // Create first stream
    kcpmux_stream_callbacks_t stream_callbacks = create_stream_callbacks(&ctx.client_ctx);
    auto *stream = kcpmux_stream_create(client_conn, nullptr, &stream_callbacks, &ctx.client_ctx);
    pump_kcp(ctx);

    // Send bootstrap payload to trigger server-side auto-create.
    uint8_t bootstrap = 0x7f;
    int ret = kcpmux_stream_send(stream, &bootstrap, 1, 1);
    ASSERT_EQ(ret, 1);

    kcpmux_conn_get_stats(client_conn, &stats_after);
    EXPECT_GT(stats_after.tx_packets, stats_before.tx_packets);

    // Create second stream
    stats_before = stats_after;
    kcpmux_stream_create(client_conn, nullptr, &stream_callbacks, &ctx.client_ctx);
    pump_kcp(ctx);

    kcpmux_conn_get_stats(client_conn, &stats_after);
    EXPECT_EQ(stats_after.tx_packets, stats_before.tx_packets);

    // Stats should accumulate from all stream operations
    EXPECT_GT(stats_after.tx_bytes, 0u);
    EXPECT_GT(stats_after.rx_bytes, 0u);

    ctx.teardown();
}

// ============================================================================
// Stream Stats Tests
// ============================================================================

TEST(kcpmux_stats_stream, initial_zero) {
    DualEngineContext ctx;
    ctx.setup();

    // Establish connection
    kcpmux_conn_callbacks_t client_callbacks = create_conn_callbacks(&ctx.client_ctx);
    kcpmux_conn_t *client_conn = kcpmux_conn_connect(
        ctx.client_engine,
        ctx.server_addr.get(),
        nullptr,
        nullptr,
        &client_callbacks,
        &ctx.client_ctx);
    ctx.deliver_all();

    // Create stream
    kcpmux_stream_callbacks_t stream_callbacks = create_stream_callbacks(&ctx.client_ctx);
    kcpmux_stream_t *client_stream = kcpmux_stream_create(
        client_conn,
        nullptr,
        &stream_callbacks,
        &ctx.client_ctx);
    ASSERT_NE(client_stream, nullptr);

    kcpmux_stream_stats_t stats;
    kcpmux_stream_get_stats(client_stream, &stats);

    // TX/RX should be zero initially (packet not sent yet)
    EXPECT_EQ(stats.tx_packets, 0u);
    EXPECT_EQ(stats.tx_bytes, 0u);
    EXPECT_EQ(stats.rx_packets, 0u);
    EXPECT_EQ(stats.rx_bytes, 0u);
    // Upper layer bytes should be zero
    EXPECT_EQ(stats.up_sent_bytes, 0u);
    EXPECT_EQ(stats.up_recv_bytes, 0u);
    // Block stats should be zero
    EXPECT_EQ(stats.write_block_count, 0u);
    EXPECT_EQ(stats.write_block_time_ms, 0u);
    EXPECT_EQ(stats.read_block_count, 1u);
    EXPECT_EQ(stats.read_block_time_ms, 0u);
    // TTFB should be zero
    EXPECT_EQ(stats.ttfb_time_ms, 0u);

    ctx.teardown();
}

TEST(kcpmux_stats_stream, tx_rx_packets) {
    DualEngineContext ctx;
    ctx.setup();

    // Establish connection
    kcpmux_conn_callbacks_t client_callbacks = create_conn_callbacks(&ctx.client_ctx);
    kcpmux_conn_t *client_conn = kcpmux_conn_connect(
        ctx.client_engine,
        ctx.server_addr.get(),
        nullptr,
        nullptr,
        &client_callbacks,
        &ctx.client_ctx);
    ctx.deliver_all();

    kcpmux_conn_t *server_conn = kcpmux_engine_get_conn_by_addr(
        ctx.server_engine,
        ctx.client_addr.get());
    kcpmux_conn_callbacks_t server_callbacks = create_conn_callbacks(&ctx.server_ctx);
    kcpmux_conn_set_callbacks(server_conn, &server_callbacks, &ctx.server_ctx);

    // Create stream
    kcpmux_stream_callbacks_t stream_callbacks = create_stream_callbacks(&ctx.client_ctx);
    kcpmux_stream_t *client_stream = kcpmux_stream_create(
        client_conn,
        nullptr,
        &stream_callbacks,
        &ctx.client_ctx);

    // Send bootstrap payload to trigger server-side auto-create.
    uint8_t bootstrap = 0x7f;
    int ret = kcpmux_stream_send(client_stream, &bootstrap, 1, 1);
    ASSERT_EQ(ret, 1);

    pump_kcp(ctx);

    uint32_t stream_id = kcpmux_stream_id(client_stream);
    kcpmux_stream_t *server_stream = kcpmux_conn_get_stream_by_id(server_conn, stream_id);
    ASSERT_NE(server_stream, nullptr);
    kcpmux_stream_callbacks_t server_stream_callbacks = create_stream_callbacks(&ctx.server_ctx);
    kcpmux_stream_set_callbacks(server_stream, &server_stream_callbacks, &ctx.server_ctx);
    pump_kcp(ctx);

    kcpmux_stream_stats_t client_stats, server_stats;

    // Get stats after handshake (should be 1 for bootstrap packet)
    kcpmux_stream_get_stats(client_stream, &client_stats);
    EXPECT_EQ(client_stats.tx_packets, 1u);
    EXPECT_EQ(client_stats.rx_packets, 1u);

    kcpmux_stream_get_stats(server_stream, &server_stats);
    EXPECT_EQ(server_stats.rx_packets, 1u);
    EXPECT_EQ(server_stats.tx_packets, 1u);

    // Send data and verify stats increase
    std::string data = "Hello";
    kcpmux_stream_send(client_stream, (const uint8_t*)data.c_str(), data.size(), 1);
    pump_kcp(ctx);

    kcpmux_stream_get_stats(client_stream, &client_stats);
    EXPECT_GT(client_stats.tx_packets, 0u);  // Should have sent data packets

    ctx.teardown();
}

TEST(kcpmux_stats_stream, upper_layer_bytes) {
    DualEngineContext ctx;
    ctx.setup();

    // Establish connection
    kcpmux_conn_callbacks_t client_callbacks = create_conn_callbacks(&ctx.client_ctx);
    kcpmux_conn_t *client_conn = kcpmux_conn_connect(
        ctx.client_engine,
        ctx.server_addr.get(),
        nullptr,
        nullptr,
        &client_callbacks,
        &ctx.client_ctx);
    ctx.deliver_all();

    kcpmux_conn_t *server_conn = kcpmux_engine_get_conn_by_addr(
        ctx.server_engine,
        ctx.client_addr.get());
    kcpmux_conn_callbacks_t server_callbacks = create_conn_callbacks(&ctx.server_ctx);
    kcpmux_conn_set_callbacks(server_conn, &server_callbacks, &ctx.server_ctx);

    // Create stream
    kcpmux_stream_callbacks_t stream_callbacks = create_stream_callbacks(&ctx.client_ctx);
    kcpmux_stream_t *client_stream = kcpmux_stream_create(
        client_conn,
        nullptr,
        &stream_callbacks,
        &ctx.client_ctx);

    // Send bootstrap payload to trigger server-side auto-create.
    uint8_t bootstrap = 0x7f;
    int ret = kcpmux_stream_send(client_stream, &bootstrap, 1, 1);
    ASSERT_EQ(ret, 1);

    pump_kcp(ctx);

    uint32_t stream_id = kcpmux_stream_id(client_stream);
    kcpmux_stream_t *server_stream = kcpmux_conn_get_stream_by_id(server_conn, stream_id);
    kcpmux_stream_callbacks_t server_stream_callbacks = create_stream_callbacks(&ctx.server_ctx);
    kcpmux_stream_set_callbacks(server_stream, &server_stream_callbacks, &ctx.server_ctx);
    pump_kcp(ctx);

    // Send data from client
    std::string send_data = "Hello, kcpmux stats!";
    ret = kcpmux_stream_send(
        client_stream,
        (const uint8_t *)send_data.c_str(),
        send_data.size(),
        1);
    ASSERT_GT(ret, 0);
    pump_kcp(ctx);

    // Check client up_sent_bytes
    kcpmux_stream_stats_t client_stats;
    kcpmux_stream_get_stats(client_stream, &client_stats);
    EXPECT_EQ(client_stats.up_sent_bytes, send_data.size() + 1);

    // Receive on server
    uint8_t recv_buf[256];
    ret = kcpmux_stream_recv(server_stream, recv_buf, sizeof(recv_buf));
    ASSERT_EQ(ret, 1);
    ret = kcpmux_stream_recv(server_stream, recv_buf, sizeof(recv_buf));
    ASSERT_EQ(ret, (int)send_data.size());

    // Check server up_recv_bytes
    kcpmux_stream_stats_t server_stats;
    kcpmux_stream_get_stats(server_stream, &server_stats);
    EXPECT_EQ(server_stats.up_recv_bytes, send_data.size() + 1);

    ctx.teardown();
}

TEST(kcpmux_stats_stream, ttfb) {
    DualEngineContext ctx;
    ctx.client_ctx.current_time_ms = 1000;
    ctx.server_ctx.current_time_ms = 1000;
    ctx.setup();

    // Establish connection
    kcpmux_conn_callbacks_t client_callbacks = create_conn_callbacks(&ctx.client_ctx);
    kcpmux_conn_t *client_conn = kcpmux_conn_connect(
        ctx.client_engine,
        ctx.server_addr.get(),
        nullptr,
        nullptr,
        &client_callbacks,
        &ctx.client_ctx);
    ctx.deliver_all();

    kcpmux_conn_t *server_conn = kcpmux_engine_get_conn_by_addr(
        ctx.server_engine,
        ctx.client_addr.get());
    kcpmux_conn_callbacks_t server_callbacks = create_conn_callbacks(&ctx.server_ctx);
    kcpmux_conn_set_callbacks(server_conn, &server_callbacks, &ctx.server_ctx);

    // Create stream
    kcpmux_stream_callbacks_t stream_callbacks = create_stream_callbacks(&ctx.client_ctx);
    kcpmux_stream_t *client_stream = kcpmux_stream_create(
        client_conn,
        nullptr,
        &stream_callbacks,
        &ctx.client_ctx);

    // Send bootstrap payload to trigger server-side auto-create.
    uint8_t bootstrap = 0x7f;
    int ret = kcpmux_stream_send(client_stream, &bootstrap, 1, 1);
    ASSERT_EQ(ret, 1);

    pump_kcp(ctx);

    uint32_t stream_id = kcpmux_stream_id(client_stream);
    kcpmux_stream_t *server_stream = kcpmux_conn_get_stream_by_id(server_conn, stream_id);
    kcpmux_stream_callbacks_t server_stream_callbacks = create_stream_callbacks(&ctx.server_ctx);
    kcpmux_stream_set_callbacks(server_stream, &server_stream_callbacks, &ctx.server_ctx);
    pump_kcp(ctx);

    // TTFB should be zero before receiving first byte
    kcpmux_stream_stats_t client_stats;
    kcpmux_stream_get_stats(client_stream, &client_stats);
    EXPECT_EQ(client_stats.ttfb_time_ms, 0u);

    // Advance time before sending data
    ctx.advance_time(100);  // 100ms delay

    // Send data from client
    std::string data = "First byte!";
    kcpmux_stream_send(server_stream, (const uint8_t*)data.c_str(), data.size(), 1);
    pump_kcp(ctx);

    // Receive first byte on server
    uint8_t recv_buf[256];
    kcpmux_stream_recv(client_stream, recv_buf, sizeof(recv_buf));

    // Check TTFB (time from stream open to first byte received)
    kcpmux_stream_get_stats(client_stream, &client_stats);
    EXPECT_GE(client_stats.ttfb_time_ms, 100u);

    ctx.teardown();
}

TEST(kcpmux_stats_stream, write_block) {
    DualEngineContext ctx;
    ctx.client_ctx.current_time_ms = 1000;
    ctx.server_ctx.current_time_ms = 1000;
    ctx.setup();

    // Configure stream with very low pause threshold
    kcpmux_stream_config_t stream_config;
    kcpmux_stream_config_init(&stream_config);
    stream_config.send_pause_threshold = 6;
    stream_config.send_resume_threshold = 3;

    // Establish connection
    kcpmux_conn_callbacks_t client_callbacks = create_conn_callbacks(&ctx.client_ctx);
    kcpmux_conn_t *client_conn = kcpmux_conn_connect(
        ctx.client_engine,
        ctx.server_addr.get(),
        nullptr,
        nullptr,
        &client_callbacks,
        &ctx.client_ctx);
    ctx.deliver_all();

    kcpmux_conn_t *server_conn = kcpmux_engine_get_conn_by_addr(
        ctx.server_engine,
        ctx.client_addr.get());
    kcpmux_conn_callbacks_t server_callbacks = create_conn_callbacks(&ctx.server_ctx);
    kcpmux_conn_set_callbacks(server_conn, &server_callbacks, &ctx.server_ctx);

    // Create stream with low threshold
    kcpmux_stream_callbacks_t stream_callbacks = create_stream_callbacks(&ctx.client_ctx);
    kcpmux_stream_t *client_stream = kcpmux_stream_create(
        client_conn,
        &stream_config,
        &stream_callbacks,
        &ctx.client_ctx);
    pump_kcp(ctx);

    uint32_t stream_id = kcpmux_stream_id(client_stream);
    kcpmux_stream_t *server_stream = kcpmux_conn_get_stream_by_id(server_conn, stream_id);
    kcpmux_stream_callbacks_t server_stream_callbacks = create_stream_callbacks(&ctx.server_ctx);
    kcpmux_stream_set_callbacks(server_stream, &server_stream_callbacks, &ctx.server_ctx);
    pump_kcp(ctx);

    // Check write_block_count is zero initially
    kcpmux_stream_stats_t stats;
    kcpmux_stream_get_stats(client_stream, &stats);
    EXPECT_EQ(stats.write_block_count, 0u);
    EXPECT_EQ(stats.write_block_time_ms, 0u);

    // Send data until blocked
    std::string chunk = "TestData123456789";
    int blocked = 0;
    for (int i = 0; i < 100; i++) {
        int ret = kcpmux_stream_send(
            client_stream,
            (const uint8_t *)chunk.c_str(),
            chunk.size(),
            1);
        if (ret == 0) {
            blocked = 1;
            break;
        }
    }

    EXPECT_EQ(blocked, 1);
    if (blocked) {
        // Check write_block_count incremented
        kcpmux_stream_get_stats(client_stream, &stats);
        EXPECT_GE(stats.write_block_count, 1u);

        // Unblock by receiving data
        pump_kcp(ctx);
        uint8_t recv_buf[4096];
        while (kcpmux_stream_recv(server_stream, recv_buf, sizeof(recv_buf)) > 0) {}
        pump_kcp(ctx);

        // Check stats after unblock
        kcpmux_stream_get_stats(client_stream, &stats);
        // Block count should still be recorded
        EXPECT_GE(stats.write_block_count, 1u);
    }

    ctx.teardown();
}

TEST(kcpmux_stats_stream, read_block) {
    DualEngineContext ctx;
    ctx.client_ctx.current_time_ms = 1000;
    ctx.server_ctx.current_time_ms = 1000;
    ctx.setup();

    // Establish connection
    kcpmux_conn_callbacks_t client_callbacks = create_conn_callbacks(&ctx.client_ctx);
    kcpmux_conn_t *client_conn = kcpmux_conn_connect(
        ctx.client_engine,
        ctx.server_addr.get(),
        nullptr,
        nullptr,
        &client_callbacks,
        &ctx.client_ctx);
    ctx.deliver_all();

    kcpmux_conn_t *server_conn = kcpmux_engine_get_conn_by_addr(
        ctx.server_engine,
        ctx.client_addr.get());
    kcpmux_conn_callbacks_t server_callbacks = create_conn_callbacks(&ctx.server_ctx);
    kcpmux_conn_set_callbacks(server_conn, &server_callbacks, &ctx.server_ctx);

    // Create stream
    kcpmux_stream_callbacks_t stream_callbacks = create_stream_callbacks(&ctx.client_ctx);
    kcpmux_stream_t *client_stream = kcpmux_stream_create(
        client_conn,
        nullptr,
        &stream_callbacks,
        &ctx.client_ctx);

    // Send bootstrap payload to trigger server-side auto-create.
    uint8_t bootstrap = 0x7f;
    int ret = kcpmux_stream_send(client_stream, &bootstrap, 1, 1);
    ASSERT_EQ(ret, 1);

    pump_kcp(ctx);

    uint32_t stream_id = kcpmux_stream_id(client_stream);
    kcpmux_stream_t *server_stream = kcpmux_conn_get_stream_by_id(server_conn, stream_id);
    kcpmux_stream_callbacks_t server_stream_callbacks = create_stream_callbacks(&ctx.server_ctx);
    kcpmux_stream_set_callbacks(server_stream, &server_stream_callbacks, &ctx.server_ctx);
    pump_kcp(ctx);

    // Check initial read stats
    kcpmux_stream_stats_t stats;
    kcpmux_stream_get_stats(server_stream, &stats);
    EXPECT_EQ(stats.read_block_count, 1u);

    // Read bootstrap
    uint8_t recv_buf[256];
    ret = kcpmux_stream_recv(server_stream, recv_buf, sizeof(recv_buf));
    EXPECT_EQ(ret, 1);
    // Try to read when no data available (blocks)
    ret = kcpmux_stream_recv(server_stream, recv_buf, sizeof(recv_buf));
    EXPECT_EQ(ret, 0);

    // Check read_block_count incremented
    kcpmux_stream_get_stats(server_stream, &stats);
    EXPECT_EQ(stats.read_block_count, 2u);

    // Advance time while blocked
    ctx.advance_time(30);

    // Send data and receive
    std::string data = "Data arrives!";
    kcpmux_stream_send(client_stream, (const uint8_t*)data.c_str(), data.size(), 1);
    pump_kcp(ctx);

    ret = kcpmux_stream_recv(server_stream, recv_buf, sizeof(recv_buf));
    EXPECT_EQ(ret, (int)data.size());

    // Check read_block_time_ms
    kcpmux_stream_get_stats(server_stream, &stats);
    EXPECT_GE(stats.read_block_time_ms, 30u);

    ctx.teardown();
}
