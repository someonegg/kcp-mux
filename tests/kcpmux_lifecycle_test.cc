#include "kcpmux_test_utils.h"

using namespace kcpmux_test;

namespace {

struct RandomContext : TestContext {
    uint64_t random_value = 0;
    unsigned random_calls = 0;
};

uint64_t test_random_u64(void *user_data)
{
    auto *ctx = static_cast<RandomContext *>(static_cast<TestContext *>(user_data));
    ctx->random_calls++;
    return ctx->random_value;
}

kcpmux_engine_t *create_random_engine(RandomContext &ctx, bool custom_source = true)
{
    kcpmux_engine_callbacks_t callbacks{};
    callbacks.set_timer = test_set_timer;
    callbacks.write_socketv = test_write_socketv;
    callbacks.monotonic_time_ms = test_monotonic_time_ms;
    callbacks.random_u64 = custom_source ? test_random_u64 : nullptr;
    return kcpmux_engine_create(nullptr, nullptr, nullptr, &callbacks,
        static_cast<TestContext *>(&ctx), nullptr);
}

struct ImmediateTimerOwner {
    kcpmux_engine_t *engine = nullptr;
    kcpmux_timer_node_t node{};
    int callback_count = 0;
};

void immediate_timer_callback(kcpmux_timer_node_t *node, int64_t now_ms)
{
    auto *owner = static_cast<ImmediateTimerOwner *>(node->owner);
    owner->callback_count++;
    if (owner->callback_count == 1) {
        EXPECT_EQ(kcpmux_engine_schedule_timer_node(
            owner->engine, node, now_ms, now_ms), KCPMUX_ERR_OK);
    } else {
        kcpmux_engine_cancel_timer_node(owner->engine, node, now_ms);
    }
}

struct CloseOrderContext : TestContext {
    std::vector<char> notifications;
    uint8_t stream_state = KCPMUX_STREAM_STATE_ERROR;
    uint8_t conn_state = KCPMUX_CONN_STATE_ERROR;
};

void record_stream_close(kcpmux_stream_t *stream, int, void *user_data)
{
    auto *ctx = static_cast<CloseOrderContext *>(user_data);
    ctx->notifications.push_back('S');
    ctx->stream_state = stream->state;
}

void record_conn_close(kcpmux_conn_t *conn, int, void *user_data)
{
    auto *ctx = static_cast<CloseOrderContext *>(user_data);
    ctx->notifications.push_back('C');
    ctx->conn_state = conn->state;
}

struct DueFreeOwner {
    kcpmux_engine_t *engine = nullptr;
    kcpmux_timer_node_t node{};
    kcpmux_stream_t *stream = nullptr;
    int callback_count = 0;
};

void close_other_due_stream(kcpmux_timer_node_t *node, int64_t now_ms)
{
    auto *owner = static_cast<DueFreeOwner *>(node->owner);
    owner->callback_count++;
    EXPECT_EQ(kcpmux_stream_close(owner->stream), 0);
    owner->stream = nullptr;
    kcpmux_engine_cancel_timer_node(owner->engine, node, now_ms);
}

kcpmux_conn_t *create_connected_conn(kcpmux_engine_t *engine, const kcpmux_addr_t *addr)
{
    kcpmux_conn_t *conn = kcpmux_conn_new(engine, addr, nullptr, 1);
    if (!conn) return nullptr;
    kcpmux_engine_add_conn(engine, conn);
    kcpmux_conn_set_state(conn, KCPMUX_CONN_STATE_CONNECTED);
    return conn;
}

// ============================================================================
// Engine Tests
// ============================================================================

TEST(kcpmux_lifecycle, engine_create_destroy) {
    TestContext ctx;
    kcpmux_engine_t *engine = create_test_engine(&ctx);

    ASSERT_NE(engine, nullptr);
    EXPECT_EQ(ctx.timer_ms, 0u);
    EXPECT_EQ(kcpmux_timer_peek(&engine->timer_manager), nullptr);

    kcpmux_engine_destroy(engine);
}

TEST(kcpmux_lifecycle, engine_create_null_callbacks) {
    kcpmux_engine_t
        *engine = kcpmux_engine_create(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    EXPECT_EQ(engine, nullptr);
}

TEST(kcpmux_lifecycle, random_connection_start_is_nonzero_and_reserves_half_space) {
    struct Case { uint64_t random_value; uint64_t expected_start; };
    const Case cases[] = {
        {0, 1},
        {UINT64_C(1) << 63, 1},
        {UINT64_MAX, UINT64_MAX >> 1},
    };
    for (const auto &test : cases) {
        SCOPED_TRACE(test.random_value);
        RandomContext ctx;
        ctx.random_value = test.random_value;
        auto *engine = create_random_engine(ctx);
        ASSERT_NE(engine, nullptr);
        EXPECT_EQ(ctx.random_calls, 1u);
        EXPECT_EQ(engine->next_connection_id, test.expected_start);
        TestAddr addr(0x7f000001, 9000);
        auto *first = kcpmux_conn_connect(engine, addr.get(), nullptr, nullptr, nullptr, nullptr);
        auto *second = kcpmux_conn_connect(engine, addr.get(), nullptr, nullptr, nullptr, nullptr);
        ASSERT_NE(first, nullptr);
        ASSERT_NE(second, nullptr);
        EXPECT_EQ(kcpmux_conn_get_id(first), test.expected_start);
        EXPECT_EQ(kcpmux_conn_get_id(second), test.expected_start + 1);
        EXPECT_EQ(ctx.random_calls, 3u);
        kcpmux_engine_destroy(engine);
    }
}

TEST(kcpmux_lifecycle, random_stream_start_uses_low_bits_and_preserves_wraparound) {
    for (uint64_t random_value : {UINT64_C(0xabcdef0100000000), UINT64_MAX}) {
        for (uint8_t initiator : {0, 1}) {
            SCOPED_TRACE(random_value);
            SCOPED_TRACE(initiator);
            RandomContext ctx;
            auto *engine = create_random_engine(ctx);
            ASSERT_NE(engine, nullptr);
            ctx.random_value = random_value;
            TestAddr addr(0x7f000001, 9000);
            auto *conn = kcpmux_conn_new(engine, addr.get(), nullptr, initiator);
            ASSERT_NE(conn, nullptr);
            kcpmux_engine_add_conn(engine, conn);
            kcpmux_conn_set_state(conn, KCPMUX_CONN_STATE_CONNECTED);
            EXPECT_EQ(ctx.random_calls, 2u);
            const uint32_t expected = random_value == UINT64_MAX
                ? (initiator ? UINT32_MAX : UINT32_MAX - 1)
                : (initiator ? 1u : 2u);
            EXPECT_EQ(conn->next_stream_id, expected);
            auto *first = kcpmux_stream_create(conn, nullptr, nullptr, nullptr);
            auto *second = kcpmux_stream_create(conn, nullptr, nullptr, nullptr);
            ASSERT_NE(first, nullptr);
            ASSERT_NE(second, nullptr);
            EXPECT_EQ(first->stream_id, expected);
            EXPECT_EQ(second->stream_id, random_value == UINT64_MAX
                ? (initiator ? 1u : 2u) : expected + 2);
            conn->next_stream_id = expected;
            auto *third = kcpmux_stream_create(conn, nullptr, nullptr, nullptr);
            ASSERT_NE(third, nullptr);
            EXPECT_NE(third->stream_id, first->stream_id);
            EXPECT_NE(third->stream_id, second->stream_id);
            EXPECT_NE(third->stream_id, 0u);
            EXPECT_EQ(third->stream_id & 1u, initiator);
            EXPECT_EQ(ctx.random_calls, 2u);
            kcpmux_engine_destroy(engine);
        }
    }
}

TEST(kcpmux_lifecycle, random_callback_is_optional) {
    RandomContext ctx;
    auto *engine = create_random_engine(ctx, false);
    ASSERT_NE(engine, nullptr);
    EXPECT_NE(engine->next_connection_id, 0u);
    EXPECT_LE(engine->next_connection_id, UINT64_MAX >> 1);
    TestAddr addr(0x7f000001, 9000);
    auto *conn = kcpmux_conn_connect(engine, addr.get(), nullptr, nullptr, nullptr, nullptr);
    ASSERT_NE(conn, nullptr);
    EXPECT_NE(conn->next_stream_id, 0u);
    EXPECT_EQ(conn->next_stream_id & 1u, 1u);
    EXPECT_EQ(ctx.random_calls, 0u);
    kcpmux_engine_destroy(engine);
}

TEST(kcpmux_lifecycle, engine_create_rejects_incomplete_dependencies) {
    TestContext ctx;
    kcpmux_engine_callbacks_t callbacks{};
    callbacks.set_timer = test_set_timer;
    callbacks.write_socketv = test_write_socketv;
    callbacks.monotonic_time_ms = test_monotonic_time_ms;

    kcpmux_engine_callbacks_t incomplete_callbacks = callbacks;
    incomplete_callbacks.write_socketv = nullptr;
    EXPECT_EQ(kcpmux_engine_create(
        nullptr, nullptr, nullptr, &incomplete_callbacks, &ctx, nullptr), nullptr);

    incomplete_callbacks = callbacks;
    incomplete_callbacks.monotonic_time_ms = nullptr;
    EXPECT_EQ(kcpmux_engine_create(
        nullptr, nullptr, nullptr, &incomplete_callbacks, &ctx, nullptr), nullptr);

    kcpmux_kcp_ops_t incomplete_ops = *kcpmux_default_kcp_ops();
    EXPECT_NE(incomplete_ops.sendv, nullptr);
    incomplete_ops.current_update = nullptr;
    EXPECT_EQ(kcpmux_engine_create(
        nullptr, nullptr, nullptr, &callbacks, &ctx, &incomplete_ops), nullptr);
}

TEST(kcpmux_lifecycle, set_timer_zero_requests_async_update) {
    TestContext ctx;
    ctx.current_time_ms = 1000;
    kcpmux_engine_t *engine = create_test_engine(&ctx);
    ASSERT_NE(engine, nullptr);

    ImmediateTimerOwner owner;
    owner.engine = engine;
    ASSERT_EQ(kcpmux_engine_register_timer_node(
        engine, &owner.node, &owner, immediate_timer_callback), KCPMUX_ERR_OK);
    ASSERT_EQ(kcpmux_engine_schedule_timer_node(
        engine, &owner.node, 1000, 1000), KCPMUX_ERR_OK);
    EXPECT_EQ(ctx.timer_ms, 0u);
    EXPECT_EQ(owner.callback_count, 0);

    kcpmux_engine_update(engine);
    EXPECT_EQ(owner.callback_count, 1);
    EXPECT_EQ(owner.node.state, KCPMUX_TIMER_HEAP);
    EXPECT_EQ(ctx.timer_ms, 0u);

    kcpmux_engine_update(engine);
    EXPECT_EQ(owner.callback_count, 2);
    EXPECT_EQ(owner.node.state, KCPMUX_TIMER_IDLE);
    EXPECT_EQ(kcpmux_timer_peek(&engine->timer_manager), nullptr);

    kcpmux_engine_unregister_timer_node(engine, &owner.node);
    kcpmux_engine_destroy(engine);
}

TEST(kcpmux_lifecycle, owner_registration_reserves_heap_capacity) {
    TestContext ctx;
    ctx.current_time_ms = 1000;
    kcpmux_engine_t *engine = create_test_engine(&ctx);
    ASSERT_NE(engine, nullptr);
    ImmediateTimerOwner owners[8];
    for (ImmediateTimerOwner &owner : owners) {
        owner.engine = engine;
        ASSERT_EQ(kcpmux_engine_register_timer_node(
                      engine, &owner.node, &owner, immediate_timer_callback),
                  KCPMUX_ERR_OK);
    }

    ASSERT_GE(engine->timer_manager.capacity, 8u);
    size_t reserved_capacity = engine->timer_manager.capacity;
    for (ImmediateTimerOwner &owner : owners) {
        ASSERT_EQ(kcpmux_engine_schedule_timer_node(
                      engine, &owner.node, ctx.current_time_ms + 2000,
                      ctx.current_time_ms),
                  KCPMUX_ERR_OK);
        EXPECT_EQ(engine->timer_manager.capacity, reserved_capacity);
    }

    for (ImmediateTimerOwner &owner : owners) {
        kcpmux_engine_unregister_timer_node(engine, &owner.node);
    }
    kcpmux_engine_destroy(engine);
}

TEST(kcpmux_lifecycle, conn_finalization_closes_streams_before_conn_notification) {
    CloseOrderContext ctx;
    kcpmux_engine_t *engine = create_test_engine(&ctx);
    TestAddr addr(0x7f000001, 12008);
    kcpmux_conn_t *conn = create_connected_conn(engine, addr.get());
    ASSERT_NE(conn, nullptr);
    kcpmux_conn_callbacks_t conn_callbacks{};
    conn_callbacks.conn_close_notify = record_conn_close;
    kcpmux_conn_set_callbacks(conn, &conn_callbacks, &ctx);
    kcpmux_stream_callbacks_t stream_callbacks{};
    stream_callbacks.stream_close_notify = record_stream_close;
    kcpmux_stream_t *stream = kcpmux_stream_create(conn, nullptr, &stream_callbacks, &ctx);
    ASSERT_NE(stream, nullptr);

    kcpmux_engine_operation_enter(engine);
    kcpmux_conn_close_internal(conn, KCPMUX_CLOSE_REASON_NORMAL);

    ASSERT_EQ(ctx.notifications.size(), 2u);
    EXPECT_EQ(ctx.notifications[0], 'S');
    EXPECT_EQ(ctx.notifications[1], 'C');
    EXPECT_EQ(ctx.stream_state, KCPMUX_STREAM_STATE_CLOSED);
    EXPECT_EQ(ctx.conn_state, KCPMUX_CONN_STATE_CLOSED);
    EXPECT_EQ(engine->conn_count, 0u);
    EXPECT_EQ(engine->timer_node_count, 0u);
    kcpmux_engine_operation_leave(engine);
    kcpmux_engine_destroy(engine);
}

TEST(kcpmux_lifecycle, due_callback_can_close_another_due_owner) {
    TestContext ctx;
    ctx.current_time_ms = 500;
    kcpmux_engine_t *engine = create_test_engine(&ctx);
    TestAddr addr(0x7f000001, 12004);
    kcpmux_conn_t *conn = create_connected_conn(engine, addr.get());
    ASSERT_NE(conn, nullptr);
    kcpmux_stream_t *stream = kcpmux_stream_create(conn, nullptr, nullptr, nullptr);
    ASSERT_NE(stream, nullptr);

    DueFreeOwner owner;
    owner.engine = engine;
    owner.stream = stream;
    ASSERT_EQ(kcpmux_engine_register_timer_node(
        engine, &owner.node, &owner, close_other_due_stream), KCPMUX_ERR_OK);
    kcpmux_engine_cancel_timer_node(engine, &stream->timer_node, 500);
    ASSERT_EQ(kcpmux_engine_schedule_timer_node(
        engine, &owner.node, 500, 500), KCPMUX_ERR_OK);
    ASSERT_EQ(kcpmux_engine_schedule_timer_node(
        engine, &stream->timer_node, 500, 500), KCPMUX_ERR_OK);

    kcpmux_engine_update(engine);

    EXPECT_EQ(owner.callback_count, 1);
    EXPECT_EQ(conn->stream_count, 0u);
    EXPECT_EQ(owner.node.state, KCPMUX_TIMER_IDLE);
    kcpmux_engine_unregister_timer_node(engine, &owner.node);
    kcpmux_engine_destroy(engine);
}

TEST(kcpmux_lifecycle, empty_heap_stale_timer_fire_is_consumed_without_rearm) {
    TestContext ctx;
    kcpmux_engine_t *engine = create_test_engine(&ctx);
    TestAddr addr(0x7f000001, 12006);
    kcpmux_conn_t
        *conn = kcpmux_conn_connect(engine, addr.get(), nullptr, nullptr, nullptr, nullptr);
    ASSERT_NE(conn, nullptr);
    ASSERT_TRUE(ctx.timer_probe.armed);
    kcpmux_engine_operation_enter(engine);
    kcpmux_conn_close_internal(conn, KCPMUX_CLOSE_REASON_NORMAL);
    kcpmux_engine_operation_leave(engine);

    ASSERT_EQ(kcpmux_timer_peek(&engine->timer_manager), nullptr);
    size_t set_count = ctx.timer_probe.history.size();
    ctx.timer_probe.consume();
    kcpmux_engine_update(engine);

    EXPECT_FALSE(ctx.timer_probe.armed);
    EXPECT_EQ(ctx.timer_probe.history.size(), set_count);
    kcpmux_engine_destroy(engine);
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
    EXPECT_EQ(stream_config.drain_timeout_ms, KCPMUX_DEFAULT_SDRAIN_TIMEOUT_MS);
    EXPECT_EQ(stream_config.batch_threshold, KCPMUX_DEFAULT_BATCH_THRESHOLD);
}

TEST(kcpmux_lifecycle, engine_prepares_and_validates_default_configs) {
    TestContext ctx;
    kcpmux_engine_callbacks_t callbacks{};
    callbacks.set_timer = test_set_timer;
    callbacks.write_socketv = test_write_socketv;
    callbacks.monotonic_time_ms = test_monotonic_time_ms;

    kcpmux_conn_config_t conn_config;
    kcpmux_conn_config_init(&conn_config);
    conn_config.keepalive_timeout_ms = 0;

    kcpmux_stream_config_t stream_config;
    kcpmux_stream_config_init(&stream_config);
    stream_config.kcp_mss = 1463;

    kcpmux_engine_t *engine = kcpmux_engine_create(
        nullptr, &conn_config, &stream_config, &callbacks, &ctx, nullptr);
    ASSERT_NE(engine, nullptr);
    EXPECT_EQ(engine->default_conn_config.keepalive_timeout_ms,
              KCPMUX_DEFAULT_KEEPALIVE_TIMEOUT_MS);
    EXPECT_EQ(engine->default_stream_config.kcp_mss, 1463);
    kcpmux_engine_destroy(engine);

    conn_config.ctrl_timeout_ms = 0;
    EXPECT_EQ(kcpmux_engine_create(
        nullptr, &conn_config, nullptr, &callbacks, &ctx, nullptr), nullptr);

    kcpmux_conn_config_init(&conn_config);
    stream_config.kcp_mss = 1464;
    EXPECT_EQ(kcpmux_engine_create(
        nullptr, &conn_config, &stream_config, &callbacks, &ctx, nullptr), nullptr);
}

TEST(kcpmux_lifecycle, stream_config_validation_boundaries) {
    kcpmux_stream_config_t source;
    kcpmux_stream_config_init(&source);
    kcpmux_stream_config_t prepared;

    source.kcp_mss = 1;
    EXPECT_TRUE(kcpmux_stream_config_prepare(&prepared, &source));
    source.kcp_mss = 1463;
    EXPECT_TRUE(kcpmux_stream_config_prepare(&prepared, &source));
    source.kcp_mss = 0;
    EXPECT_FALSE(kcpmux_stream_config_prepare(&prepared, &source));
    source.kcp_mss = 1464;
    EXPECT_FALSE(kcpmux_stream_config_prepare(&prepared, &source));

    kcpmux_stream_config_init(&source);
    source.ctrl_timeout_ms = 0;
    EXPECT_FALSE(kcpmux_stream_config_prepare(&prepared, &source));
    kcpmux_stream_config_init(&source);
    source.drain_timeout_ms = 0;
    EXPECT_FALSE(kcpmux_stream_config_prepare(&prepared, &source));
    kcpmux_stream_config_init(&source);
    source.send_pause_threshold = 0;
    EXPECT_FALSE(kcpmux_stream_config_prepare(&prepared, &source));
    kcpmux_stream_config_init(&source);
    source.send_resume_threshold = source.send_pause_threshold + 1;
    EXPECT_FALSE(kcpmux_stream_config_prepare(&prepared, &source));
}

TEST(kcpmux_lifecycle, config_setters_reject_invalid_and_keep_stream_mss) {
    TestContext ctx;
    kcpmux_engine_t *engine = create_test_engine(&ctx);
    ASSERT_NE(engine, nullptr);
    TestAddr addr(0x7f000001, 12345);
    kcpmux_conn_t *conn = create_connected_conn(engine, addr.get());
    ASSERT_NE(conn, nullptr);

    kcpmux_conn_config_t conn_config = conn->config;
    conn_config.keepalive_timeout_ms = 0;
    kcpmux_conn_set_config(conn, &conn_config);
    EXPECT_EQ(conn->config.keepalive_timeout_ms,
              KCPMUX_DEFAULT_KEEPALIVE_TIMEOUT_MS);
    kcpmux_conn_config_t accepted_conn_config = conn->config;
    conn_config.ctrl_timeout_ms = 0;
    kcpmux_conn_set_config(conn, &conn_config);
    EXPECT_EQ(memcmp(&conn->config, &accepted_conn_config, sizeof(conn->config)), 0);
    TestAddr other_addr(0x7f000001, 12346);
    EXPECT_EQ(kcpmux_conn_connect(
        engine, other_addr.get(), &conn_config, nullptr, nullptr, nullptr), nullptr);

    kcpmux_stream_t *stream = kcpmux_stream_create(conn, nullptr, nullptr, nullptr);
    ASSERT_NE(stream, nullptr);
    kcpmux_stream_config_t stream_config = stream->config;
    stream_config.kcp_mss = 1000;
    stream_config.batch_threshold = 4;
    kcpmux_stream_set_config(stream, &stream_config);
    EXPECT_EQ(stream->config.kcp_mss, KCPMUX_DEFAULT_KCP_MSS);
    EXPECT_EQ(stream->config.batch_threshold, 4u);

    kcpmux_stream_config_t accepted_stream_config = stream->config;
    stream_config.send_pause_threshold = 0;
    kcpmux_stream_set_config(stream, &stream_config);
    EXPECT_EQ(memcmp(
        &stream->config, &accepted_stream_config, sizeof(stream->config)), 0);

    stream_config = accepted_stream_config;
    stream_config.send_resume_threshold = stream_config.send_pause_threshold + 1;
    EXPECT_EQ(kcpmux_stream_create(conn, &stream_config, nullptr, nullptr), nullptr);

    kcpmux_engine_destroy(engine);
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
        ctx.client_engine,
        ctx.server_addr.get(),
        nullptr,
        nullptr,
        &client_callbacks,
        &ctx.client_ctx);
    ASSERT_NE(client_conn, nullptr);
    EXPECT_EQ(kcpmux_conn_get_state(client_conn), KCPMUX_CONN_STATE_CONNECTING);

    // Deliver CONN_CONNECT to server
    ctx.deliver_client_to_server();

    // Server should have accepted and created connection
    kcpmux_conn_t *server_conn = find_only_conn(
        ctx.server_engine,
        ctx.client_addr.get());
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
        ctx.client_engine,
        ctx.server_addr.get(),
        nullptr,
        nullptr,
        &client_callbacks,
        &ctx.client_ctx);
    ctx.deliver_all();

