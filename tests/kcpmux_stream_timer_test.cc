#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <vector>

#include "kcpmux_test_utils.h"

extern "C" {
#include "kcpmux/kcpmux.h"
#include "kcpmux_conn.h"
#include "kcpmux_engine.h"
#include "kcpmux_protocol.h"
#include "kcpmux_stream.h"
}

namespace {

using namespace kcpmux_test;

struct FakeKcpContext;

struct FakeKcp {
    FakeKcpContext *context;
    void *user;
    int update_calls;
    int check_calls;
    int64_t current_ms;
    int (*output)(const char *, int, void *, void *);
};

struct FakeKcpContext {
    int64_t now_ms = 1000;
    int64_t check_deadline_ms = 1000000;
    int input_result = 0;
    int recv_result = -1;
    int peek_result = 0;
    int total_update_calls = 0;
    int total_check_calls = 0;
    int total_input_calls = 0;
    uint64_t last_timer_ms = UINT64_MAX;
    int release_calls = 0;
    int stream_create_result = 0;
    int stream_create_calls = 0;
    int stream_state_calls = 0;
    int stream_read_calls = 0;
    int stream_write_calls = 0;
    int stream_close_calls = 0;
    bool external_wrapper_alive = true;
    bool release_saw_released_wrapper = false;
    kcpmux_addr_t peer_addr{};
    std::vector<char> lifecycle_events;
    std::vector<std::vector<uint8_t>> sent_packets;
    std::vector<FakeKcp *> instances;
};

static void *fake_create(uint32_t conv, void *kcp_user, void *engine_user) {
    (void)conv;
    (void)kcp_user;
    auto *context = static_cast<FakeKcpContext *>(engine_user);
    auto *kcp = static_cast<FakeKcp *>(calloc(1, sizeof(FakeKcp)));
    if (kcp) {
        kcp->context = context;
        kcp->user = kcp_user;
        context->instances.push_back(kcp);
    }
    return kcp;
}

static void fake_release(void *kcp) {
    auto *fake = static_cast<FakeKcp *>(kcp);
    FakeKcpContext *context = fake->context;
    context->release_saw_released_wrapper =
        context->release_saw_released_wrapper || !context->external_wrapper_alive;
    context->lifecycle_events.push_back('K');
    context->release_calls++;
    free(kcp);
}

static void fake_setmss(void *kcp, int mss) {
    (void)kcp;
    (void)mss;
}

static void fake_setoutput(
    void *kcp, int (*output)(const char *, int, void *, void *)) {
    static_cast<FakeKcp *>(kcp)->output = output;
}

static int fake_send(void *kcp, const char *buf, int len) {
    (void)kcp;
    (void)buf;
    (void)len;
    return 0;
}

static int fake_input(void *kcp, const char *data, long size) {
    (void)data;
    (void)size;
    FakeKcpContext *context = static_cast<FakeKcp *>(kcp)->context;
    context->total_input_calls++;
    return context->input_result;
}

static int fake_recv(void *kcp, char *buf, int len) {
    auto *fake = static_cast<FakeKcp *>(kcp);
    int result = fake->context->recv_result;
    if (result > 0) {
        if (result > len) return -3;
        memset(buf, 0x5a, (size_t)result);
        fake->context->peek_result = 0;
    }
    return result;
}

static int fake_peeksize(void *kcp) {
    return static_cast<FakeKcp *>(kcp)->context->peek_result;
}

static int fake_waitsnd(void *kcp) {
    (void)kcp;
    return 0;
}

static void fake_update(void *kcp, int64_t current) {
    auto *fake = static_cast<FakeKcp *>(kcp);
    fake->current_ms = current;
    fake->update_calls++;
    fake->context->total_update_calls++;
}

static int64_t fake_check(void *kcp, int64_t current) {
    auto *fake = static_cast<FakeKcp *>(kcp);
    (void)current;
    fake->check_calls++;
    fake->context->total_check_calls++;
    return fake->context->check_deadline_ms;
}

static int64_t fake_current(void *kcp) {
    return static_cast<FakeKcp *>(kcp)->current_ms;
}

static void fake_current_update(void *kcp, int64_t current) {
    static_cast<FakeKcp *>(kcp)->current_ms = current;
}

static kcpmux_kcp_ops_t fake_ops = {
    fake_create,
    fake_release,
    fake_setmss,
    fake_setoutput,
    fake_send,
    fake_input,
    fake_recv,
    fake_peeksize,
    fake_waitsnd,
    fake_update,
    fake_check,
    fake_current,
    fake_current_update,
};

static void fake_set_timer(uint64_t wake_after_ms, void *user_data) {
    static_cast<FakeKcpContext *>(user_data)->last_timer_ms = wake_after_ms;
}

static int fake_write_socket(const uint8_t *buf, unsigned size,
                                const kcpmux_addr_t *addr, void *user_data) {
    (void)buf;
    (void)size;
    (void)addr;
    auto *context = static_cast<FakeKcpContext *>(user_data);
    context->sent_packets.emplace_back(buf, buf + size);
    return 1;
}

static void fake_stream_state_notify(kcpmux_stream_t *, uint8_t, uint8_t,
                                     void *user_data) {
    static_cast<FakeKcpContext *>(user_data)->stream_state_calls++;
}

static void fake_stream_read_notify(kcpmux_stream_t *, void *user_data) {
    static_cast<FakeKcpContext *>(user_data)->stream_read_calls++;
}

static void fake_stream_write_notify(kcpmux_stream_t *, void *user_data) {
    static_cast<FakeKcpContext *>(user_data)->stream_write_calls++;
}

static void fake_stream_close_notify(kcpmux_stream_t *, int, void *user_data) {
    auto *context = static_cast<FakeKcpContext *>(user_data);
    context->lifecycle_events.push_back('S');
    context->stream_close_calls++;
}

static void fake_conn_close_notify(kcpmux_conn_t *, int, void *user_data) {
    auto *context = static_cast<FakeKcpContext *>(user_data);
    context->lifecycle_events.push_back('C');
    context->external_wrapper_alive = false;
}

static int fake_stream_create_notify(kcpmux_stream_t *stream,
                                     void *user_data) {
    auto *context = static_cast<FakeKcpContext *>(user_data);
    context->stream_create_calls++;
    kcpmux_stream_callbacks_t callbacks{};
    callbacks.stream_state_changed = fake_stream_state_notify;
    callbacks.stream_read_notify = fake_stream_read_notify;
    callbacks.stream_write_notify = fake_stream_write_notify;
    callbacks.stream_close_notify = fake_stream_close_notify;
    kcpmux_stream_set_callbacks(stream, &callbacks, user_data);
    return context->stream_create_result;
}

static int64_t fake_now(void *user_data) {
    return static_cast<FakeKcpContext *>(user_data)->now_ms;
}

class kcpmux_stream_timer : public ::testing::Test {
protected:
    void SetUp() override {
        kcpmux_engine_callbacks_t callbacks{};
        callbacks.set_timer = fake_set_timer;
        callbacks.write_socket = fake_write_socket;
        callbacks.monotonic_time_ms = fake_now;

        kcpmux_conn_config_t config;
        kcpmux_conn_config_init(&config);
        config.keepalive_interval_ms = 2000000;
        config.keepalive_timeout_ms = 2000000;
        config.idle_timeout_ms = 0;

        engine = kcpmux_engine_create(nullptr, &config, nullptr, &callbacks,
                                     &context, &fake_ops);
        ASSERT_NE(engine, nullptr);

        addr_bytes[0] = 127;
        addr_bytes[1] = 0;
        addr_bytes[2] = 0;
        addr_bytes[3] = 1;
        addr_bytes[4] = 0x30;
        addr_bytes[5] = 0x39;
        kcpmux_addr_t addr{addr_bytes, sizeof(addr_bytes)};
        conn = kcpmux_conn_new(engine, &addr, &config, 1);
        ASSERT_NE(conn, nullptr);
        kcpmux_engine_add_conn(engine, conn);
        kcpmux_conn_set_state(conn, KCPMUX_CONN_STATE_CONNECTED);
        kcpmux_conn_callbacks_t conn_callbacks{};
        conn_callbacks.stream_create_notify = fake_stream_create_notify;
        kcpmux_conn_set_callbacks(conn, &conn_callbacks, &context);
        context.peer_addr = addr;
    }

