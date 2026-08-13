#include "kcpmux_test_utils.h"

using namespace kcpmux_test;

namespace {

struct RejectingConnectContext : TestContext {
    int rejected_conn_close_calls = 0;
};

void rejected_conn_close_notify(kcpmux_conn_t *, int, void *user_data)
{
    auto *context = static_cast<RejectingConnectContext *>(user_data);
    context->rejected_conn_close_calls++;
}

int reject_conn_after_installing_callbacks(
    kcpmux_conn_t *conn,
    const kcpmux_proto_ext_t *,
    kcpmux_proto_ext_t *,
    void *user_data)
{
    kcpmux_conn_callbacks_t callbacks{};
    callbacks.conn_close_notify = rejected_conn_close_notify;
    kcpmux_conn_set_callbacks(conn, &callbacks, user_data);
    return KCPMUX_ACK_RESULT_ERROR;
}

// ============================================================================
// Connection Protocol Message Format Tests
// ============================================================================

TEST(kcpmux_protocol, conn_connect_message_format) {
    TestContext ctx;
    kcpmux_engine_t *engine = create_test_engine(&ctx);
    ASSERT_NE(engine, nullptr);

    TestAddr addr(0x7f000001, 12345);
    kcpmux_conn_t
        *conn = kcpmux_conn_connect(engine, addr.get(), nullptr, nullptr, nullptr, nullptr);
    ASSERT_NE(conn, nullptr);

    // Verify message was sent
    ASSERT_EQ(ctx.sent_packets.size(), 1u);
    auto &pkt = ctx.sent_packets[0];

    // Verify format: common(4) + version(1) + ext_len(2) + ext(N)
    ASSERT_GE(pkt.size(), 7u);
    EXPECT_EQ(pkt[0], KCPMUX_MSG_CONN_CONNECT);
    EXPECT_EQ(read_u24(pkt.data() + 1), conn->generation_id);
    EXPECT_NE(conn->generation_id, 0u);
    EXPECT_LE(conn->generation_id, 0x00FFFFFFu);
    EXPECT_EQ(pkt[4], KCPMUX_VERSION);
    uint16_t ext_len = (pkt[5] << 8) | pkt[6];
    EXPECT_EQ(pkt.size(), 7u + ext_len);

    kcpmux_engine_destroy(engine);
}

TEST(kcpmux_protocol, conn_connect_ack_message_format) {
    TestContext ctx;
    kcpmux_engine_t *engine = create_test_engine(&ctx);
    ASSERT_NE(engine, nullptr);

    TestAddr addr(0x7f000001, 12345);

    // Simulate receiving CONN_CONNECT to trigger CONN_CONNECT_ACK
    auto connect_msg = build_conn_connect();
    kcpmux_engine_input(engine, connect_msg.data(), connect_msg.size(), addr.get());

    // Verify CONN_CONNECT_ACK was sent
    ASSERT_EQ(ctx.sent_packets.size(), 1u);
    auto &pkt = ctx.sent_packets[0];

    // Verify format: common(4) + version(1) + result(1) + ext_len(2) + ext(N)
    ASSERT_GE(pkt.size(), 8u);
    EXPECT_EQ(pkt[0], KCPMUX_MSG_CONN_CONNECT_ACK);
    EXPECT_EQ(read_u24(pkt.data() + 1), 1u);
    EXPECT_EQ(pkt[4], KCPMUX_VERSION);
    EXPECT_EQ(pkt[5], KCPMUX_ACK_RESULT_OK);
    uint16_t ext_len = (pkt[6] << 8) | pkt[7];
    EXPECT_EQ(pkt.size(), 8u + ext_len);

    kcpmux_engine_destroy(engine);
}

TEST(kcpmux_protocol, rejected_passive_conn_discards_installed_callbacks) {
    RejectingConnectContext ctx;
    kcpmux_engine_callbacks_t callbacks{};
    callbacks.set_timer = test_set_timer;
    callbacks.write_socket = test_write_socket;
    callbacks.monotonic_time_ms = test_monotonic_time_ms;
    callbacks.conn_connect_notify = reject_conn_after_installing_callbacks;
    kcpmux_engine_t
        *engine = kcpmux_engine_create(nullptr, nullptr, nullptr, &callbacks, &ctx, nullptr);
    ASSERT_NE(engine, nullptr);
    TestAddr addr(0x7f000001, 12345);

    auto connect_msg = build_conn_connect();
    ASSERT_EQ(kcpmux_engine_input(
                  engine, connect_msg.data(), connect_msg.size(), addr.get()),
              0);

    EXPECT_EQ(ctx.rejected_conn_close_calls, 0);
    EXPECT_EQ(kcpmux_engine_get_conn_by_addr(engine, addr.get()), nullptr);
    kcpmux_engine_destroy(engine);
}

TEST(kcpmux_protocol, zero_generation_connect_is_rejected) {
    TestContext ctx;
    kcpmux_engine_t *engine = create_test_engine(&ctx);
    ASSERT_NE(engine, nullptr);
    TestAddr addr(0x7f000001, 12345);

    auto connect_msg = build_conn_connect(0);
    EXPECT_EQ(kcpmux_engine_input(
                  engine, connect_msg.data(), connect_msg.size(), addr.get()),
              -KCPMUX_ERR_INVALID_FORMAT);
    EXPECT_EQ(kcpmux_engine_get_conn_by_addr(engine, addr.get()), nullptr);
    EXPECT_TRUE(ctx.sent_packets.empty());

    kcpmux_engine_destroy(engine);
}

TEST(kcpmux_protocol, matching_generation_updates_conn_but_mismatch_is_isolated) {
    TestContext ctx;
    ctx.current_time_ms = 1000;
    kcpmux_engine_t *engine = create_test_engine(&ctx);
    ASSERT_NE(engine, nullptr);
    TestAddr addr(0x7f000001, 12345);
    constexpr uint32_t generation_id = 0x123456;

    auto connect_msg = build_conn_connect(generation_id);
    ASSERT_EQ(kcpmux_engine_input(
                  engine, connect_msg.data(), connect_msg.size(), addr.get()),
              0);
    kcpmux_conn_t *conn = kcpmux_engine_get_conn_by_addr(engine, addr.get());
    ASSERT_NE(conn, nullptr);
    ASSERT_EQ(conn->generation_id, generation_id);

    ctx.current_time_ms = 1100;
    auto keepalive = build_conn_keepalive(10, 1, generation_id);
    ASSERT_EQ(kcpmux_engine_input(
                  engine, keepalive.data(), keepalive.size(), addr.get()), 0);
    EXPECT_EQ(conn->last_recv_ts, 1100);

    kcpmux_conn_stats_t before = conn->stats;
    int64_t last_recv_before = conn->last_recv_ts;
    int64_t deadline_before = conn->timer_node.deadline_ms;
    uint8_t state_before = conn->state;
    ctx.current_time_ms = 1200;
    keepalive = build_conn_keepalive(11, 2, generation_id + 1);
    EXPECT_EQ(kcpmux_engine_input(
                  engine, keepalive.data(), keepalive.size(), addr.get()),
              -KCPMUX_ERR_NOT_FOUND);
    EXPECT_EQ(conn->stats.rx_packets, before.rx_packets);
    EXPECT_EQ(conn->stats.rx_bytes, before.rx_bytes);
    EXPECT_EQ(conn->last_recv_ts, last_recv_before);
    EXPECT_EQ(conn->timer_node.deadline_ms, deadline_before);
    EXPECT_EQ(conn->state, state_before);

    kcpmux_engine_destroy(engine);
}

TEST(kcpmux_protocol, same_generation_connect_retransmits_ack_without_notify) {
    TestContext ctx;
    kcpmux_engine_t *engine = create_test_engine(&ctx);
    ASSERT_NE(engine, nullptr);
    TestAddr addr(0x7f000001, 12345);
    constexpr uint32_t generation_id = 0x234567;
    auto connect_msg = build_conn_connect(generation_id);

    ASSERT_EQ(kcpmux_engine_input(
                  engine, connect_msg.data(), connect_msg.size(), addr.get()),
              0);
    kcpmux_conn_t *conn = kcpmux_engine_get_conn_by_addr(engine, addr.get());
    ASSERT_NE(conn, nullptr);
    ASSERT_EQ(ctx.conn_notify_count, 1);
    ASSERT_EQ(ctx.sent_packets.size(), 1u);

    ASSERT_EQ(kcpmux_engine_input(
                  engine, connect_msg.data(), connect_msg.size(), addr.get()),
              0);
    EXPECT_EQ(kcpmux_engine_get_conn_by_addr(engine, addr.get()), conn);
    EXPECT_EQ(ctx.conn_notify_count, 1);
    ASSERT_EQ(ctx.sent_packets.size(), 2u);
    EXPECT_EQ(ctx.sent_packets.back()[0], KCPMUX_MSG_CONN_CONNECT_ACK);
    EXPECT_EQ(read_u24(ctx.sent_packets.back().data() + 1), generation_id);

    kcpmux_engine_destroy(engine);
}

TEST(kcpmux_protocol, new_generation_replaces_connection_before_notify) {
    TestContext ctx;
    kcpmux_engine_t *engine = create_test_engine(&ctx);
    ASSERT_NE(engine, nullptr);
    TestAddr addr(0x7f000001, 12345);
    auto first_connect = build_conn_connect(0x010203);
    ASSERT_EQ(kcpmux_engine_input(
                  engine, first_connect.data(), first_connect.size(),
                  addr.get()), 0);
    kcpmux_conn_t *old_conn = kcpmux_engine_get_conn_by_addr(engine, addr.get());
    ASSERT_NE(old_conn, nullptr);
    kcpmux_conn_callbacks_t callbacks = create_conn_callbacks(&ctx);
    kcpmux_conn_set_callbacks(old_conn, &callbacks, &ctx);

    auto replacement_connect = build_conn_connect(0x010205);
    ASSERT_EQ(kcpmux_engine_input(
                  engine, replacement_connect.data(),
                  replacement_connect.size(), addr.get()), 0);

    kcpmux_conn_t *new_conn = kcpmux_engine_get_conn_by_addr(engine, addr.get());
    ASSERT_NE(new_conn, nullptr);
    EXPECT_NE(new_conn, old_conn);
    EXPECT_EQ(new_conn->generation_id, 0x010205u);
    EXPECT_EQ(new_conn->state, KCPMUX_CONN_STATE_CONNECTED);
    EXPECT_EQ(ctx.conn_notify_count, 2);
    EXPECT_EQ(ctx.conn_close_count, 1);
    EXPECT_EQ(ctx.conn_close_reason, KCPMUX_CLOSE_REASON_REPLACED);
    EXPECT_EQ(ctx.conn_close_state, KCPMUX_CONN_STATE_CLOSED);
    ASSERT_EQ(ctx.sent_packets.size(), 2u);
    EXPECT_EQ(read_u24(ctx.sent_packets.back().data() + 1), 0x010205u);

    kcpmux_engine_destroy(engine);
}

TEST(kcpmux_protocol, closed_address_accepts_new_generation_immediately) {
    TestContext ctx;
    kcpmux_engine_t *engine = create_test_engine(&ctx);
    ASSERT_NE(engine, nullptr);
    TestAddr addr(0x7f000001, 12345);
    auto connect_msg = build_conn_connect(0x010203);
    ASSERT_EQ(kcpmux_engine_input(
                  engine, connect_msg.data(), connect_msg.size(), addr.get()),
              0);

    auto close_msg = build_conn_close(KCPMUX_CLOSE_REASON_NORMAL, 0x010203);
    ASSERT_EQ(kcpmux_engine_input(
                  engine, close_msg.data(), close_msg.size(), addr.get()), 0);
    ASSERT_EQ(kcpmux_engine_get_conn_by_addr(engine, addr.get()), nullptr);

    connect_msg = build_conn_connect(0x010205);
    ASSERT_EQ(kcpmux_engine_input(
                  engine, connect_msg.data(), connect_msg.size(), addr.get()),
              0);
    kcpmux_conn_t *conn = kcpmux_engine_get_conn_by_addr(engine, addr.get());
    ASSERT_NE(conn, nullptr);
    EXPECT_EQ(conn->generation_id, 0x010205u);
    EXPECT_EQ(conn->state, KCPMUX_CONN_STATE_CONNECTED);

    kcpmux_engine_destroy(engine);
}

TEST(kcpmux_protocol, conn_close_message_format) {
    TestContext ctx;
    kcpmux_engine_t *engine = create_test_engine(&ctx);
    ASSERT_NE(engine, nullptr);

    TestAddr addr(0x7f000001, 12345);
    kcpmux_conn_callbacks_t callbacks = create_conn_callbacks(&ctx);
    kcpmux_conn_t
        *conn = kcpmux_conn_connect(engine, addr.get(), nullptr, nullptr, &callbacks, &ctx);
    ASSERT_NE(conn, nullptr);

    // Simulate connection established
    auto ack_msg = build_conn_connect_ack(KCPMUX_ACK_RESULT_OK, conn->generation_id);
    ctx.sent_packets.clear();
    kcpmux_engine_input(engine, ack_msg.data(), ack_msg.size(), addr.get());

    // Close connection
    ctx.sent_packets.clear();
    kcpmux_conn_close(conn);

    // Verify CONN_CLOSE was sent
    ASSERT_EQ(ctx.sent_packets.size(), 1u);
    auto &pkt = ctx.sent_packets[0];

    // Verify format: common(4) + reason(1)
    ASSERT_EQ(pkt.size(), 5u);
    EXPECT_EQ(pkt[0], KCPMUX_MSG_CONN_CLOSE);
    EXPECT_EQ(read_u24(pkt.data() + 1), conn->generation_id);
    EXPECT_EQ(pkt[4], KCPMUX_CLOSE_REASON_NORMAL);

    kcpmux_engine_destroy(engine);
}

TEST(kcpmux_protocol, conn_keepalive_message_format) {
    TestContext ctx;
    kcpmux_engine_t *engine = create_test_engine(&ctx);
    ASSERT_NE(engine, nullptr);

    TestAddr addr(0x7f000001, 12345);
    kcpmux_conn_t
        *conn = kcpmux_conn_connect(engine, addr.get(), nullptr, nullptr, nullptr, nullptr);
    ASSERT_NE(conn, nullptr);

    // Simulate connection established
    auto ack_msg = build_conn_connect_ack(KCPMUX_ACK_RESULT_OK, conn->generation_id);
    kcpmux_engine_input(engine, ack_msg.data(), ack_msg.size(), addr.get());

    // Trigger keepalive by calling internal send function
    ctx.sent_packets.clear();
    kcpmux_conn_send_keepalive(conn);

    // Verify CONN_KEEPALIVE was sent
    ASSERT_EQ(ctx.sent_packets.size(), 1u);
    auto &pkt = ctx.sent_packets[0];

    // Verify format: common(4) + time(4) + seq(4)
    ASSERT_EQ(pkt.size(), 12u);
    EXPECT_EQ(pkt[0], KCPMUX_MSG_CONN_KEEPALIVE);
    EXPECT_EQ(read_u24(pkt.data() + 1), conn->generation_id);

    kcpmux_engine_destroy(engine);
}

TEST(kcpmux_protocol, conn_close_ack_message_format) {
    TestContext ctx;
    kcpmux_engine_t *engine = create_test_engine(&ctx);
    ASSERT_NE(engine, nullptr);
    TestAddr addr(0x7f000001, 12345);
    auto connect_msg = build_conn_connect(0x123456);
    ASSERT_EQ(kcpmux_engine_input(
                  engine, connect_msg.data(), connect_msg.size(), addr.get()),
              0);
    ctx.sent_packets.clear();

    auto close_msg = build_conn_close(KCPMUX_CLOSE_REASON_NORMAL, 0x123456);
    ASSERT_EQ(kcpmux_engine_input(
                  engine, close_msg.data(), close_msg.size(), addr.get()), 0);
    ASSERT_EQ(ctx.sent_packets.size(), 1u);
    const auto &pkt = ctx.sent_packets[0];
    ASSERT_EQ(pkt.size(), 5u);
    EXPECT_EQ(pkt[0], KCPMUX_MSG_CONN_CLOSE_ACK);
    EXPECT_EQ(read_u24(pkt.data() + 1), 0x123456u);
    EXPECT_EQ(pkt[4], KCPMUX_CLOSE_REASON_NORMAL);

    kcpmux_engine_destroy(engine);
}

// ============================================================================
// Stream Protocol Message Format Tests
// ============================================================================

TEST(kcpmux_protocol, stream_create_no_control_packet) {
    TestContext ctx;
    kcpmux_engine_t *engine = create_test_engine(&ctx);
    ASSERT_NE(engine, nullptr);

    TestAddr addr(0x7f000001, 12345);
    kcpmux_conn_callbacks_t conn_callbacks = create_conn_callbacks(&ctx);
    kcpmux_conn_t
        *conn = kcpmux_conn_connect(engine, addr.get(), nullptr, nullptr, &conn_callbacks, &ctx);
    ASSERT_NE(conn, nullptr);

    // Simulate connection established
    auto ack_msg = build_conn_connect_ack(KCPMUX_ACK_RESULT_OK, conn->generation_id);
    kcpmux_engine_input(engine, ack_msg.data(), ack_msg.size(), addr.get());

    ctx.sent_packets.clear();
    kcpmux_stream_callbacks_t stream_callbacks = create_stream_callbacks(&ctx);
    kcpmux_stream_t *stream = kcpmux_stream_create(conn, nullptr, &stream_callbacks, &ctx);
    ASSERT_NE(stream, nullptr);
    EXPECT_EQ(kcpmux_stream_get_state(stream), KCPMUX_STREAM_STATE_OPEN);

    // No control packet should be emitted on stream_create.
    EXPECT_EQ(ctx.sent_packets.size(), 0u);

    kcpmux_engine_destroy(engine);
}

TEST(kcpmux_protocol, stream_close_message_format) {
    TestContext ctx;
    kcpmux_engine_t *engine = create_test_engine(&ctx);
    ASSERT_NE(engine, nullptr);

    TestAddr addr(0x7f000001, 12345);
    kcpmux_conn_callbacks_t conn_callbacks = create_conn_callbacks(&ctx);
    kcpmux_conn_t
        *conn = kcpmux_conn_connect(engine, addr.get(), nullptr, nullptr, &conn_callbacks, &ctx);
    ASSERT_NE(conn, nullptr);

    // Simulate connection established
    auto ack_msg = build_conn_connect_ack(KCPMUX_ACK_RESULT_OK, conn->generation_id);
    kcpmux_engine_input(engine, ack_msg.data(), ack_msg.size(), addr.get());

    // Create and open stream
    kcpmux_stream_callbacks_t stream_callbacks = create_stream_callbacks(&ctx);
    kcpmux_stream_t *stream = kcpmux_stream_create(conn, nullptr, &stream_callbacks, &ctx);
    ASSERT_NE(stream, nullptr);
    kcpmux_stream_id(stream);

    // Send payload first to ensure close follows protocol path.
    uint8_t data[] = {0x01};
    ASSERT_EQ(kcpmux_stream_send(stream, data, sizeof(data), 1), 1);

    // Close stream
    ctx.sent_packets.clear();
    kcpmux_stream_close(stream);

    // Verify STREAM_CLOSE was sent
    ASSERT_EQ(ctx.sent_packets.size(), 1u);
    auto &pkt = ctx.sent_packets[0];

    // Verify format: common(4) + stream_id(4) + reason(1)
    ASSERT_EQ(pkt.size(), 9u);
    EXPECT_EQ(pkt[0], KCPMUX_MSG_STREAM_CLOSE);
    EXPECT_EQ(read_u24(pkt.data() + 1), conn->generation_id);
    EXPECT_EQ(read_u32(pkt.data() + 4), kcpmux_stream_id(stream));
    EXPECT_EQ(pkt[8], KCPMUX_CLOSE_REASON_NORMAL);

    kcpmux_engine_destroy(engine);
}

TEST(kcpmux_protocol, stream_close_ack_uses_full_stream_id) {
    TestContext ctx;
    kcpmux_engine_t *engine = create_test_engine(&ctx);
    ASSERT_NE(engine, nullptr);
    TestAddr addr(0x7f000001, 12345);
    auto connect_msg = build_conn_connect(0x123456);
    ASSERT_EQ(kcpmux_engine_input(
                  engine, connect_msg.data(), connect_msg.size(), addr.get()),
              0);
    kcpmux_conn_t *conn = kcpmux_engine_get_conn_by_addr(engine, addr.get());
    ASSERT_NE(conn, nullptr);
    constexpr uint32_t stream_id = 0xAABBCCDD;
    kcpmux_stream_t *stream = kcpmux_stream_new(conn, stream_id, nullptr, 0);
    ASSERT_NE(stream, nullptr);
    kcpmux_conn_add_stream(conn, stream);
    ctx.sent_packets.clear();

    auto close_msg = build_stream_close(stream_id, KCPMUX_CLOSE_REASON_NORMAL, conn->generation_id);
    ASSERT_EQ(kcpmux_engine_input(
                  engine, close_msg.data(), close_msg.size(), addr.get()), 0);
    ASSERT_EQ(ctx.sent_packets.size(), 1u);
    const auto &pkt = ctx.sent_packets[0];
    ASSERT_EQ(pkt.size(), 9u);
    EXPECT_EQ(pkt[0], KCPMUX_MSG_STREAM_CLOSE_ACK);
    EXPECT_EQ(read_u24(pkt.data() + 1), 0x123456u);
    EXPECT_EQ(read_u32(pkt.data() + 4), stream_id);
    EXPECT_EQ(pkt[8], KCPMUX_CLOSE_REASON_NORMAL);

    kcpmux_engine_destroy(engine);
}

TEST(kcpmux_protocol, stream_close_initiator_without_payload_no_control_packet) {
    TestContext ctx;
    kcpmux_engine_t *engine = create_test_engine(&ctx);
    ASSERT_NE(engine, nullptr);

    TestAddr addr(0x7f000001, 12345);
    kcpmux_conn_callbacks_t conn_callbacks = create_conn_callbacks(&ctx);
    kcpmux_conn_t
        *conn = kcpmux_conn_connect(engine, addr.get(), nullptr, nullptr, &conn_callbacks, &ctx);
    ASSERT_NE(conn, nullptr);

    // Simulate connection established
    auto ack_msg = build_conn_connect_ack(KCPMUX_ACK_RESULT_OK, conn->generation_id);
    kcpmux_engine_input(engine, ack_msg.data(), ack_msg.size(), addr.get());

    kcpmux_stream_callbacks_t stream_callbacks = create_stream_callbacks(&ctx);
    kcpmux_stream_t *stream = kcpmux_stream_create(conn, nullptr, &stream_callbacks, &ctx);
    ASSERT_NE(stream, nullptr);
    EXPECT_EQ(stream->is_initiator, 1);
    EXPECT_EQ(stream->stats.up_sent_bytes, 0u);

    ctx.stream_close_count = 0;
    ctx.sent_packets.clear();
    kcpmux_stream_close(stream);

    EXPECT_EQ(ctx.stream_close_state, KCPMUX_STREAM_STATE_CLOSED);
    EXPECT_EQ(conn->stream_count, 0u);
    EXPECT_EQ(ctx.stream_close_count, 1);
    EXPECT_EQ(ctx.sent_packets.size(), 0u);

    kcpmux_engine_destroy(engine);
}

TEST(kcpmux_protocol, stream_payload_message_format) {
    TestContext ctx;
    kcpmux_engine_t *engine = create_test_engine(&ctx);
    ASSERT_NE(engine, nullptr);

    TestAddr addr(0x7f000001, 12345);
    kcpmux_conn_callbacks_t conn_callbacks = create_conn_callbacks(&ctx);
    kcpmux_conn_t
        *conn = kcpmux_conn_connect(engine, addr.get(), nullptr, nullptr, &conn_callbacks, &ctx);
    ASSERT_NE(conn, nullptr);

    // Simulate connection established
    auto ack_msg = build_conn_connect_ack(KCPMUX_ACK_RESULT_OK, conn->generation_id);
    kcpmux_engine_input(engine, ack_msg.data(), ack_msg.size(), addr.get());

    // Create and open stream
    kcpmux_stream_callbacks_t stream_callbacks = create_stream_callbacks(&ctx);
    kcpmux_stream_t *stream = kcpmux_stream_create(conn, nullptr, &stream_callbacks, &ctx);
    ASSERT_NE(stream, nullptr);
    uint32_t stream_id = kcpmux_stream_id(stream);

    // Send data
    ctx.sent_packets.clear();
    uint8_t data[] = {0x01, 0x02, 0x03, 0x04};
    kcpmux_stream_send(stream, data, sizeof(data), 1);

    // Verify STREAM_PAYLOAD was sent
    ASSERT_GE(ctx.sent_packets.size(), 1u);
    auto &pkt = ctx.sent_packets[0];

    // Verify format: common(4) + stream_id(4) + kcp_data(N)
    ASSERT_GE(pkt.size(), 8u);
    EXPECT_EQ(pkt[0], KCPMUX_MSG_STREAM_PAYLOAD);
    EXPECT_EQ(read_u24(pkt.data() + 1), conn->generation_id);
    uint32_t pkt_stream_id = read_u32(pkt.data() + 4);
    EXPECT_EQ(pkt_stream_id, stream_id);

    kcpmux_engine_destroy(engine);
}

// ============================================================================
// Protocol Input Error Handling Tests
// ============================================================================

TEST(kcpmux_protocol, input_invalid_message_type) {
    TestContext ctx;
    kcpmux_engine_t *engine = create_test_engine(&ctx);
    ASSERT_NE(engine, nullptr);

    TestAddr addr(0x7f000001, 12345);

    // Send invalid message type
    uint8_t invalid_msg[] = {0xFF, 0x01, 0x02};
    int ret = kcpmux_engine_input(engine, invalid_msg, sizeof(invalid_msg), addr.get());
    EXPECT_LT(ret, 0);

    kcpmux_engine_destroy(engine);
}

TEST(kcpmux_protocol, input_truncated_message) {
    TestContext ctx;
    kcpmux_engine_t *engine = create_test_engine(&ctx);
    ASSERT_NE(engine, nullptr);

    TestAddr addr(0x7f000001, 12345);

    // Send truncated CONN_CONNECT (missing ext_len)
    uint8_t truncated_msg[] = {
        KCPMUX_MSG_CONN_CONNECT, 0x00, 0x00, 0x01, KCPMUX_VERSION
    };
    int ret = kcpmux_engine_input(engine, truncated_msg, sizeof(truncated_msg), addr.get());
    EXPECT_LT(ret, 0);

    kcpmux_engine_destroy(engine);
}

TEST(kcpmux_protocol, input_version_mismatch) {
    TestContext ctx;
    kcpmux_engine_t *engine = create_test_engine(&ctx);
    ASSERT_NE(engine, nullptr);

    TestAddr addr(0x7f000001, 12345);

    // Send CONN_CONNECT with wrong version
    std::vector<uint8_t> bad_version_msg;
    bad_version_msg = build_conn_connect();
    bad_version_msg[4] = 0xFF;

    int ret = kcpmux_engine_input(
        engine,
        bad_version_msg.data(),
        bad_version_msg.size(),
        addr.get());
    EXPECT_LT(ret, 0);

    kcpmux_engine_destroy(engine);
}

TEST(kcpmux_protocol, input_oversized_ext) {
    TestContext ctx;
    kcpmux_engine_t *engine = create_test_engine(&ctx);
    ASSERT_NE(engine, nullptr);

    TestAddr addr(0x7f000001, 12345);

    // Build message with ext_len larger than KCPMUX_PROTO_EXT_MAX_LEN
    std::vector<uint8_t> msg;
    msg.push_back(KCPMUX_MSG_CONN_CONNECT);
    msg.push_back(0x00);
    msg.push_back(0x00);
    msg.push_back(0x01);
    msg.push_back(KCPMUX_VERSION);
    uint16_t bad_ext_len = KCPMUX_PROTO_EXT_MAX_LEN + 100;
    msg.push_back((bad_ext_len >> 8) & 0xff);
    msg.push_back(bad_ext_len & 0xff);

    int ret = kcpmux_engine_input(engine, msg.data(), msg.size(), addr.get());
    EXPECT_LT(ret, 0);

    kcpmux_engine_destroy(engine);
}

// ============================================================================
// stream_id Validation Tests
// ============================================================================

TEST(kcpmux_protocol, stream_payload_rejects_wrong_parity_stream_id) {
    TestContext ctx;
    kcpmux_engine_t *engine = create_test_engine(&ctx);
    ASSERT_NE(engine, nullptr);

    TestAddr addr(0x7f000001, 12345);

    // Simulate receiving CONN_CONNECT to create server-side connection (acceptor, is_initiator=0).
    auto connect_msg = build_conn_connect();
    int ret = kcpmux_engine_input(engine, connect_msg.data(), connect_msg.size(), addr.get());
    EXPECT_EQ(ret, 0);

    kcpmux_conn_t *conn = kcpmux_engine_get_conn_by_addr(engine, addr.get());
    ASSERT_NE(conn, nullptr);
    EXPECT_EQ(conn->is_initiator, 0);  // Verify this is acceptor side

    const uint8_t fake_kcp[] = {0x00, 0x01};
    auto payload_msg = build_stream_payload(2, fake_kcp, sizeof(fake_kcp));

    ret = kcpmux_engine_input(engine, payload_msg.data(), payload_msg.size(), addr.get());
    EXPECT_EQ(ret, -KCPMUX_ERR_INVALID_FORMAT);

    kcpmux_engine_destroy(engine);
}

TEST(kcpmux_protocol, invalid_first_payload_rolls_back_passive_stream) {
    TestContext ctx;
    kcpmux_engine_t *engine = create_test_engine(&ctx);
    ASSERT_NE(engine, nullptr);

    TestAddr addr(0x7f000001, 12345);

    // Build server-side connection (acceptor).
    auto connect_msg = build_conn_connect();
    int ret = kcpmux_engine_input(engine, connect_msg.data(), connect_msg.size(), addr.get());
    EXPECT_EQ(ret, 0);
    ctx.sent_packets.clear();

    kcpmux_conn_t *conn = kcpmux_engine_get_conn_by_addr(engine, addr.get());
    ASSERT_NE(conn, nullptr);

    // Explicitly enable passive stream creation callback.
    kcpmux_conn_callbacks_t callbacks = create_conn_callbacks(&ctx);
    kcpmux_conn_set_callbacks(conn, &callbacks, &ctx);

    // A valid peer stream ID reaches passive creation before KCP rejects data.
    const uint8_t fake_kcp[] = {0x00, 0x01};
    auto payload_msg = build_stream_payload(1, fake_kcp, sizeof(fake_kcp));

    ret = kcpmux_engine_input(engine, payload_msg.data(), payload_msg.size(), addr.get());
    EXPECT_LE(ret, KCPMUX_ERR_KCPRET(0)); // invalid kcp format

    EXPECT_EQ(ctx.stream_notify_count, 1);
    EXPECT_EQ(kcpmux_conn_get_stream_by_id(conn, 1), nullptr);
    EXPECT_EQ(engine->stats.stream_closed_total, 1u);

    kcpmux_engine_destroy(engine);
}

TEST(kcpmux_protocol, stream_payload_without_create_notify_drops_silently) {
    TestContext ctx;
    kcpmux_engine_t *engine = create_test_engine(&ctx);
    ASSERT_NE(engine, nullptr);

    TestAddr addr(0x7f000001, 12345);

    // Build server-side connection (acceptor) without conn callbacks.
    auto connect_msg = build_conn_connect();
    int ret = kcpmux_engine_input(engine, connect_msg.data(), connect_msg.size(), addr.get());
    EXPECT_EQ(ret, 0);

    kcpmux_conn_t *conn = kcpmux_engine_get_conn_by_addr(engine, addr.get());
    ASSERT_NE(conn, nullptr);
    EXPECT_EQ(conn->callbacks.stream_create_notify, nullptr);

    int64_t old_last_recv_ts = conn->last_recv_ts;
    int64_t old_last_payload_ts = conn->last_payload_ts;

    // Advance input time to verify timestamps are unchanged on silent drop.
    ctx.current_time_ms = old_last_recv_ts + 123;

    const uint8_t fake_kcp[] = {0x00, 0x01};
    auto payload_msg = build_stream_payload(1, fake_kcp, sizeof(fake_kcp));

    ret = kcpmux_engine_input(engine, payload_msg.data(), payload_msg.size(), addr.get());
    EXPECT_EQ(ret, -KCPMUX_ERR_NOT_FOUND);

    EXPECT_EQ(conn->last_recv_ts, old_last_recv_ts);
    EXPECT_EQ(conn->last_payload_ts, old_last_payload_ts);
    EXPECT_EQ(kcpmux_conn_get_stream_by_id(conn, 1), nullptr);

    kcpmux_engine_destroy(engine);
}

TEST(kcpmux_protocol, stream_close_rejects_zero_stream_id) {
    TestContext ctx;
    kcpmux_engine_t *engine = create_test_engine(&ctx);
    ASSERT_NE(engine, nullptr);

    TestAddr addr(0x7f000001, 12345);

    // First establish a connection
    auto connect_msg = build_conn_connect();
    kcpmux_engine_input(engine, connect_msg.data(), connect_msg.size(), addr.get());

    kcpmux_conn_t *conn = kcpmux_engine_get_conn_by_addr(engine, addr.get());
    ASSERT_NE(conn, nullptr);

    // Build STREAM_CLOSE with stream_id=0
    auto close_msg = build_stream_close(0, KCPMUX_CLOSE_REASON_NORMAL);
    int ret = kcpmux_engine_input(engine, close_msg.data(), close_msg.size(), addr.get());

    // Should return error
    EXPECT_EQ(ret, -KCPMUX_ERR_INVALID_FORMAT);

    kcpmux_engine_destroy(engine);
}

TEST(kcpmux_protocol, stream_payload_rejects_zero_stream_id) {
    TestContext ctx;
    kcpmux_engine_t *engine = create_test_engine(&ctx);
    ASSERT_NE(engine, nullptr);

    TestAddr addr(0x7f000001, 12345);

    // First establish a connection
    auto connect_msg = build_conn_connect();
    kcpmux_engine_input(engine, connect_msg.data(), connect_msg.size(), addr.get());

    kcpmux_conn_t *conn = kcpmux_engine_get_conn_by_addr(engine, addr.get());
    ASSERT_NE(conn, nullptr);

    auto payload_msg = build_stream_payload(0, nullptr, 0);

    int ret = kcpmux_engine_input(engine, payload_msg.data(), payload_msg.size(), addr.get());

    // Should return error
    EXPECT_EQ(ret, -KCPMUX_ERR_INVALID_FORMAT);

    kcpmux_engine_destroy(engine);
}

}  // namespace