    kcpmux_conn_t *server_conn = find_only_conn(
        ctx.server_engine,
        ctx.client_addr.get());
    kcpmux_conn_callbacks_t server_callbacks = create_conn_callbacks(&ctx.server_ctx);
    kcpmux_conn_set_callbacks(server_conn, &server_callbacks, &ctx.server_ctx);

    // Client closes connection
    ctx.client_ctx.conn_state_changes.clear();
    kcpmux_conn_close(client_conn);
    EXPECT_EQ(kcpmux_conn_get_state(client_conn), KCPMUX_CONN_STATE_CLOSING);

    // Deliver CONN_CLOSE to server
    ctx.deliver_client_to_server();
    EXPECT_EQ(ctx.server_ctx.conn_close_state, KCPMUX_CONN_STATE_CLOSED);
    EXPECT_EQ(find_only_conn(
                  ctx.server_engine, ctx.client_addr.get()), nullptr);

    // Deliver CONN_CLOSE_ACK to client
    ctx.deliver_server_to_client();
    EXPECT_EQ(ctx.client_ctx.conn_close_state, KCPMUX_CONN_STATE_CLOSED);
    EXPECT_EQ(find_only_conn(
                  ctx.client_engine, ctx.server_addr.get()), nullptr);

    ctx.teardown();
}

