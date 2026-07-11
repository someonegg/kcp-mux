#include "kcpmux_test_utils.h"

using namespace kcpmux_test;

namespace {

// ============================================================================
// Connection Protocol Message Format Tests
// ============================================================================

TEST(kcpmux_protocol, conn_connect_message_format) {
    TestContext ctx;
    kcpmux_engine_t *engine = create_test_engine(&ctx);
    ASSERT_NE(engine, nullptr);

    TestAddr addr(0x7f000001, 12345);
    kcpmux_conn_t *conn = kcpmux_conn_connect(engine, addr.get(), nullptr, nullptr, nullptr, nullptr);
    ASSERT_NE(conn, nullptr);

    // Verify message was sent
    ASSERT_EQ(ctx.sent_packets.size(), 1u);
    auto &pkt = ctx.sent_packets[0];

    // Verify format: type(1) + version(1) + ext_len(2) + ext(N)
    ASSERT_GE(pkt.size(), 4u);
    EXPECT_EQ(pkt[0], KCPMUX_MSG_CONN_CONNECT);
    EXPECT_EQ(pkt[1], KCPMUX_VERSION);
    uint16_t ext_len = (pkt[2] << 8) | pkt[3];
    EXPECT_EQ(pkt.size(), 4u + ext_len);

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

    // Verify format: type(1) + version(1) + result(1) + ext_len(2) + ext(N)
    ASSERT_GE(pkt.size(), 5u);
    EXPECT_EQ(pkt[0], KCPMUX_MSG_CONN_CONNECT_ACK);
    EXPECT_EQ(pkt[1], KCPMUX_VERSION);
    EXPECT_EQ(pkt[2], KCPMUX_ACK_RESULT_OK);
    uint16_t ext_len = (pkt[3] << 8) | pkt[4];
    EXPECT_EQ(pkt.size(), 5u + ext_len);

    kcpmux_engine_destroy(engine);
}

TEST(kcpmux_protocol, conn_close_message_format) {
    TestContext ctx;
    kcpmux_engine_t *engine = create_test_engine(&ctx);
    ASSERT_NE(engine, nullptr);

    TestAddr addr(0x7f000001, 12345);
    kcpmux_conn_callbacks_t callbacks = create_conn_callbacks(&ctx);
    kcpmux_conn_t *conn = kcpmux_conn_connect(engine, addr.get(), nullptr, nullptr, &callbacks, &ctx);
    ASSERT_NE(conn, nullptr);

    // Simulate connection established
    auto ack_msg = build_conn_connect_ack(KCPMUX_ACK_RESULT_OK);
    ctx.sent_packets.clear();
    kcpmux_engine_input(engine, ack_msg.data(), ack_msg.size(), addr.get());

    // Close connection
    ctx.sent_packets.clear();
    kcpmux_conn_close(conn);

    // Verify CONN_CLOSE was sent
    ASSERT_EQ(ctx.sent_packets.size(), 1u);
    auto &pkt = ctx.sent_packets[0];

    // Verify format: type(1) + reason(1)
    ASSERT_EQ(pkt.size(), 2u);
    EXPECT_EQ(pkt[0], KCPMUX_MSG_CONN_CLOSE);
    EXPECT_EQ(pkt[1], KCPMUX_CLOSE_REASON_NORMAL);

    kcpmux_engine_destroy(engine);
}

TEST(kcpmux_protocol, conn_keepalive_message_format) {
    TestContext ctx;
    kcpmux_engine_t *engine = create_test_engine(&ctx);
    ASSERT_NE(engine, nullptr);

    TestAddr addr(0x7f000001, 12345);
    kcpmux_conn_t *conn = kcpmux_conn_connect(engine, addr.get(), nullptr, nullptr, nullptr, nullptr);
    ASSERT_NE(conn, nullptr);

    // Simulate connection established
    auto ack_msg = build_conn_connect_ack(KCPMUX_ACK_RESULT_OK);
    kcpmux_engine_input(engine, ack_msg.data(), ack_msg.size(), addr.get());

    // Trigger keepalive by calling internal send function
    ctx.sent_packets.clear();
    kcpmux_conn_send_keepalive(conn);

    // Verify CONN_KEEPALIVE was sent
    ASSERT_EQ(ctx.sent_packets.size(), 1u);
    auto &pkt = ctx.sent_packets[0];

    // Verify format: type(1) + time(4) + seq(4)
    ASSERT_EQ(pkt.size(), 9u);
    EXPECT_EQ(pkt[0], KCPMUX_MSG_CONN_KEEPALIVE);

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
    kcpmux_conn_t *conn = kcpmux_conn_connect(engine, addr.get(), nullptr, nullptr, &conn_callbacks, &ctx);
    ASSERT_NE(conn, nullptr);

    // Simulate connection established
    auto ack_msg = build_conn_connect_ack(KCPMUX_ACK_RESULT_OK);
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
    kcpmux_conn_t *conn = kcpmux_conn_connect(engine, addr.get(), nullptr, nullptr, &conn_callbacks, &ctx);
    ASSERT_NE(conn, nullptr);

    // Simulate connection established
    auto ack_msg = build_conn_connect_ack(KCPMUX_ACK_RESULT_OK);
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

    // Verify format: type(1) + stream_id(3) + reason(1)
    ASSERT_EQ(pkt.size(), 5u);
    EXPECT_EQ(pkt[0], KCPMUX_MSG_STREAM_CLOSE);
    EXPECT_EQ(pkt[4], KCPMUX_CLOSE_REASON_NORMAL);

    kcpmux_engine_destroy(engine);
}

TEST(kcpmux_protocol, stream_close_initiator_without_payload_no_control_packet) {
    TestContext ctx;
    kcpmux_engine_t *engine = create_test_engine(&ctx);
    ASSERT_NE(engine, nullptr);

    TestAddr addr(0x7f000001, 12345);
    kcpmux_conn_callbacks_t conn_callbacks = create_conn_callbacks(&ctx);
    kcpmux_conn_t *conn = kcpmux_conn_connect(engine, addr.get(), nullptr, nullptr, &conn_callbacks, &ctx);
    ASSERT_NE(conn, nullptr);

    // Simulate connection established
    auto ack_msg = build_conn_connect_ack(KCPMUX_ACK_RESULT_OK);
    kcpmux_engine_input(engine, ack_msg.data(), ack_msg.size(), addr.get());

    kcpmux_stream_callbacks_t stream_callbacks = create_stream_callbacks(&ctx);
    kcpmux_stream_t *stream = kcpmux_stream_create(conn, nullptr, &stream_callbacks, &ctx);
    ASSERT_NE(stream, nullptr);
    EXPECT_EQ(stream->is_initiator, 1);
    EXPECT_EQ(stream->stats.up_sent_bytes, 0u);

    ctx.stream_close_count = 0;
    ctx.sent_packets.clear();
    kcpmux_stream_close(stream);

    EXPECT_EQ(kcpmux_stream_get_state(stream), KCPMUX_STREAM_STATE_CLOSED);
    EXPECT_TRUE(stream->internal_closed);
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
    kcpmux_conn_t *conn = kcpmux_conn_connect(engine, addr.get(), nullptr, nullptr, &conn_callbacks, &ctx);
    ASSERT_NE(conn, nullptr);

    // Simulate connection established
    auto ack_msg = build_conn_connect_ack(KCPMUX_ACK_RESULT_OK);
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

    // Verify format: type(1) + stream_id(3) + kcp_data(N)
    ASSERT_GE(pkt.size(), 4u);
    EXPECT_EQ(pkt[0], KCPMUX_MSG_STREAM_PAYLOAD);
    uint32_t pkt_stream_id = ((uint32_t)pkt[1] << 16) | ((uint32_t)pkt[2] << 8) | pkt[3];
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
    uint8_t truncated_msg[] = {KCPMUX_MSG_CONN_CONNECT, KCPMUX_VERSION};
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
    bad_version_msg.push_back(KCPMUX_MSG_CONN_CONNECT);
    bad_version_msg.push_back(0xFF);  // Invalid version
    bad_version_msg.push_back(0x00);  // ext_len high byte
    bad_version_msg.push_back(0x00);  // ext_len low byte

    int ret = kcpmux_engine_input(engine, bad_version_msg.data(), bad_version_msg.size(), addr.get());
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

    std::vector<uint8_t> payload_msg;
    payload_msg.push_back(KCPMUX_MSG_STREAM_PAYLOAD);
    payload_msg.push_back(0x00);
    payload_msg.push_back(0x00);
    payload_msg.push_back(0x02);  // even id, wrong parity for acceptor
    payload_msg.push_back(0x00);  // fake kcp bytes
    payload_msg.push_back(0x01);

    ret = kcpmux_engine_input(engine, payload_msg.data(), payload_msg.size(), addr.get());
    EXPECT_EQ(ret, -KCPMUX_ERR_INVALID_FORMAT);

    kcpmux_engine_destroy(engine);
}

TEST(kcpmux_protocol, stream_payload_auto_creates_on_acceptor) {
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

    // Odd stream_id payload from initiator should trigger auto-create.
    std::vector<uint8_t> payload_msg;
    payload_msg.push_back(KCPMUX_MSG_STREAM_PAYLOAD);
    payload_msg.push_back(0x00);
    payload_msg.push_back(0x00);
    payload_msg.push_back(0x01);
    payload_msg.push_back(0x00);
    payload_msg.push_back(0x01);

    ret = kcpmux_engine_input(engine, payload_msg.data(), payload_msg.size(), addr.get());
    EXPECT_LE(ret, KCPMUX_ERR_KCPRET(0)); // invalid kcp format

    kcpmux_stream_t *stream = kcpmux_conn_get_stream_by_id(conn, 1);
    ASSERT_NE(stream, nullptr);
    EXPECT_EQ(kcpmux_stream_get_state(stream), KCPMUX_STREAM_STATE_ERROR); // invalid kcp format

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

    std::vector<uint8_t> payload_msg;
    payload_msg.push_back(KCPMUX_MSG_STREAM_PAYLOAD);
    payload_msg.push_back(0x00);
    payload_msg.push_back(0x00);
    payload_msg.push_back(0x01);  // odd id, valid parity for acceptor
    payload_msg.push_back(0x00);
    payload_msg.push_back(0x01);

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

    // Build STREAM_PAYLOAD with stream_id=0
    std::vector<uint8_t> payload_msg;
    payload_msg.push_back(KCPMUX_MSG_STREAM_PAYLOAD);
    payload_msg.push_back(0x00);  // stream_id=0 (24-bit)
    payload_msg.push_back(0x00);
    payload_msg.push_back(0x00);

    int ret = kcpmux_engine_input(engine, payload_msg.data(), payload_msg.size(), addr.get());

    // Should return error
    EXPECT_EQ(ret, -KCPMUX_ERR_INVALID_FORMAT);

    kcpmux_engine_destroy(engine);
}

}  // namespace