    void TearDown() override {
        if (engine) {
            kcpmux_engine_destroy(engine);
        }
    }

    kcpmux_stream_t *NewStream(uint32_t id) {
        kcpmux_stream_t *stream = kcpmux_stream_new(conn, id, nullptr, 1);
        if (stream) {
            kcpmux_conn_add_stream(conn, stream);
        }
        return stream;
    }

    void ResetKcpCounts() {
        context.total_update_calls = 0;
        context.total_check_calls = 0;
        for (FakeKcp *instance : context.instances) {
            instance->update_calls = 0;
            instance->check_calls = 0;
        }
    }

    FakeKcpContext context;
    kcpmux_engine_t *engine = nullptr;
    kcpmux_conn_t *conn = nullptr;
    uint8_t addr_bytes[6]{};
};

TEST_F(kcpmux_stream_timer, only_due_stream_is_touched) {
    std::vector<kcpmux_stream_t *> streams;
    for (uint32_t id = 1; id <= 1000; ++id) {
        kcpmux_stream_t *stream = NewStream(id);
        ASSERT_NE(stream, nullptr);
        streams.push_back(stream);
    }

    ASSERT_EQ(kcpmux_engine_schedule_timer_node(
                  engine, &streams.front()->timer_node,
                  context.now_ms, context.now_ms), KCPMUX_ERR_OK);
    ResetKcpCounts();
    uint64_t dispatch_count = engine->timer_dispatch_count;

    kcpmux_engine_update(engine);

    EXPECT_EQ(engine->timer_dispatch_count - dispatch_count, 1u);
    EXPECT_EQ(context.total_update_calls, 1);
    EXPECT_EQ(context.total_check_calls, 1);
    EXPECT_EQ(static_cast<FakeKcp *>(streams.front()->kcp)->update_calls, 1);
    EXPECT_EQ(static_cast<FakeKcp *>(streams.back()->kcp)->update_calls, 0);
}

TEST_F(kcpmux_stream_timer, passive_stream_accept_processes_first_payload) {
    const uint8_t kcp_payload[] = {0xaa};
    auto payload = build_stream_payload(
        2, kcp_payload, sizeof(kcp_payload), conn->generation_id);

    ASSERT_EQ(kcpmux_engine_input(
                  engine, payload.data(), payload.size(), &context.peer_addr), 0);

    EXPECT_EQ(context.stream_create_calls, 1);
    EXPECT_EQ(context.total_input_calls, 1);
    kcpmux_stream_t *stream = kcpmux_conn_get_stream_by_id(conn, 2);
    ASSERT_NE(stream, nullptr);
    EXPECT_EQ(stream->state, KCPMUX_STREAM_STATE_OPEN);
    EXPECT_EQ(stream->stats.rx_packets, 1u);
    EXPECT_EQ(stream->stats.rx_bytes, 1u);
}

TEST_F(kcpmux_stream_timer, passive_stream_reject_skips_payload_and_releases_on_ack) {
    context.stream_create_result = 123;
    const uint8_t kcp_payload[] = {0xaa};
    auto payload = build_stream_payload(
        2, kcp_payload, sizeof(kcp_payload), conn->generation_id);

    ASSERT_EQ(kcpmux_engine_input(
                  engine, payload.data(), payload.size(), &context.peer_addr), 0);

    EXPECT_EQ(context.stream_create_calls, 1);
    EXPECT_EQ(context.total_input_calls, 0);
    EXPECT_EQ(context.stream_state_calls, 0);
    EXPECT_EQ(context.stream_read_calls, 0);
    EXPECT_EQ(context.stream_write_calls, 0);
    EXPECT_EQ(context.stream_close_calls, 0);
    kcpmux_stream_t *stream = kcpmux_conn_get_stream_by_id(conn, 2);
    ASSERT_NE(stream, nullptr);
    EXPECT_EQ(stream->state, KCPMUX_STREAM_STATE_CLOSING);
    EXPECT_EQ(stream->close_reason, KCPMUX_CLOSE_REASON_REJECTED);
    EXPECT_EQ(stream->user_data, nullptr);
    EXPECT_EQ(stream->callbacks.stream_state_changed, nullptr);
    EXPECT_EQ(stream->callbacks.stream_read_notify, nullptr);
    EXPECT_EQ(stream->callbacks.stream_write_notify, nullptr);
    EXPECT_EQ(stream->callbacks.stream_close_notify, nullptr);
    ASSERT_EQ(context.sent_packets.size(), 1u);
    EXPECT_EQ(context.sent_packets.back()[0], KCPMUX_MSG_STREAM_CLOSE);
    EXPECT_EQ(context.sent_packets.back()[8], KCPMUX_CLOSE_REASON_REJECTED);

    auto ack = build_stream_close_ack(
        2, KCPMUX_CLOSE_REASON_REJECTED, conn->generation_id);
    ASSERT_EQ(kcpmux_engine_input(
                  engine, ack.data(), ack.size(), &context.peer_addr), 0);
    EXPECT_EQ(kcpmux_conn_get_stream_by_id(conn, 2), nullptr);
    EXPECT_EQ(context.release_calls, 1);
    EXPECT_EQ(context.stream_close_calls, 0);
}

TEST_F(kcpmux_stream_timer, passive_stream_reject_retransmits_and_times_out) {
    context.stream_create_result = -1;
    kcpmux_stream_config_t config;
    kcpmux_stream_config_init(&config);
    config.ctrl_timeout_ms = 10;
    config.close_retries = 1;
    engine->default_stream_config = config;
    const uint8_t kcp_payload[] = {0xaa};
    auto payload = build_stream_payload(
        2, kcp_payload, sizeof(kcp_payload), conn->generation_id);

    ASSERT_EQ(kcpmux_engine_input(
                  engine, payload.data(), payload.size(), &context.peer_addr), 0);
    ASSERT_EQ(context.sent_packets.size(), 1u);

    context.now_ms += 10;
    kcpmux_engine_update(engine);
    ASSERT_EQ(context.sent_packets.size(), 2u);
    EXPECT_EQ(context.sent_packets.back()[0], KCPMUX_MSG_STREAM_CLOSE);
    EXPECT_EQ(context.sent_packets.back()[8], KCPMUX_CLOSE_REASON_REJECTED);
    EXPECT_EQ(kcpmux_conn_get_stream_by_id(conn, 2), nullptr);
    EXPECT_EQ(context.release_calls, 1);
    EXPECT_EQ(context.stream_close_calls, 0);
}

TEST_F(kcpmux_stream_timer, close_zero_retries_sends_once_without_waiting) {
    kcpmux_stream_config_t config;
    kcpmux_stream_config_init(&config);
    config.close_retries = 0;
    kcpmux_stream_t *stream = kcpmux_stream_new(conn, 1, &config, 1);
    ASSERT_NE(stream, nullptr);
    kcpmux_conn_add_stream(conn, stream);
    uint8_t byte = 0x42;
    ASSERT_EQ(kcpmux_stream_send(stream, &byte, 1, 0), 1);
    context.sent_packets.clear();

    ASSERT_EQ(kcpmux_stream_close(stream), 0);

    ASSERT_EQ(context.sent_packets.size(), 1u);
    EXPECT_EQ(context.sent_packets[0][0], KCPMUX_MSG_STREAM_CLOSE);
    EXPECT_EQ(kcpmux_conn_get_stream_by_id(conn, 1), nullptr);
    EXPECT_EQ(context.release_calls, 1);
}

TEST_F(kcpmux_stream_timer, close_retries_wait_only_between_sends) {
    kcpmux_stream_config_t config;
    kcpmux_stream_config_init(&config);
    config.ctrl_timeout_ms = 10;
    config.close_retries = 2;
    kcpmux_stream_t *stream = kcpmux_stream_new(conn, 1, &config, 1);
    ASSERT_NE(stream, nullptr);
    kcpmux_conn_add_stream(conn, stream);
    uint8_t byte = 0x42;
    ASSERT_EQ(kcpmux_stream_send(stream, &byte, 1, 0), 1);
    context.sent_packets.clear();

    ASSERT_EQ(kcpmux_stream_close(stream), 0);
    ASSERT_EQ(context.sent_packets.size(), 1u);
    ASSERT_NE(kcpmux_conn_get_stream_by_id(conn, 1), nullptr);

    context.now_ms += 10;
    kcpmux_engine_update(engine);
    ASSERT_EQ(context.sent_packets.size(), 2u);
    ASSERT_NE(kcpmux_conn_get_stream_by_id(conn, 1), nullptr);

    context.now_ms += 10;
    kcpmux_engine_update(engine);
    ASSERT_EQ(context.sent_packets.size(), 3u);
    EXPECT_EQ(kcpmux_conn_get_stream_by_id(conn, 1), nullptr);
    EXPECT_EQ(context.release_calls, 1);
}

TEST_F(kcpmux_stream_timer, active_older_peer_stream_continues_after_high_water_advances) {
    const uint8_t kcp_payload[] = {0xaa};
    auto first = build_stream_payload(
        100, kcp_payload, sizeof(kcp_payload), conn->generation_id);
    auto second = build_stream_payload(
        102, kcp_payload, sizeof(kcp_payload), conn->generation_id);

    ASSERT_EQ(kcpmux_engine_input(
                  engine, first.data(), first.size(), &context.peer_addr), 0);
    ASSERT_EQ(kcpmux_engine_input(
                  engine, second.data(), second.size(), &context.peer_addr), 0);
    ASSERT_EQ(conn->latest_peer_stream_id, 102u);
    ASSERT_EQ(kcpmux_engine_input(
                  engine, first.data(), first.size(), &context.peer_addr), 0);

    EXPECT_EQ(context.stream_create_calls, 2);
    EXPECT_EQ(context.total_input_calls, 3);
    EXPECT_NE(kcpmux_conn_get_stream_by_id(conn, 100), nullptr);
    EXPECT_NE(kcpmux_conn_get_stream_by_id(conn, 102), nullptr);
}

TEST_F(kcpmux_stream_timer, duplicate_backward_and_half_ring_peer_ids_are_old) {
    const uint8_t kcp_payload[] = {0xaa};
    auto first = build_stream_payload(
        100, kcp_payload, sizeof(kcp_payload), conn->generation_id);
    ASSERT_EQ(kcpmux_engine_input(
                  engine, first.data(), first.size(), &context.peer_addr), 0);
    kcpmux_stream_t *stream = kcpmux_conn_get_stream_by_id(conn, 100);
    ASSERT_NE(stream, nullptr);
    kcpmux_engine_operation_enter(engine);
    kcpmux_stream_close_internal(stream, KCPMUX_CLOSE_REASON_NORMAL);
    kcpmux_engine_operation_leave(engine);

    auto backward = build_stream_payload(
        98, kcp_payload, sizeof(kcp_payload), conn->generation_id);
    auto half_ring = build_stream_payload(
        100U + 0x80000000U, kcp_payload, sizeof(kcp_payload),
        conn->generation_id);
    EXPECT_EQ(kcpmux_engine_input(
                  engine, first.data(), first.size(), &context.peer_addr),
              -KCPMUX_ERR_NOT_FOUND);
    EXPECT_EQ(kcpmux_engine_input(
                  engine, backward.data(), backward.size(), &context.peer_addr),
              -KCPMUX_ERR_NOT_FOUND);
    EXPECT_EQ(kcpmux_engine_input(
                  engine, half_ring.data(), half_ring.size(),
                  &context.peer_addr),
              -KCPMUX_ERR_NOT_FOUND);
    EXPECT_EQ(context.stream_create_calls, 1);
    EXPECT_EQ(context.total_input_calls, 1);
}

TEST_F(kcpmux_stream_timer, out_of_order_unknown_peer_stream_is_rejected) {
    const uint8_t kcp_payload[] = {0xaa};
    auto newer = build_stream_payload(
        102, kcp_payload, sizeof(kcp_payload), conn->generation_id);
    auto delayed = build_stream_payload(
        100, kcp_payload, sizeof(kcp_payload), conn->generation_id);

    ASSERT_EQ(kcpmux_engine_input(
                  engine, newer.data(), newer.size(), &context.peer_addr), 0);
    EXPECT_EQ(kcpmux_engine_input(
                  engine, delayed.data(), delayed.size(), &context.peer_addr),
              -KCPMUX_ERR_NOT_FOUND);
    EXPECT_EQ(context.stream_create_calls, 1);
    EXPECT_EQ(kcpmux_conn_get_stream_by_id(conn, 100), nullptr);
}

TEST_F(kcpmux_stream_timer, peer_stream_serial_wraps_on_both_parities) {
    const uint8_t kcp_payload[] = {0xaa};
    auto even_last = build_stream_payload(
        0xFFFFFFFEU, kcp_payload, sizeof(kcp_payload), conn->generation_id);
    auto even_wrapped = build_stream_payload(
        2, kcp_payload, sizeof(kcp_payload), conn->generation_id);
    ASSERT_EQ(kcpmux_engine_input(
                  engine, even_last.data(), even_last.size(),
                  &context.peer_addr), 0);
    ASSERT_EQ(kcpmux_engine_input(
                  engine, even_wrapped.data(), even_wrapped.size(),
                  &context.peer_addr), 0);
    EXPECT_EQ(conn->latest_peer_stream_id, 2u);

    kcpmux_conn_t *acceptor_conn = conn;
    acceptor_conn->is_initiator = 0;
    acceptor_conn->peer_stream_id_initialized = 0;
    auto odd_last = build_stream_payload(
        0xFFFFFFFFU, kcp_payload, sizeof(kcp_payload),
        acceptor_conn->generation_id);
    auto odd_wrapped = build_stream_payload(
        1, kcp_payload, sizeof(kcp_payload), acceptor_conn->generation_id);
    ASSERT_EQ(kcpmux_engine_input(
                  engine, odd_last.data(), odd_last.size(),
                  &context.peer_addr), 0);
    ASSERT_EQ(kcpmux_engine_input(
                  engine, odd_wrapped.data(), odd_wrapped.size(),
                  &context.peer_addr), 0);
    EXPECT_EQ(acceptor_conn->latest_peer_stream_id, 1u);
}

TEST_F(kcpmux_stream_timer, rejected_and_invalid_first_payloads_consume_peer_id) {
    const uint8_t kcp_payload[] = {0xaa};
    context.stream_create_result = 1;
    auto rejected = build_stream_payload(
        2, kcp_payload, sizeof(kcp_payload), conn->generation_id);
    ASSERT_EQ(kcpmux_engine_input(
                  engine, rejected.data(), rejected.size(),
                  &context.peer_addr), 0);
    EXPECT_EQ(conn->latest_peer_stream_id, 2u);
    EXPECT_EQ(context.stream_create_calls, 1);

    context.stream_create_result = 0;
    context.input_result = -7;
    auto invalid = build_stream_payload(
        4, kcp_payload, sizeof(kcp_payload), conn->generation_id);
    EXPECT_EQ(kcpmux_engine_input(
                  engine, invalid.data(), invalid.size(), &context.peer_addr),
              KCPMUX_ERR_KCPRET(-7));
    EXPECT_EQ(conn->latest_peer_stream_id, 4u);
    EXPECT_EQ(kcpmux_conn_get_stream_by_id(conn, 4), nullptr);
    EXPECT_EQ(kcpmux_engine_input(
                  engine, invalid.data(), invalid.size(), &context.peer_addr),
              -KCPMUX_ERR_NOT_FOUND);
    EXPECT_EQ(context.stream_create_calls, 2);
}

TEST_F(kcpmux_stream_timer, local_allocator_wraps_skips_zero_and_active_ids) {
    conn->is_initiator = 1;
    kcpmux_stream_t *odd_conflict = NewStream(1);
    ASSERT_NE(odd_conflict, nullptr);
    conn->next_stream_id = 0xFFFFFFFFU;
    EXPECT_EQ(kcpmux_conn_alloc_stream_id(conn), 0xFFFFFFFFU);
    EXPECT_EQ(kcpmux_conn_alloc_stream_id(conn), 3u);

    conn->is_initiator = 0;
    kcpmux_stream_t *even_conflict = NewStream(2);
    ASSERT_NE(even_conflict, nullptr);
    conn->next_stream_id = 0xFFFFFFFEU;
    EXPECT_EQ(kcpmux_conn_alloc_stream_id(conn), 0xFFFFFFFEU);
    EXPECT_EQ(kcpmux_conn_alloc_stream_id(conn), 4u);
}

TEST_F(kcpmux_stream_timer, local_stream_ids_start_random_nonzero_and_advance) {
    uint32_t first_candidate = conn->next_stream_id;
    EXPECT_NE(first_candidate, 0u);
    EXPECT_EQ(first_candidate & 1U, 1u);
    kcpmux_stream_t *first = kcpmux_stream_create(conn, nullptr, nullptr, nullptr);
    kcpmux_stream_t *second = kcpmux_stream_create(conn, nullptr, nullptr, nullptr);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(first->stream_id, first_candidate);
    uint32_t expected_second = first_candidate + 2U;
    if (expected_second == 0) expected_second = 1;
    EXPECT_EQ(second->stream_id, expected_second);
    EXPECT_EQ(second->stream_id & 1U, 1u);
}

TEST_F(kcpmux_stream_timer, terminal_stream_is_released_once) {
    kcpmux_stream_t *stream = NewStream(1);
    ASSERT_NE(stream, nullptr);

    kcpmux_engine_operation_enter(engine);
    kcpmux_stream_close_internal(stream, KCPMUX_CLOSE_REASON_NORMAL);
    ASSERT_FALSE(list_empty(&engine->pending_release_list));
    kcpmux_engine_operation_leave(engine);

    EXPECT_EQ(context.release_calls, 1);
    EXPECT_EQ(conn->stream_count, 0u);
    EXPECT_EQ(engine->timer_node_count, 1u);
    EXPECT_TRUE(list_empty(&engine->pending_release_list));
}

TEST_F(kcpmux_stream_timer, kcp_is_released_before_terminal_notifications) {
    kcpmux_stream_t *stream = NewStream(1);
    ASSERT_NE(stream, nullptr);

    kcpmux_stream_callbacks_t stream_callbacks{};
    stream_callbacks.stream_close_notify = fake_stream_close_notify;
    kcpmux_stream_set_callbacks(stream, &stream_callbacks, &context);
    kcpmux_conn_callbacks_t conn_callbacks{};
    conn_callbacks.conn_close_notify = fake_conn_close_notify;
    kcpmux_conn_set_callbacks(conn, &conn_callbacks, &context);

    kcpmux_engine_operation_enter(engine);
    kcpmux_conn_close_internal(conn, KCPMUX_CLOSE_REASON_NORMAL);

    EXPECT_EQ(context.release_calls, 1);
    EXPECT_FALSE(context.release_saw_released_wrapper);
    EXPECT_EQ(context.lifecycle_events, (std::vector<char>{'K', 'S', 'C'}));
    kcpmux_engine_operation_leave(engine);
}

TEST_F(kcpmux_stream_timer, check_now_and_past_continue_on_next_update) {
    kcpmux_stream_t *stream = NewStream(1);
    ASSERT_NE(stream, nullptr);

    context.check_deadline_ms = context.now_ms;
    ASSERT_EQ(kcpmux_engine_schedule_timer_node(
                  engine, &stream->timer_node,
                  context.now_ms, context.now_ms), KCPMUX_ERR_OK);
    ResetKcpCounts();

    kcpmux_engine_update(engine);
    EXPECT_EQ(context.total_update_calls, 1);
    EXPECT_EQ(stream->timer_node.state, KCPMUX_TIMER_HEAP);
    EXPECT_EQ(stream->timer_node.deadline_ms, context.now_ms);
    EXPECT_EQ(context.last_timer_ms, 0u);

    kcpmux_engine_update(engine);
    EXPECT_EQ(context.total_update_calls, 2);

    context.check_deadline_ms = context.now_ms - 500;
    kcpmux_engine_update(engine);
    EXPECT_EQ(context.total_update_calls, 3);
    EXPECT_EQ(stream->timer_node.deadline_ms, context.now_ms);
    EXPECT_EQ(context.last_timer_ms, 0u);
}

TEST_F(kcpmux_stream_timer, check_far_deadline_is_preserved) {
    kcpmux_stream_t *stream = NewStream(1);
    ASSERT_NE(stream, nullptr);

    context.check_deadline_ms = 9000000;
    ASSERT_EQ(kcpmux_engine_schedule_timer_node(
                  engine, &stream->timer_node,
                  context.now_ms, context.now_ms), KCPMUX_ERR_OK);
    kcpmux_engine_update(engine);

    EXPECT_EQ(stream->timer_node.deadline_ms, 9000000);
}

TEST_F(kcpmux_stream_timer, closing_deadline_is_anchored_and_configurable) {
    kcpmux_stream_config_t config;
    kcpmux_stream_config_init(&config);
    config.ctrl_timeout_ms = 100;
    config.close_retries = 1;
    kcpmux_stream_t *stream = kcpmux_stream_new(conn, 1, &config, 1);
    ASSERT_NE(stream, nullptr);
    kcpmux_conn_add_stream(conn, stream);

    stream->last_ctrl_ts = context.now_ms;
    stream->close_reason = KCPMUX_CLOSE_REASON_NORMAL;
    kcpmux_stream_set_state(stream, KCPMUX_STREAM_STATE_CLOSING);
    EXPECT_EQ(stream->timer_node.deadline_ms, 1100);

    context.now_ms = 1050;
    kcpmux_engine_update(engine);
    EXPECT_EQ(stream->timer_node.deadline_ms, 1100);

    config.ctrl_timeout_ms = 20;
    kcpmux_stream_set_config(stream, &config);
    EXPECT_EQ(stream->timer_node.deadline_ms, context.now_ms);

    kcpmux_engine_update(engine);
    EXPECT_EQ(kcpmux_conn_get_stream_by_id(conn, 1), nullptr);
    EXPECT_EQ(context.release_calls, 1);
    ASSERT_EQ(context.sent_packets.size(), 1u);
    EXPECT_EQ(context.sent_packets[0][0], KCPMUX_MSG_STREAM_CLOSE);
}

TEST_F(kcpmux_stream_timer, recv_preserves_readable_state_for_small_buffer_and_errors) {
    kcpmux_stream_t *stream = NewStream(1);
    ASSERT_NE(stream, nullptr);
    stream->read_blocked = 0;
    uint64_t block_count = stream->stats.read_block_count;
    uint8_t buffer[8]{};
    context.peek_result = 8;
    context.recv_result = 8;

    EXPECT_EQ(kcpmux_stream_peek_size(stream), 8);
    EXPECT_EQ(kcpmux_stream_recv(stream, buffer, 4),
              KCPMUX_ERR_BUFFER_TOO_SMALL);
    EXPECT_EQ(stream->read_blocked, 0);
    EXPECT_EQ(stream->stats.read_block_count, block_count);
    EXPECT_EQ(kcpmux_stream_peek_size(stream), 8);

    EXPECT_EQ(kcpmux_stream_recv(stream, buffer, sizeof(buffer)), 8);
    EXPECT_EQ(buffer[0], 0x5a);
    EXPECT_EQ(stream->stats.up_recv_bytes, 8u);
    EXPECT_EQ(stream->read_blocked, 0);

    context.peek_result = -1;
    EXPECT_EQ(kcpmux_stream_recv(stream, buffer, sizeof(buffer)), 0);
    EXPECT_EQ(stream->read_blocked, 1);
    EXPECT_EQ(stream->stats.read_block_count, block_count + 1);

    stream->read_blocked = 0;
    context.peek_result = 4;
    context.recv_result = -9;
    EXPECT_EQ(kcpmux_stream_recv(stream, buffer, sizeof(buffer)),
              KCPMUX_ERR_KCPRET(-9));
    EXPECT_EQ(stream->read_blocked, 0);
    EXPECT_EQ(stream->stats.read_block_count, block_count + 1);
}

TEST_F(kcpmux_stream_timer, peek_size_reports_state_errors) {
    kcpmux_stream_t *stream = NewStream(1);
    ASSERT_NE(stream, nullptr);

    EXPECT_EQ(kcpmux_stream_peek_size(nullptr), -KCPMUX_ERR_INVALID_PARAM);
    kcpmux_stream_set_state(stream, KCPMUX_STREAM_STATE_CLOSING);
    EXPECT_EQ(kcpmux_stream_peek_size(stream), KCPMUX_ERR_STATE);
    uint8_t buffer = 0;
    EXPECT_EQ(kcpmux_stream_recv(stream, &buffer, sizeof(buffer)),
              KCPMUX_ERR_STATE);
    stream->internal_closed = 1;
    EXPECT_EQ(kcpmux_stream_peek_size(stream), KCPMUX_ERR_CLOSED);
    EXPECT_EQ(kcpmux_stream_recv(stream, &buffer, sizeof(buffer)),
              KCPMUX_ERR_CLOSED);
    stream->internal_closed = 0;
}

TEST_F(kcpmux_stream_timer, same_parity_stream_ids_use_both_bucket_parities) {
    std::vector<kcpmux_stream_t *> streams;
    for (uint32_t id = 1; id < 33; id += 2) {
        kcpmux_stream_t *stream = NewStream(id);
        ASSERT_NE(stream, nullptr);
        streams.push_back(stream);
        EXPECT_EQ(kcpmux_conn_get_stream_by_id(conn, id), stream);
    }

    EXPECT_EQ(conn->stream_map->size, 32);
    bool has_even_bucket = false;
    bool has_odd_bucket = false;
    for (int i = 0; i < conn->stream_map->size; i++) {
        if (!list_empty(&conn->stream_map->hashtable[i])) {
            has_even_bucket |= (i % 2) == 0;
            has_odd_bucket |= (i % 2) != 0;
        }
    }
    EXPECT_TRUE(has_even_bucket);
    EXPECT_TRUE(has_odd_bucket);
    EXPECT_EQ(conn->stream_count, streams.size());
    EXPECT_EQ(engine->stats.stream_count, streams.size());

    kcpmux_engine_operation_enter(engine);
    for (kcpmux_stream_t *stream : streams) {
        kcpmux_stream_close_internal(stream, KCPMUX_CLOSE_REASON_NORMAL);
    }
    kcpmux_engine_operation_leave(engine);
    EXPECT_EQ(conn->stream_count, 0u);
    EXPECT_EQ(engine->stats.stream_count, 0u);
}

TEST_F(kcpmux_stream_timer, send_flush_modes_have_distinct_scheduling) {
    kcpmux_stream_t *stream = NewStream(1);
    ASSERT_NE(stream, nullptr);
    uint8_t byte = 0x42;

    ResetKcpCounts();
    ASSERT_EQ(kcpmux_stream_send(stream, &byte, 1, 0), 1);
    EXPECT_EQ(context.total_update_calls, 0);
    EXPECT_EQ(stream->timer_node.deadline_ms, context.now_ms);
    EXPECT_EQ(context.last_timer_ms, 0u);

    context.check_deadline_ms = 5000;
    ASSERT_EQ(kcpmux_stream_send(stream, &byte, 1, 1), 1);
    EXPECT_EQ(context.total_update_calls, 1);
    EXPECT_EQ(context.total_check_calls, 1);
    EXPECT_EQ(stream->timer_node.deadline_ms, 5000);
}

TEST_F(kcpmux_stream_timer, input_error_still_schedules_immediate_update) {
    kcpmux_stream_t *stream = NewStream(1);
    ASSERT_NE(stream, nullptr);
    uint8_t byte = 0x42;
    context.input_result = -7;

    int ret = kcpmux_stream_handle_payload(
        stream, &byte, 1, context.now_ms);

    EXPECT_LT(ret, 0);
    EXPECT_EQ(stream->timer_node.deadline_ms, context.now_ms);
    EXPECT_EQ(context.last_timer_ms, 0u);
}

}  // namespace