TEST(kcpmux_lifecycle, conn_state_callbacks) {
    DualEngineContext ctx;
    ctx.setup();

    // Establish connection with callbacks
    kcpmux_conn_callbacks_t client_callbacks = create_conn_callbacks(&ctx.client_ctx);
    ctx.client_ctx.conn_state_changes.clear();

    kcpmux_conn_t *client_conn = kcpmux_conn_connect(
        ctx.client_engine,
        ctx.server_addr.get(),
        nullptr,
        nullptr,
        &client_callbacks,
        &ctx.client_ctx);
    ASSERT_NE(client_conn, nullptr);
    EXPECT_EQ(kcpmux_conn_get_state(client_conn), KCPMUX_CONN_STATE_CONNECTING);
    EXPECT_TRUE(ctx.client_ctx.conn_state_changes.empty());
    EXPECT_EQ(ctx.client_ctx.conn_close_count, 0);
    EXPECT_EQ(ctx.client_ctx.stream_notify_count, 0);

    // Complete handshake; the initial state must never be replayed.
    ctx.deliver_all();
    ASSERT_EQ(ctx.client_ctx.conn_state_changes.size(), 1u);
    EXPECT_EQ(ctx.client_ctx.conn_state_changes[0].first, KCPMUX_CONN_STATE_CONNECTING);
    EXPECT_EQ(ctx.client_ctx.conn_state_changes[0].second, KCPMUX_CONN_STATE_CONNECTED);

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
        ctx.client_engine,
        ctx.server_addr.get(),
        nullptr,
        nullptr,
        &client_callbacks,
        &ctx.client_ctx);
    ASSERT_NE(client_conn, nullptr);

    // Deliver CONN_CONNECT to server (server rejects)
    ctx.deliver_client_to_server();

    // Deliver reject ACK to client
    ctx.deliver_server_to_client();

    EXPECT_TRUE(ctx.client_ctx.conn_state_changes.empty());

    // Client should be in ERROR state
    EXPECT_EQ(ctx.client_ctx.conn_close_state, KCPMUX_CONN_STATE_ERROR);
    EXPECT_EQ(find_only_conn(
                  ctx.client_engine, ctx.server_addr.get()), nullptr);
    EXPECT_EQ(ctx.client_ctx.conn_close_count, 1);
    EXPECT_EQ(ctx.client_ctx.conn_close_reason, KCPMUX_CLOSE_REASON_REJECTED);
    for (const auto &change : ctx.client_ctx.conn_state_changes) {
        EXPECT_NE(change.second, KCPMUX_CONN_STATE_CLOSED);
        EXPECT_NE(change.second, KCPMUX_CONN_STATE_ERROR);
    }

    ctx.teardown();
}

