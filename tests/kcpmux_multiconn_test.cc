#include "kcpmux_test_utils.h"

using namespace kcpmux_test;

namespace {
class MultiConnectionTest : public DualEngineTest {};

TEST_F(MultiConnectionTest, simultaneous_connect_with_equal_endpoint_ids) {
    ctx.client_engine->next_connection_id = 0x1122334455667788ULL;
    ctx.server_engine->next_connection_id = 0x1122334455667788ULL;
    auto client_callbacks = create_conn_callbacks(&ctx.client_ctx);
    auto server_callbacks = create_conn_callbacks(&ctx.server_ctx);
    auto *client_out = kcpmux_conn_connect(ctx.client_engine, ctx.server_addr.get(),
        nullptr, nullptr, &client_callbacks, &ctx.client_ctx);
    auto *server_out = kcpmux_conn_connect(ctx.server_engine, ctx.client_addr.get(),
        nullptr, nullptr, &server_callbacks, &ctx.server_ctx);
    ASSERT_NE(client_out, nullptr);
    ASSERT_NE(server_out, nullptr);
    EXPECT_EQ(kcpmux_conn_get_id(client_out), kcpmux_conn_get_id(server_out));
    EXPECT_EQ(kcpmux_conn_get_peer_id(client_out), 0u);
    ctx.deliver_all();
    ctx.deliver_all();
    ASSERT_EQ(client_out->state, KCPMUX_CONN_STATE_CONNECTED);
    ASSERT_EQ(server_out->state, KCPMUX_CONN_STATE_CONNECTED);
    EXPECT_EQ(ctx.client_engine->conn_count, 2u);
    EXPECT_EQ(ctx.server_engine->conn_count, 2u);
    auto *client_in = kcpmux_engine_get_conn_by_id(ctx.client_engine,
        ctx.server_addr.get(), kcpmux_conn_get_peer_id(server_out));
    auto *server_in = kcpmux_engine_get_conn_by_id(ctx.server_engine,
        ctx.client_addr.get(), kcpmux_conn_get_peer_id(client_out));
    ASSERT_NE(client_in, nullptr);
    ASSERT_NE(server_in, nullptr);
    EXPECT_NE(client_in, client_out);
    EXPECT_NE(server_in, server_out);
    EXPECT_EQ(kcpmux_conn_get_peer_id(client_in), kcpmux_conn_get_id(server_out));
    EXPECT_EQ(kcpmux_conn_get_peer_id(server_in), kcpmux_conn_get_id(client_out));
    EXPECT_EQ(ctx.client_ctx.conn_notify_count, 1);
    EXPECT_EQ(ctx.server_ctx.conn_notify_count, 1);
}

TEST_F(MultiConnectionTest, dropped_connect_and_ack_retry_without_duplicate_accept) {
    auto callbacks = create_conn_callbacks(&ctx.client_ctx);
    auto *conn = kcpmux_conn_connect(ctx.client_engine, ctx.server_addr.get(),
        nullptr, nullptr, &callbacks, &ctx.client_ctx);
    ASSERT_NE(conn, nullptr);
    ctx.client_ctx.sent_packets.clear(); // Drop initial CONNECT.
    ctx.advance_time(KCPMUX_DEFAULT_CONTROL_TIMEOUT_MS);
    kcpmux_engine_update(ctx.client_engine);
    ctx.deliver_client_to_server();
    ASSERT_EQ(ctx.server_ctx.conn_notify_count, 1);
    ASSERT_EQ(ctx.server_ctx.sent_packets.size(), 1u);
    auto first_ack = ctx.server_ctx.sent_packets.front();
    ctx.server_ctx.sent_packets.clear(); // Drop initial ACK.
    ctx.advance_time(KCPMUX_DEFAULT_CONTROL_TIMEOUT_MS);
    kcpmux_engine_update(ctx.client_engine);
    ctx.deliver_client_to_server();
    ASSERT_EQ(ctx.server_ctx.sent_packets.size(), 1u);
    EXPECT_EQ(ctx.server_ctx.sent_packets.front(), first_ack);
    EXPECT_EQ(ctx.server_ctx.conn_notify_count, 1);
    EXPECT_EQ(ctx.server_engine->conn_count, 1u);
    EXPECT_EQ(ctx.server_engine->handshake_map->num, 1);
    ctx.deliver_server_to_client();
    EXPECT_EQ(conn->state, KCPMUX_CONN_STATE_CONNECTED);
    EXPECT_EQ(kcpmux_conn_get_peer_id(conn), read_u64(first_ack.data() + 9));
}

TEST_F(MultiConnectionTest, same_stream_ids_are_isolated_and_close_is_independent) {
    auto first = connect_pair();
    auto second = connect_pair();
    ASSERT_NE(first.server, nullptr);
    ASSERT_NE(second.server, nullptr);
    first.client->next_stream_id = second.client->next_stream_id = 1;
    auto a = open_stream_pair(first, nullptr, 'A');
    auto b = open_stream_pair(second, nullptr, 'B');
    ASSERT_NE(a.server, nullptr);
    ASSERT_NE(b.server, nullptr);
    EXPECT_EQ(a.stream_id, b.stream_id);
    uint8_t bootstrap;
    ASSERT_EQ(kcpmux_stream_recv(a.server, &bootstrap, 1), 1);
    EXPECT_EQ(bootstrap, 'A');
    ASSERT_EQ(kcpmux_stream_recv(b.server, &bootstrap, 1), 1);
    EXPECT_EQ(bootstrap, 'B');

    TestContext first_notifications, second_notifications;
    auto a_callbacks = create_stream_callbacks(&first_notifications);
    auto b_callbacks = create_stream_callbacks(&second_notifications);
    kcpmux_stream_set_callbacks(a.server, &a_callbacks, &first_notifications);
    kcpmux_stream_set_callbacks(b.server, &b_callbacks, &second_notifications);
    const uint8_t alpha[] = {'a', 'l', 'p', 'h', 'a'};
    const uint8_t beta[] = {'b', 'e', 't', 'a'};
    auto a_before = first.server->stats;
    auto b_before = second.server->stats;
    ASSERT_EQ(kcpmux_stream_send(a.client, alpha, sizeof(alpha), 1), sizeof(alpha));
    ASSERT_EQ(kcpmux_stream_send(b.client, beta, sizeof(beta), 1), sizeof(beta));
    pump_kcp(ctx);
    uint8_t buf[32];
    ASSERT_EQ(kcpmux_stream_recv(a.server, buf, sizeof(buf)), sizeof(alpha));
    EXPECT_EQ(memcmp(buf, alpha, sizeof(alpha)), 0);
    ASSERT_EQ(kcpmux_stream_recv(b.server, buf, sizeof(buf)), sizeof(beta));
    EXPECT_EQ(memcmp(buf, beta, sizeof(beta)), 0);
    EXPECT_EQ(first_notifications.read_notify_count, 1);
    EXPECT_EQ(second_notifications.read_notify_count, 1);
    EXPECT_EQ(first.server->stats.rx_bytes - a_before.rx_bytes,
        KCPMUX_PROTO_STREAM_HEADER_LEN + KCPMUX_IKCP_OVERHEAD + sizeof(alpha));
    EXPECT_EQ(second.server->stats.rx_bytes - b_before.rx_bytes,
        KCPMUX_PROTO_STREAM_HEADER_LEN + KCPMUX_IKCP_OVERHEAD + sizeof(beta));

    const uint64_t first_id = kcpmux_conn_get_id(first.client);
    const uint64_t first_server_id = kcpmux_conn_get_id(first.server);
    ASSERT_EQ(kcpmux_conn_close(first.client), 0);
    ctx.deliver_all();
    EXPECT_EQ(kcpmux_engine_get_conn_by_id(ctx.client_engine, ctx.server_addr.get(), first_id), nullptr);
    EXPECT_EQ(ctx.client_engine->conn_count, 1u);
    EXPECT_EQ(ctx.server_engine->conn_count, 1u);
    EXPECT_EQ(ctx.server_engine->handshake_map->num, 1);
    EXPECT_EQ(first_notifications.stream_close_count, 1);
    EXPECT_EQ(second_notifications.stream_close_count, 0);
    auto stale = build_stream_payload(1, alpha, sizeof(alpha), first_server_id);
    EXPECT_EQ(kcpmux_engine_input(ctx.server_engine, stale.data(), stale.size(), ctx.client_addr.get()),
        -KCPMUX_ERR_NOT_FOUND);
    ASSERT_EQ(kcpmux_stream_send(b.client, beta, sizeof(beta), 1), sizeof(beta));
    ctx.advance_time(20);
    pump_kcp(ctx);
    ASSERT_EQ(kcpmux_stream_recv(b.server, buf, sizeof(buf)), sizeof(beta));
    EXPECT_EQ(memcmp(buf, beta, sizeof(beta)), 0);
}

TEST_F(MultiConnectionTest, rejecting_one_handshake_keeps_existing_connection) {
    auto first = connect_pair();
    ASSERT_NE(first.server, nullptr);
    const uint64_t first_server_id = kcpmux_conn_get_id(first.server);
    ctx.server_ctx.conn_notify_result = KCPMUX_ACK_RESULT_CUSTOM;
    TestContext rejected;
    auto callbacks = create_conn_callbacks(&rejected);
    auto *second = kcpmux_conn_connect(ctx.client_engine, ctx.server_addr.get(),
        nullptr, nullptr, &callbacks, &rejected);
    ASSERT_NE(second, nullptr);
    const uint64_t rejected_id = kcpmux_conn_get_id(second);
    ctx.deliver_all();
    EXPECT_EQ(rejected.conn_close_count, 1);
    EXPECT_EQ(rejected.conn_close_reason, KCPMUX_CLOSE_REASON_REJECTED);
    EXPECT_EQ(kcpmux_engine_get_conn_by_id(ctx.client_engine, ctx.server_addr.get(), rejected_id), nullptr);
    EXPECT_EQ(kcpmux_engine_get_conn_by_id(ctx.server_engine, ctx.client_addr.get(), first_server_id), first.server);
    EXPECT_EQ(first.client->state, KCPMUX_CONN_STATE_CONNECTED);
    EXPECT_EQ(ctx.client_engine->conn_count, 1u);
    EXPECT_EQ(ctx.server_engine->handshake_map->num, 1);
}

TEST_F(MultiConnectionTest, timeout_is_per_connection) {
    kcpmux_conn_config_t short_config;
    kcpmux_conn_config_init(&short_config);
    short_config.keepalive_interval_ms = 0;
    short_config.keepalive_timeout_ms = 10;
    short_config.idle_timeout_ms = 0;
    auto first = connect_pair(&short_config);
    auto second = connect_pair();
    ASSERT_NE(first.server, nullptr);
    ASSERT_NE(second.server, nullptr);
    const uint64_t first_id = kcpmux_conn_get_id(first.client);
    ctx.advance_time(11);
    kcpmux_engine_update(ctx.client_engine);
    EXPECT_EQ(kcpmux_engine_get_conn_by_id(ctx.client_engine, ctx.server_addr.get(), first_id), nullptr);
    EXPECT_EQ(second.client->state, KCPMUX_CONN_STATE_CONNECTED);
    EXPECT_EQ(ctx.client_engine->conn_count, 1u);
    EXPECT_EQ(ctx.client_ctx.conn_close_count, 1);
    EXPECT_EQ(ctx.client_ctx.conn_close_reason, KCPMUX_CLOSE_REASON_TIMEOUT);
}

TEST_F(MultiConnectionTest, connecting_timeout_does_not_close_connected_neighbor) {
    auto existing = connect_pair();
    ASSERT_NE(existing.server, nullptr);
    kcpmux_conn_config_t config;
    kcpmux_conn_config_init(&config);
    config.ctrl_timeout_ms = 10;
    config.connect_retries = 0;
    auto *pending = kcpmux_conn_connect(ctx.client_engine, ctx.server_addr.get(),
        &config, nullptr, nullptr, nullptr);
    ASSERT_NE(pending, nullptr);
    const uint64_t pending_id = kcpmux_conn_get_id(pending);
    ctx.client_ctx.sent_packets.clear(); // Drop only the new handshake.
    ctx.advance_time(10);
    kcpmux_engine_update(ctx.client_engine);
    EXPECT_EQ(kcpmux_engine_get_conn_by_id(ctx.client_engine, ctx.server_addr.get(), pending_id), nullptr);
    EXPECT_EQ(existing.client->state, KCPMUX_CONN_STATE_CONNECTED);
    EXPECT_EQ(ctx.client_engine->conn_count, 1u);
}

TEST_F(MultiConnectionTest, same_source_id_from_different_addresses_is_independent) {
    auto message = build_conn_connect(0xfedcba9876543210ULL);
    TestAddr other(0x7f000001, 54321);
    ASSERT_EQ(kcpmux_engine_input(ctx.server_engine, message.data(), message.size(), ctx.client_addr.get()), 0);
    ASSERT_EQ(kcpmux_engine_input(ctx.server_engine, message.data(), message.size(), other.get()), 0);
    auto *first = kcpmux_engine_get_conn_by_id(ctx.server_engine, ctx.client_addr.get(), 1);
    auto *second = kcpmux_engine_get_conn_by_id(ctx.server_engine, other.get(), 2);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(kcpmux_conn_get_peer_id(first), kcpmux_conn_get_peer_id(second));
    EXPECT_EQ(ctx.server_engine->handshake_map->num, 2);
    EXPECT_EQ(kcpmux_engine_get_conn_by_id(ctx.server_engine, other.get(), 1), nullptr);
    EXPECT_EQ(ctx.server_ctx.conn_notify_count, 2);
}

TEST_F(MultiConnectionTest, routing_rejects_wrong_address_unknown_id_and_zero_id) {
    auto pair = connect_pair();
    ASSERT_NE(pair.server, nullptr);
    const uint64_t id = kcpmux_conn_get_id(pair.client);
    auto message = build_conn_keepalive(0, 0, id);
    TestAddr wrong_addr(0x7f000001, 1234);
    auto before = pair.client->stats;
    EXPECT_EQ(kcpmux_engine_input(ctx.client_engine, message.data(), message.size(), wrong_addr.get()),
        -KCPMUX_ERR_NOT_FOUND);
    write_u64(message.data() + 1, id + 1);
    EXPECT_EQ(kcpmux_engine_input(ctx.client_engine, message.data(), message.size(), ctx.server_addr.get()),
        -KCPMUX_ERR_NOT_FOUND);
    write_u64(message.data() + 1, 0);
    EXPECT_EQ(kcpmux_engine_input(ctx.client_engine, message.data(), message.size(), ctx.server_addr.get()),
        -KCPMUX_ERR_INVALID_FORMAT);
    EXPECT_EQ(pair.client->stats.rx_packets, before.rx_packets);
    EXPECT_EQ(kcpmux_engine_get_conn_by_id(ctx.client_engine, ctx.server_addr.get(), 0), nullptr);
    EXPECT_EQ(kcpmux_conn_get_id(nullptr), 0u);
    EXPECT_EQ(kcpmux_conn_get_peer_id(nullptr), 0u);
}

TEST_F(MultiConnectionTest, connect_retransmission_does_not_revive_closing_connection) {
    auto pair = connect_pair();
    ASSERT_NE(pair.server, nullptr);
    ASSERT_EQ(kcpmux_conn_close(pair.server), 0);
    auto connect = build_conn_connect(kcpmux_conn_get_id(pair.client));
    const auto packets_before = ctx.server_ctx.sent_packets.size();
    EXPECT_EQ(kcpmux_engine_input(ctx.server_engine, connect.data(), connect.size(), ctx.client_addr.get()),
        -KCPMUX_ERR_STATE);
    EXPECT_EQ(pair.server->state, KCPMUX_CONN_STATE_CLOSING);
    EXPECT_EQ(ctx.server_ctx.sent_packets.size(), packets_before);
    EXPECT_EQ(ctx.server_ctx.conn_notify_count, 1);
}

TEST_F(MultiConnectionTest, id_exhaustion_does_not_wrap_or_disturb_existing_connection) {
    ctx.client_engine->next_connection_id = UINT64_MAX;
    auto pair = connect_pair();
    ASSERT_NE(pair.server, nullptr);
    EXPECT_EQ(kcpmux_conn_get_id(pair.client), UINT64_MAX);
    EXPECT_EQ(ctx.client_engine->next_connection_id, 0u);
    auto nodes_before = ctx.client_engine->timer_node_count;
    EXPECT_EQ(kcpmux_conn_connect(ctx.client_engine, ctx.server_addr.get(), nullptr, nullptr, nullptr, nullptr), nullptr);
    EXPECT_EQ(ctx.client_engine->conn_count, 1u);
    EXPECT_EQ(ctx.client_engine->timer_node_count, nodes_before);
    EXPECT_EQ(pair.client->state, KCPMUX_CONN_STATE_CONNECTED);
    auto connect = build_conn_connect(777);
    EXPECT_EQ(kcpmux_engine_input(ctx.client_engine, connect.data(), connect.size(), ctx.server_addr.get()),
        -KCPMUX_ERR_OOM);
    EXPECT_EQ(ctx.client_engine->handshake_map->num, 0);
}

TEST_F(MultiConnectionTest, truncated_handshake_and_invalid_source_or_destination_ids) {
    auto connect = build_conn_connect(0x1122334455667788ULL);
    for (unsigned size = 1; size < connect.size(); ++size) {
        EXPECT_EQ(kcpmux_engine_input(ctx.server_engine, connect.data(), size, ctx.client_addr.get()),
            -KCPMUX_ERR_INVALID_FORMAT);
    }
    write_u64(connect.data() + 1, 1);
    EXPECT_EQ(kcpmux_engine_input(ctx.server_engine, connect.data(), connect.size(), ctx.client_addr.get()),
        -KCPMUX_ERR_INVALID_FORMAT);
    EXPECT_EQ(ctx.server_engine->conn_count, 0u);
    auto *conn = kcpmux_conn_connect(ctx.client_engine, ctx.server_addr.get(), nullptr, nullptr, nullptr, nullptr);
    ASSERT_NE(conn, nullptr);
    auto ack = build_conn_connect_ack(KCPMUX_ACK_RESULT_OK, kcpmux_conn_get_id(conn));
    for (unsigned size = 1; size < ack.size(); ++size) {
        EXPECT_EQ(kcpmux_engine_input(ctx.client_engine, ack.data(), size, ctx.server_addr.get()),
            -KCPMUX_ERR_INVALID_FORMAT);
    }
    write_u64(ack.data() + 9, 0);
    EXPECT_EQ(kcpmux_engine_input(ctx.client_engine, ack.data(), ack.size(), ctx.server_addr.get()),
        -KCPMUX_ERR_INVALID_FORMAT);
    EXPECT_EQ(kcpmux_conn_get_peer_id(conn), 0u);
    EXPECT_EQ(conn->state, KCPMUX_CONN_STATE_CONNECTING);
}

TEST_F(MultiConnectionTest, maximum_mss_produces_exactly_1500_byte_packet) {
    auto pair = connect_pair();
    ASSERT_NE(pair.server, nullptr);
    kcpmux_stream_config_t config;
    kcpmux_stream_config_init(&config);
    config.kcp_mss = 1463;
    auto *stream = kcpmux_stream_create(pair.client, &config, nullptr, nullptr);
    ASSERT_NE(stream, nullptr);
    std::vector<uint8_t> data(config.kcp_mss, 0x5a);
    ctx.client_ctx.sent_packets.clear();
    ASSERT_EQ(kcpmux_stream_send(stream, data.data(), data.size(), 1), data.size());
    ASSERT_EQ(ctx.client_ctx.sent_packets.size(), 1u);
    EXPECT_EQ(ctx.client_ctx.sent_packets[0].size(), 1500u);
    EXPECT_EQ(ctx.client_ctx.sent_iov_fragments.back()[0].size(), KCPMUX_PROTO_STREAM_HEADER_LEN);
}
} // namespace