TEST(kcpmux_lifecycle, conn_initial_send_failure_defers_notifications) {
    TestContext ctx;
    kcpmux_engine_t *engine = create_test_engine(&ctx);
    ASSERT_NE(engine, nullptr);
    engine->callbacks.write_socketv = [](const kcpmux_iovec_t *, unsigned,
                                         const kcpmux_addr_t *, void *) { return 0; };
    kcpmux_conn_config_t config;
    kcpmux_conn_config_init(&config);
    config.ctrl_timeout_ms = 100;
    config.connect_retries = 1;
    TestAddr addr(0x7f000001, 12345);
    auto callbacks = create_conn_callbacks(&ctx);
    auto *conn = kcpmux_conn_connect(engine, addr.get(), &config, nullptr, &callbacks, &ctx);
    ASSERT_NE(conn, nullptr);
    EXPECT_EQ(kcpmux_conn_get_state(conn), KCPMUX_CONN_STATE_CONNECTING);
    EXPECT_TRUE(ctx.conn_state_changes.empty());
    EXPECT_EQ(ctx.conn_close_count, 0);
    EXPECT_EQ(ctx.stream_notify_count, 0);
    EXPECT_EQ(engine->stats.tx_error_packets, 1u);
    EXPECT_EQ(ctx.timer_probe.deadline_ms, 100);

    engine->callbacks.write_socketv = test_write_socketv;
    ctx.current_time_ms = 100;
    kcpmux_engine_update(engine);
    ASSERT_EQ(ctx.sent_packets.size(), 1u);
    EXPECT_EQ(ctx.sent_packets[0][0], KCPMUX_MSG_CONN_CONNECT);
    EXPECT_EQ(ctx.conn_close_count, 0);
    ctx.current_time_ms = 200;
    kcpmux_engine_update(engine);
    EXPECT_TRUE(ctx.conn_state_changes.empty());
    EXPECT_EQ(ctx.conn_close_count, 1);
    EXPECT_EQ(ctx.conn_close_reason, KCPMUX_CLOSE_REASON_TIMEOUT);
    kcpmux_engine_destroy(engine);
}

TEST(kcpmux_lifecycle, conn_close_immediately_after_connect) {
    TestContext ctx;
    auto *engine = create_test_engine(&ctx);
    ASSERT_NE(engine, nullptr);
    kcpmux_conn_config_t config;
    kcpmux_conn_config_init(&config);
    config.close_retries = 0;
    TestAddr addr(0x7f000001, 12345);
    auto callbacks = create_conn_callbacks(&ctx);
    auto *conn = kcpmux_conn_connect(engine, addr.get(), &config, nullptr, &callbacks, &ctx);
    ASSERT_NE(conn, nullptr);
    EXPECT_TRUE(ctx.conn_state_changes.empty());
    EXPECT_EQ(ctx.conn_close_count, 0);
    ASSERT_EQ(kcpmux_conn_close(conn), 0);
    ASSERT_EQ(ctx.conn_state_changes.size(), 1u);
    EXPECT_EQ(ctx.conn_state_changes[0].first, KCPMUX_CONN_STATE_CONNECTING);
    EXPECT_EQ(ctx.conn_state_changes[0].second, KCPMUX_CONN_STATE_CLOSING);
    EXPECT_EQ(ctx.conn_close_count, 1);
    EXPECT_EQ(ctx.conn_close_reason, KCPMUX_CLOSE_REASON_NORMAL);
    ASSERT_EQ(ctx.sent_packets.size(), 1u); // Only CONNECT, no zero-ID CLOSE.
    EXPECT_EQ(ctx.sent_packets[0][0], KCPMUX_MSG_CONN_CONNECT);
    kcpmux_engine_update(engine);
    EXPECT_EQ(ctx.conn_state_changes.size(), 1u);
    EXPECT_EQ(ctx.conn_close_count, 1);
    kcpmux_engine_destroy(engine);
}

TEST(kcpmux_lifecycle, same_address_connections_coexist) {
    TestContext ctx;
    kcpmux_engine_t *engine = create_test_engine(&ctx);
    ASSERT_NE(engine, nullptr);

    TestAddr addr(0x7f000001, 12345);

    kcpmux_conn_t
        *conn1 = kcpmux_conn_connect(engine, addr.get(), nullptr, nullptr, nullptr, nullptr);
    ASSERT_NE(conn1, nullptr);

    kcpmux_conn_t
        *conn2 = kcpmux_conn_connect(engine, addr.get(), nullptr, nullptr, nullptr, nullptr);
    ASSERT_NE(conn2, nullptr);
    EXPECT_NE(kcpmux_conn_get_id(conn1), kcpmux_conn_get_id(conn2));
    EXPECT_EQ(engine->conn_count, 2u);

    kcpmux_engine_destroy(engine);
}

TEST(kcpmux_lifecycle, closing_connection_coexists_with_new_connection) {
    TestContext ctx;
    kcpmux_engine_t *engine = create_test_engine(&ctx);
    ASSERT_NE(engine, nullptr);
    TestAddr addr(0x7f000001, 12346);
    kcpmux_conn_callbacks_t callbacks = create_conn_callbacks(&ctx);
    kcpmux_conn_t *old_conn = kcpmux_conn_connect(
        engine, addr.get(), nullptr, nullptr, &callbacks, &ctx);
    ASSERT_NE(old_conn, nullptr);
    const uint64_t old_id = kcpmux_conn_get_id(old_conn);
    ASSERT_EQ(kcpmux_conn_close(old_conn), 0);
    ASSERT_EQ(old_conn->state, KCPMUX_CONN_STATE_CLOSING);

    kcpmux_conn_t *new_conn = kcpmux_conn_connect(
        engine, addr.get(), nullptr, nullptr, &callbacks, &ctx);
    ASSERT_NE(new_conn, nullptr);
    const uint64_t new_id = kcpmux_conn_get_id(new_conn);
    EXPECT_NE(new_id, old_id);
    EXPECT_EQ(ctx.conn_close_count, 0);
    EXPECT_EQ(engine->conn_count, 2u);
    EXPECT_EQ(kcpmux_engine_get_conn_by_id(engine, addr.get(), old_id), old_conn);
    EXPECT_EQ(kcpmux_engine_get_conn_by_id(engine, addr.get(), new_id), new_conn);

    auto ack = build_conn_connect_ack(KCPMUX_ACK_RESULT_OK, new_id);
    ASSERT_EQ(kcpmux_engine_input(engine, ack.data(), ack.size(), addr.get()), 0);
    EXPECT_EQ(new_conn->state, KCPMUX_CONN_STATE_CONNECTED);
    auto old_close_ack = build_conn_close_ack(KCPMUX_CLOSE_REASON_NORMAL, old_id);
    ASSERT_EQ(kcpmux_engine_input(engine, old_close_ack.data(), old_close_ack.size(), addr.get()), 0);
    EXPECT_EQ(ctx.conn_close_count, 1);
    EXPECT_EQ(kcpmux_engine_get_conn_by_id(engine, addr.get(), old_id), nullptr);
    EXPECT_EQ(kcpmux_engine_get_conn_by_id(engine, addr.get(), new_id), new_conn);
    EXPECT_EQ(engine->conn_count, 1u);

    auto stale_close = build_conn_close(KCPMUX_CLOSE_REASON_NORMAL, old_id);
    EXPECT_EQ(kcpmux_engine_input(engine, stale_close.data(), stale_close.size(), addr.get()),
              -KCPMUX_ERR_NOT_FOUND);
    EXPECT_EQ(new_conn->state, KCPMUX_CONN_STATE_CONNECTED);
    kcpmux_engine_destroy(engine);
}

TEST(kcpmux_lifecycle, conn_get_by_id) {
    TestContext ctx;
    kcpmux_engine_t *engine = create_test_engine(&ctx);
    ASSERT_NE(engine, nullptr);

    TestAddr addr(0x7f000001, 12345);

    kcpmux_conn_t
        *conn = kcpmux_conn_connect(engine, addr.get(), nullptr, nullptr, nullptr, nullptr);
    ASSERT_NE(conn, nullptr);

    kcpmux_conn_t *found = kcpmux_engine_get_conn_by_id(engine, addr.get(), kcpmux_conn_get_id(conn));
    EXPECT_EQ(found, conn);

    TestAddr other_addr(0x7f000002, 12345);
    kcpmux_conn_t *not_found = kcpmux_engine_get_conn_by_id(engine, other_addr.get(), kcpmux_conn_get_id(conn));
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
    kcpmux_conn_t
        *conn = kcpmux_conn_connect(engine, addr.get(), &conn_config, nullptr, &callbacks, &ctx);
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
    EXPECT_EQ(ctx.conn_close_state, KCPMUX_CONN_STATE_ERROR);
    EXPECT_EQ(find_only_conn(engine, addr.get()), nullptr);
    EXPECT_EQ(ctx.conn_close_count, 1);  // close_notify should be called
    EXPECT_EQ(ctx.conn_close_reason, KCPMUX_CLOSE_REASON_TIMEOUT);
    EXPECT_TRUE(ctx.conn_state_changes.empty());

    kcpmux_engine_destroy(engine);
}

TEST(kcpmux_lifecycle, conn_control_deadline_does_not_drift) {
    TestContext ctx;
    ctx.current_time_ms = 1000;

    kcpmux_conn_config_t config;
    kcpmux_conn_config_init(&config);
    config.ctrl_timeout_ms = 100;
    config.connect_retries = 1;

    kcpmux_engine_t *engine = create_test_engine(&ctx);
    ASSERT_NE(engine, nullptr);
    TestAddr addr(0x7f000001, 12345);
    kcpmux_conn_t
        *conn = kcpmux_conn_connect(engine, addr.get(), &config, nullptr, nullptr, nullptr);
    ASSERT_NE(conn, nullptr);

    ASSERT_NE(kcpmux_timer_peek(&engine->timer_manager), nullptr);
    EXPECT_EQ(kcpmux_timer_peek(&engine->timer_manager)->deadline_ms, 1100);
    EXPECT_EQ(ctx.timer_ms, 100u);
    ctx.sent_packets.clear();

    ctx.current_time_ms = 1050;
    kcpmux_engine_update(engine);
    EXPECT_EQ(kcpmux_timer_peek(&engine->timer_manager)->deadline_ms, 1100);
    EXPECT_EQ(ctx.timer_ms, 50u);
    EXPECT_TRUE(ctx.sent_packets.empty());

    ctx.current_time_ms = 1099;
    kcpmux_engine_update(engine);
    EXPECT_EQ(kcpmux_timer_peek(&engine->timer_manager)->deadline_ms, 1100);
    EXPECT_EQ(ctx.timer_ms, 1u);
    EXPECT_TRUE(ctx.sent_packets.empty());

    ctx.current_time_ms = 1100;
    kcpmux_engine_update(engine);
    ASSERT_EQ(ctx.sent_packets.size(), 1u);
    EXPECT_EQ(conn->retry_count, 1u);
    EXPECT_EQ(kcpmux_timer_peek(&engine->timer_manager)->deadline_ms, 1200);
    EXPECT_EQ(ctx.timer_ms, 100u);

    kcpmux_engine_destroy(engine);
}

TEST(kcpmux_lifecycle, conn_config_replaces_timer_earlier_and_later) {
    TestContext ctx;
    ctx.current_time_ms = 1000;

    kcpmux_conn_config_t config;
    kcpmux_conn_config_init(&config);
    config.keepalive_interval_ms = 1000;
    config.keepalive_timeout_ms = 2000;
    config.idle_timeout_ms = 0;

    kcpmux_engine_t *engine = create_test_engine(&ctx);
    ASSERT_NE(engine, nullptr);
    TestAddr addr(0x7f000001, 12345);
    kcpmux_conn_t
        *conn = kcpmux_conn_connect(engine, addr.get(), &config, nullptr, nullptr, nullptr);
    ASSERT_NE(conn, nullptr);

    auto ack = build_conn_connect_ack(KCPMUX_ACK_RESULT_OK, conn->local_connection_id);
    ASSERT_EQ(kcpmux_engine_input(
        engine, ack.data(), (unsigned)ack.size(), addr.get()), 0);
    EXPECT_EQ(kcpmux_timer_peek(&engine->timer_manager)->deadline_ms, 2000);

    ctx.current_time_ms = 1100;
    config.keepalive_interval_ms = 50;
    size_t replace_count = ctx.timer_probe.history.size();
    kcpmux_conn_set_config(conn, &config);
    EXPECT_EQ(kcpmux_timer_peek(&engine->timer_manager)->deadline_ms, 1100);
    EXPECT_EQ(engine->external_timer_deadline_ms, 1100);
    EXPECT_EQ(ctx.timer_ms, 0u);
    ASSERT_EQ(ctx.timer_probe.history.size(), replace_count + 1);
    EXPECT_EQ(ctx.timer_probe.history.back().callback_time_ms, 1100);
    EXPECT_EQ(ctx.timer_probe.history.back().delay_ms, 0u);
    EXPECT_EQ(ctx.timer_probe.history.back().deadline_ms, 1100);

    config.keepalive_interval_ms = 500;
    kcpmux_conn_set_config(conn, &config);
    EXPECT_EQ(kcpmux_timer_peek(&engine->timer_manager)->deadline_ms, 1500);
    EXPECT_EQ(engine->external_timer_deadline_ms, 1500);
    EXPECT_EQ(ctx.timer_ms, 400u);
    ASSERT_EQ(ctx.timer_probe.history.size(), replace_count + 2);
    EXPECT_EQ(ctx.timer_probe.history.back().callback_time_ms, 1100);
    EXPECT_EQ(ctx.timer_probe.history.back().delay_ms, 400u);
    EXPECT_EQ(ctx.timer_probe.history.back().deadline_ms, 1500);

    kcpmux_engine_destroy(engine);
}

TEST(kcpmux_lifecycle, conn_connected_deadline_uses_all_anchors) {
    TestContext ctx;
    ctx.current_time_ms = 1000;

    kcpmux_conn_config_t config;
    kcpmux_conn_config_init(&config);
    config.keepalive_interval_ms = 1000;
    config.keepalive_timeout_ms = 300;
    config.idle_timeout_ms = 200;

    kcpmux_engine_t *engine = create_test_engine(&ctx);
    ASSERT_NE(engine, nullptr);
    TestAddr addr(0x7f000001, 12345);
    kcpmux_conn_t
        *conn = kcpmux_conn_connect(engine, addr.get(), &config, nullptr, nullptr, nullptr);
    ASSERT_NE(conn, nullptr);

    auto ack = build_conn_connect_ack(KCPMUX_ACK_RESULT_OK, conn->local_connection_id);
    ASSERT_EQ(kcpmux_engine_input(
        engine, ack.data(), (unsigned)ack.size(), addr.get()), 0);
    EXPECT_EQ(kcpmux_timer_peek(&engine->timer_manager)->deadline_ms, 1200);

    ctx.current_time_ms = 1100;
    kcpmux_conn_note_receive(conn, 1100, 1);
    EXPECT_EQ(conn->last_recv_ts, 1100);
    EXPECT_EQ(conn->last_payload_ts, 1100);
    EXPECT_EQ(kcpmux_timer_peek(&engine->timer_manager)->deadline_ms, 1300);
    EXPECT_EQ(engine->external_timer_deadline_ms, 1300);
    EXPECT_EQ(ctx.timer_ms, 200u);

    kcpmux_engine_destroy(engine);
}

TEST(kcpmux_lifecycle, zero_keepalive_interval_disables_sending) {
    TestContext ctx;
    ctx.current_time_ms = 1000;

    kcpmux_conn_config_t config;
    kcpmux_conn_config_init(&config);
    config.keepalive_interval_ms = 0;
    config.keepalive_timeout_ms = 500;
    config.idle_timeout_ms = 0;

    kcpmux_engine_t *engine = create_test_engine(&ctx);
    ASSERT_NE(engine, nullptr);
    TestAddr addr(0x7f000001, 12345);
    kcpmux_conn_t
        *conn = kcpmux_conn_connect(engine, addr.get(), &config, nullptr, nullptr, nullptr);
    ASSERT_NE(conn, nullptr);

    auto ack = build_conn_connect_ack(KCPMUX_ACK_RESULT_OK, conn->local_connection_id);
    ASSERT_EQ(kcpmux_engine_input(
        engine, ack.data(), (unsigned)ack.size(), addr.get()), 0);
    ctx.sent_packets.clear();

    ASSERT_NE(kcpmux_timer_peek(&engine->timer_manager), nullptr);
    EXPECT_EQ(kcpmux_timer_peek(&engine->timer_manager)->deadline_ms, 1500);
    EXPECT_EQ(engine->external_timer_deadline_ms, 1500);
    EXPECT_EQ(ctx.timer_ms, 500u);

    kcpmux_engine_update(engine);
    EXPECT_TRUE(ctx.sent_packets.empty());
    EXPECT_EQ(kcpmux_timer_peek(&engine->timer_manager)->deadline_ms, 1500);
    EXPECT_EQ(ctx.timer_ms, 500u);

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
        ctx.client_engine,
        ctx.server_addr.get(),
        &conn_config,
        nullptr,
        &client_callbacks,
        &ctx.client_ctx);
    ASSERT_NE(client_conn, nullptr);

    // Complete handshake
    ctx.deliver_all();
    EXPECT_EQ(kcpmux_conn_get_state(client_conn), KCPMUX_CONN_STATE_CONNECTED);

    // Advance time past keepalive timeout without any data exchange
    ctx.client_ctx.current_time_ms += 501;
    ctx.server_ctx.current_time_ms += 501;
    kcpmux_engine_update(ctx.client_engine);

    // Client connection should be in ERROR state due to keepalive timeout
    EXPECT_EQ(ctx.client_ctx.conn_close_state, KCPMUX_CONN_STATE_ERROR);
    EXPECT_EQ(find_only_conn(
                  ctx.client_engine, ctx.server_addr.get()), nullptr);
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
        ctx.client_engine,
        ctx.server_addr.get(),
        &conn_config,
        nullptr,
        &client_callbacks,
        &ctx.client_ctx);
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
    EXPECT_EQ(ctx.client_ctx.conn_close_state, KCPMUX_CONN_STATE_CLOSED);
    EXPECT_EQ(find_only_conn(
                  ctx.client_engine, ctx.server_addr.get()), nullptr);
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
        ctx.client_engine,
        ctx.server_addr.get(),
        &conn_config,
        nullptr,
        &client_callbacks,
        &ctx.client_ctx);
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

    // The only retry is the final send, so the connection closes immediately.
    ASSERT_EQ(ctx.client_ctx.sent_packets.size(), 1u);
    EXPECT_EQ(ctx.client_ctx.sent_packets[0][0], KCPMUX_MSG_CONN_CLOSE);
    EXPECT_EQ(ctx.client_ctx.conn_close_state, KCPMUX_CONN_STATE_CLOSED);
    EXPECT_EQ(find_only_conn(
                  ctx.client_engine, ctx.server_addr.get()), nullptr);
    EXPECT_EQ(ctx.client_ctx.conn_close_count, 1);
    EXPECT_EQ(ctx.client_ctx.conn_close_reason, KCPMUX_CLOSE_REASON_NORMAL);

    ctx.teardown();
}

TEST(kcpmux_lifecycle, conn_close_zero_retries_sends_once_without_waiting) {
    DualEngineContext ctx;
    ctx.setup();

    kcpmux_conn_config_t config;
    kcpmux_conn_config_init(&config);
    config.close_retries = 0;
    kcpmux_conn_callbacks_t callbacks = create_conn_callbacks(&ctx.client_ctx);
    kcpmux_conn_t *conn = kcpmux_conn_connect(
        ctx.client_engine,
        ctx.server_addr.get(),
        &config,
        nullptr,
        &callbacks,
        &ctx.client_ctx);
    ASSERT_NE(conn, nullptr);
    ctx.deliver_all();
    ASSERT_EQ(kcpmux_conn_get_state(conn), KCPMUX_CONN_STATE_CONNECTED);

    ctx.client_ctx.sent_packets.clear();
    ASSERT_EQ(kcpmux_conn_close(conn), 0);

    ASSERT_EQ(ctx.client_ctx.sent_packets.size(), 1u);
    EXPECT_EQ(ctx.client_ctx.sent_packets[0][0], KCPMUX_MSG_CONN_CLOSE);
    EXPECT_EQ(find_only_conn(
                  ctx.client_engine, ctx.server_addr.get()), nullptr);
    EXPECT_EQ(ctx.client_ctx.conn_close_count, 1);
    EXPECT_EQ(ctx.client_ctx.conn_close_state, KCPMUX_CONN_STATE_CLOSED);

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
        ctx.client_engine,
        ctx.server_addr.get(),
        nullptr,
        nullptr,
        &client_callbacks,
        &ctx.client_ctx);
    ctx.deliver_all();

    // Set server-side callbacks to accept incoming streams
    kcpmux_conn_t *server_conn = find_only_conn(
        ctx.server_engine,
        ctx.client_addr.get());
    kcpmux_conn_callbacks_t server_callbacks = create_conn_callbacks(&ctx.server_ctx);
    kcpmux_conn_set_callbacks(server_conn, &server_callbacks, &ctx.server_ctx);

    kcpmux_stream_callbacks_t stream_callbacks = create_stream_callbacks(&ctx.client_ctx);
    kcpmux_stream_t *stream = kcpmux_stream_create(
        client_conn,
        &stream_config,
        &stream_callbacks,
        &ctx.client_ctx);
    EXPECT_EQ(kcpmux_stream_get_state(stream), KCPMUX_STREAM_STATE_OPEN);
    uint32_t stream_id = kcpmux_stream_id(stream);

    // Send bootstrap payload so close uses protocol close path (not immediate local close).
    uint8_t bootstrap = 0x7f;
    ASSERT_EQ(kcpmux_stream_send(stream, &bootstrap, 1, 1), 1);
    pump_kcp(ctx);
    pump_kcp(ctx);

    // Close stream
    ctx.client_ctx.stream_close_count = 0;
    kcpmux_stream_close(stream);
    EXPECT_EQ(kcpmux_stream_get_state(stream), KCPMUX_STREAM_STATE_CLOSING);

    // Don't deliver the CLOSE message
    ctx.client_ctx.sent_packets.clear();

    // Advance time past first timeout
    ctx.advance_time(101);
    kcpmux_engine_update(ctx.client_engine);

    // The only retry is the final send, so the stream closes immediately.
    ASSERT_EQ(ctx.client_ctx.sent_packets.size(), 1u);
    EXPECT_EQ(ctx.client_ctx.sent_packets[0][0], KCPMUX_MSG_STREAM_CLOSE);
    EXPECT_EQ(ctx.client_ctx.stream_close_state, KCPMUX_STREAM_STATE_CLOSED);
    EXPECT_EQ(kcpmux_conn_get_stream_by_id(client_conn, stream_id), nullptr);
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
        ctx.client_engine,
        ctx.server_addr.get(),
        nullptr,
        nullptr,
        &client_callbacks,
        &ctx.client_ctx);
    ctx.deliver_all();

    kcpmux_conn_t *server_conn = find_only_conn(
        ctx.server_engine,
        ctx.client_addr.get());
    kcpmux_conn_callbacks_t server_callbacks = create_conn_callbacks(&ctx.server_ctx);
    kcpmux_conn_set_callbacks(server_conn, &server_callbacks, &ctx.server_ctx);

    // Client creates stream
    kcpmux_stream_callbacks_t stream_callbacks = create_stream_callbacks(&ctx.client_ctx);
    kcpmux_stream_t *client_stream = kcpmux_stream_create(
        client_conn,
        nullptr,
        &stream_callbacks,
        &ctx.client_ctx);
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
        ctx.client_engine,
        ctx.server_addr.get(),
        nullptr,
        nullptr,
        &client_callbacks,
        &ctx.client_ctx);
    ctx.deliver_all();

    kcpmux_conn_t *server_conn = find_only_conn(
        ctx.server_engine,
        ctx.client_addr.get());
    kcpmux_conn_callbacks_t server_callbacks = create_conn_callbacks(&ctx.server_ctx);
    kcpmux_conn_set_callbacks(server_conn, &server_callbacks, &ctx.server_ctx);

    kcpmux_stream_callbacks_t client_stream_callbacks = create_stream_callbacks(&ctx.client_ctx);
    kcpmux_stream_t *client_stream = kcpmux_stream_create(
        client_conn,
        nullptr,
        &client_stream_callbacks,
        &ctx.client_ctx);

    // Send bootstrap payload to trigger server-side auto-create.
    uint8_t bootstrap = 0x7f;
    int ret = kcpmux_stream_send(client_stream, &bootstrap, 1, 1);
    ASSERT_EQ(ret, 1);
    pump_kcp(ctx);
    pump_kcp(ctx);

    uint32_t stream_id = kcpmux_stream_id(client_stream);
    kcpmux_stream_t *server_stream = kcpmux_conn_get_stream_by_id(server_conn, stream_id);
    kcpmux_stream_callbacks_t server_stream_callbacks = create_stream_callbacks(&ctx.server_ctx);
    kcpmux_stream_set_callbacks(server_stream, &server_stream_callbacks, &ctx.server_ctx);
    uint8_t received = 0;
    ASSERT_EQ(kcpmux_stream_recv(server_stream, &received, sizeof(received)), 1);

    // Client closes stream
    kcpmux_stream_close(client_stream);
    EXPECT_EQ(kcpmux_stream_get_state(client_stream), KCPMUX_STREAM_STATE_CLOSING);

    // Deliver STREAM_CLOSE to server
    ctx.deliver_client_to_server();
    EXPECT_EQ(kcpmux_stream_get_state(server_stream), KCPMUX_STREAM_STATE_CLOSING);
    kcpmux_engine_update(ctx.server_engine);
    EXPECT_EQ(ctx.server_ctx.stream_close_state, KCPMUX_STREAM_STATE_CLOSED);
    EXPECT_EQ(kcpmux_conn_get_stream_by_id(server_conn, stream_id), nullptr);

    // Deliver STREAM_CLOSE_ACK to client
    ctx.deliver_server_to_client();
    EXPECT_EQ(ctx.client_ctx.stream_close_state, KCPMUX_STREAM_STATE_CLOSED);
    EXPECT_EQ(kcpmux_conn_get_stream_by_id(client_conn, stream_id), nullptr);

    ctx.teardown();
}

TEST(kcpmux_lifecycle, stream_state_callbacks) {
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

    // Set server-side callbacks to accept incoming streams
    kcpmux_conn_t *server_conn = find_only_conn(
        ctx.server_engine,
        ctx.client_addr.get());
    kcpmux_conn_callbacks_t server_callbacks = create_conn_callbacks(&ctx.server_ctx);
    kcpmux_conn_set_callbacks(server_conn, &server_callbacks, &ctx.server_ctx);

    // Create stream with callbacks
    ctx.client_ctx.stream_state_changes.clear();
    kcpmux_stream_callbacks_t stream_callbacks = create_stream_callbacks(&ctx.client_ctx);
    kcpmux_stream_t *client_stream = kcpmux_stream_create(
        client_conn,
        nullptr,
        &stream_callbacks,
        &ctx.client_ctx);
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
        ctx.client_engine,
        ctx.server_addr.get(),
        nullptr,
        nullptr,
        &client_callbacks,
        &ctx.client_ctx);
    ctx.deliver_all();

    kcpmux_stream_callbacks_t stream_callbacks = create_stream_callbacks(&ctx.client_ctx);
    kcpmux_stream_t *stream = kcpmux_stream_create(
        client_conn,
        nullptr,
        &stream_callbacks,
        &ctx.client_ctx);
    ASSERT_NE(stream, nullptr);
    EXPECT_EQ(stream->is_initiator, 1);
    EXPECT_EQ(stream->stats.up_sent_bytes, 0u);

    ctx.client_ctx.stream_close_count = 0;
    ctx.client_ctx.sent_packets.clear();
    kcpmux_stream_close(stream);

    EXPECT_EQ(ctx.client_ctx.stream_close_state, KCPMUX_STREAM_STATE_CLOSED);
    EXPECT_EQ(client_conn->stream_count, 0u);
    EXPECT_EQ(ctx.client_ctx.stream_close_count, 1);
    EXPECT_EQ(ctx.client_ctx.sent_packets.size(), 0u);

    ctx.teardown();
}

TEST(kcpmux_lifecycle, stream_close_acceptor_without_payload_keeps_protocol_close) {
    DualEngineContext ctx;
    ctx.setup();

    kcpmux_conn_callbacks_t client_callbacks = create_conn_callbacks(&ctx.client_ctx);
    kcpmux_conn_t *client_conn = kcpmux_conn_connect(
        ctx.client_engine,
        ctx.server_addr.get(),
        nullptr,
        nullptr,
        &client_callbacks,
        &ctx.client_ctx);
    ctx.deliver_all();

    kcpmux_conn_t *server_conn = find_only_conn(
        ctx.server_engine,
        ctx.client_addr.get());
    kcpmux_conn_callbacks_t server_callbacks = create_conn_callbacks(&ctx.server_ctx);
    kcpmux_conn_set_callbacks(server_conn, &server_callbacks, &ctx.server_ctx);

    kcpmux_stream_callbacks_t client_stream_callbacks = create_stream_callbacks(&ctx.client_ctx);
    kcpmux_stream_t *client_stream = kcpmux_stream_create(
        client_conn,
        nullptr,
        &client_stream_callbacks,
        &ctx.client_ctx);
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
    kcpmux_conn_t
        *conn = kcpmux_conn_connect(engine, addr.get(), nullptr, nullptr, nullptr, nullptr);
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
        ctx.client_engine,
        ctx.server_addr.get(),
        nullptr,
        nullptr,
        &client_callbacks,
        &ctx.client_ctx);
    ctx.deliver_all();

    // Set server-side callbacks to accept incoming streams
    kcpmux_conn_t *server_conn = find_only_conn(
        ctx.server_engine,
        ctx.client_addr.get());
    kcpmux_conn_callbacks_t server_callbacks = create_conn_callbacks(&ctx.server_ctx);
    kcpmux_conn_set_callbacks(server_conn, &server_callbacks, &ctx.server_ctx);

    kcpmux_stream_callbacks_t stream_callbacks = create_stream_callbacks(&ctx.client_ctx);

    // Create multiple streams
    kcpmux_stream_t *stream1 = kcpmux_stream_create(
        client_conn,
        nullptr,
        &stream_callbacks,
        &ctx.client_ctx);
    ctx.deliver_all();

    kcpmux_stream_t *stream2 = kcpmux_stream_create(
        client_conn,
        nullptr,
        &stream_callbacks,
        &ctx.client_ctx);
    ctx.deliver_all();

    EXPECT_EQ(kcpmux_stream_get_state(stream1), KCPMUX_STREAM_STATE_OPEN);
    EXPECT_EQ(kcpmux_stream_get_state(stream2), KCPMUX_STREAM_STATE_OPEN);

    // Close connection
    kcpmux_conn_close(client_conn);
    ctx.deliver_all();

    // All streams are finalized before the connection callback returns.
    EXPECT_EQ(ctx.client_ctx.stream_close_count, 2);
    EXPECT_EQ(ctx.client_ctx.stream_close_state, KCPMUX_STREAM_STATE_CLOSED);
    EXPECT_EQ(ctx.client_ctx.conn_close_state, KCPMUX_CONN_STATE_CLOSED);
    EXPECT_EQ(ctx.client_engine->stats.stream_count, 0u);
    EXPECT_EQ(ctx.client_engine->conn_count, 0u);

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
