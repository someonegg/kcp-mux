#include "kcpmux_e2e_utils.h"

using namespace kcpmux_e2e;

// ============================================================================
// Test Fixture
// ============================================================================

class kcpmux_e2e_basic : public ::testing::Test {
protected:
    void SetUp() override { ctx.setup(); }
    void TearDown() override { ctx.teardown(); }

    E2EContext ctx;
};

// ============================================================================
// Connection Tests
// ============================================================================

TEST_F(kcpmux_e2e_basic, connect_success) {
    // Client connects to server
    ctx.client.connect_to(ctx.client.peer_addr);

    // Wait for both sides to be CONNECTED
    ASSERT_TRUE(ctx.wait_conn_state(ctx.client, KCPMUX_CONN_STATE_CONNECTED, 3000));
    ASSERT_TRUE(ctx.wait_conn_state(ctx.server, KCPMUX_CONN_STATE_CONNECTED, 3000));

    // Verify server accepted the connection
    {
        std::lock_guard<std::mutex> lock(ctx.server.mutex);
        EXPECT_EQ(ctx.server.accepted_conns.size(), 1u);
    }
}

TEST_F(kcpmux_e2e_basic, connect_with_extension) {
    // Prepare extension data
    std::string client_ext = "client-hello-ext";
    std::string server_resp_ext = "server-resp-ext";

    // Configure server to respond with extension
    ctx.server.conn_proto_ext_response.assign(server_resp_ext.begin(), server_resp_ext.end());

    // Client connects with extension data
    ctx.client.connect_to_with_ext(ctx.client.peer_addr,
                                   (const uint8_t *)client_ext.c_str(), client_ext.size());

    // Wait for both sides to be CONNECTED
    ASSERT_TRUE(ctx.wait_conn_state(ctx.client, KCPMUX_CONN_STATE_CONNECTED, 3000));
    ASSERT_TRUE(ctx.wait_conn_state(ctx.server, KCPMUX_CONN_STATE_CONNECTED, 3000));

    // Verify server received client's extension
    auto server_received = ctx.server.get_conn_proto_ext_received();
    EXPECT_EQ(server_received.size(), client_ext.size());
    EXPECT_EQ(std::string(server_received.begin(), server_received.end()), client_ext);

    // Verify client received server's response extension via kcpmux_conn_get_peer_proto_ext
    ASSERT_TRUE(ctx.wait_until([&]() { return ctx.client.conn != nullptr; }, 1000));
    const kcpmux_proto_ext_t *client_received = kcpmux_conn_get_peer_proto_ext(ctx.client.conn);
    ASSERT_NE(client_received, nullptr);
    EXPECT_EQ(client_received->len, server_resp_ext.size());
    EXPECT_EQ(std::string((char *)client_received->data, client_received->len), server_resp_ext);

    // Verify client self extension
    const kcpmux_proto_ext_t *client_self = kcpmux_conn_get_self_proto_ext(ctx.client.conn);
    ASSERT_NE(client_self, nullptr);
    EXPECT_EQ(client_self->len, client_ext.size());
    EXPECT_EQ(std::string((char *)client_self->data, client_self->len), client_ext);

    // Verify server peer and self extensions
    ASSERT_TRUE(ctx.wait_until([&]() { return ctx.server.conn != nullptr; }, 1000));
    const kcpmux_proto_ext_t *server_peer = kcpmux_conn_get_peer_proto_ext(ctx.server.conn);
    ASSERT_NE(server_peer, nullptr);
    EXPECT_EQ(server_peer->len, client_ext.size());
    EXPECT_EQ(std::string((char *)server_peer->data, server_peer->len), client_ext);

    const kcpmux_proto_ext_t *server_self = kcpmux_conn_get_self_proto_ext(ctx.server.conn);
    ASSERT_NE(server_self, nullptr);
    EXPECT_EQ(server_self->len, server_resp_ext.size());
    EXPECT_EQ(std::string((char *)server_self->data, server_self->len), server_resp_ext);
}

TEST_F(kcpmux_e2e_basic, connect_rejected) {
    // Configure server to reject connections
    ctx.server.conn_notify_result = KCPMUX_ACK_RESULT_ERROR;

    // Client connects to server
    ctx.client.connect_to(ctx.client.peer_addr);

    // Wait for client to receive rejection (state becomes CLOSED or ERROR)
    ASSERT_TRUE(ctx.wait_until([&]() {
        int state = ctx.client.conn_state.load();
        return state == KCPMUX_CONN_STATE_CLOSED || state == KCPMUX_CONN_STATE_ERROR;
    }, 3000));
}

// ============================================================================
// Close Tests
// ============================================================================

TEST_F(kcpmux_e2e_basic, close_client_initiated) {
    // Establish connection
    ctx.client.connect_to(ctx.client.peer_addr);
    ASSERT_TRUE(ctx.wait_conn_state(ctx.client, KCPMUX_CONN_STATE_CONNECTED, 3000));
    ASSERT_TRUE(ctx.wait_conn_state(ctx.server, KCPMUX_CONN_STATE_CONNECTED, 3000));

    // Client closes connection
    ctx.client.close_conn();

    // Wait for client to enter CLOSING state
    ASSERT_TRUE(ctx.wait_conn_state(ctx.client, KCPMUX_CONN_STATE_CLOSING, 3000));
}

TEST_F(kcpmux_e2e_basic, close_server_initiated) {
    // Establish connection
    ctx.client.connect_to(ctx.client.peer_addr);
    ASSERT_TRUE(ctx.wait_conn_state(ctx.client, KCPMUX_CONN_STATE_CONNECTED, 3000));
    ASSERT_TRUE(ctx.wait_conn_state(ctx.server, KCPMUX_CONN_STATE_CONNECTED, 3000));

    // Server closes connection
    ctx.server.close_conn();

    // Wait for server to enter CLOSING state
    ASSERT_TRUE(ctx.wait_conn_state(ctx.server, KCPMUX_CONN_STATE_CLOSING, 3000));
}

// ============================================================================
// Data Transfer Tests
// ============================================================================

TEST_F(kcpmux_e2e_basic, send_recv_small_data) {
    // Establish connection
    ctx.client.connect_to(ctx.client.peer_addr);
    ASSERT_TRUE(ctx.wait_conn_state(ctx.client, KCPMUX_CONN_STATE_CONNECTED, 3000));
    ASSERT_TRUE(ctx.wait_conn_state(ctx.server, KCPMUX_CONN_STATE_CONNECTED, 3000));

    // Create stream
    ctx.client.create_stream();
    ASSERT_TRUE(ctx.wait_stream_state(ctx.client, KCPMUX_STREAM_STATE_OPEN, 3000));

    // Get stream ID
    uint32_t stream_id = 0;
    ASSERT_TRUE(ctx.wait_until([&]() {
        if (ctx.client.stream) {
            stream_id = kcpmux_stream_id(ctx.client.stream);
            return true;
        }
        return false;
    }, 1000));

    // First payload triggers server-side auto-create/open.
    uint8_t bootstrap = 0x7f;
    ctx.client.send_data(&bootstrap, 1);
    ASSERT_TRUE(ctx.wait_stream_state(ctx.server, KCPMUX_STREAM_STATE_OPEN, 3000));
    ASSERT_TRUE(ctx.wait_data_received(ctx.server, stream_id, 1, 3000));

    // Send data
    std::string test_data = "Hello, E2E Test!";
    ctx.client.send_data((const uint8_t *)test_data.c_str(), test_data.size());

    // Wait for server to receive data
    ASSERT_TRUE(ctx.wait_data_received(ctx.server, stream_id, test_data.size() + 1, 3000));

    // Verify data
    uint8_t recv_buf[256];
    size_t recv_len = ctx.server.get_received_data(stream_id, recv_buf, sizeof(recv_buf));
    EXPECT_EQ(recv_len, test_data.size() + 1);
    EXPECT_EQ(std::string((char *)recv_buf + 1, recv_len - 1), test_data);
}

TEST_F(kcpmux_e2e_basic, send_recv_bidirectional) {
    // Establish connection
    ctx.client.connect_to(ctx.client.peer_addr);
    ASSERT_TRUE(ctx.wait_conn_state(ctx.client, KCPMUX_CONN_STATE_CONNECTED, 3000));
    ASSERT_TRUE(ctx.wait_conn_state(ctx.server, KCPMUX_CONN_STATE_CONNECTED, 3000));

    // Create stream
    ctx.client.create_stream();
    ASSERT_TRUE(ctx.wait_stream_state(ctx.client, KCPMUX_STREAM_STATE_OPEN, 3000));

    // Get stream ID
    uint32_t stream_id = 0;
    ASSERT_TRUE(ctx.wait_until([&]() {
        if (ctx.client.stream) {
            stream_id = kcpmux_stream_id(ctx.client.stream);
            return true;
        }
        return false;
    }, 1000));

    // Bootstrap payload to create server-side stream.
    uint8_t bootstrap = 0x7f;
    ctx.client.send_data(&bootstrap, 1);
    ASSERT_TRUE(ctx.wait_stream_state(ctx.server, KCPMUX_STREAM_STATE_OPEN, 3000));
    ASSERT_TRUE(ctx.wait_data_received(ctx.server, stream_id, 1, 3000));

    // Send data from both directions
    std::string client_data = "Client->Server";
    std::string server_data = "Server->Client";

    ctx.client.send_data((const uint8_t *)client_data.c_str(), client_data.size());
    ctx.server.send_data((const uint8_t *)server_data.c_str(), server_data.size());

    // Wait for both sides to receive data
    ASSERT_TRUE(ctx.wait_data_received(ctx.server, stream_id, client_data.size() + 1, 3000));
    ASSERT_TRUE(ctx.wait_data_received(ctx.client, stream_id, server_data.size(), 3000));

    // Verify server received client's data
    uint8_t server_recv[256];
    size_t server_recv_len = ctx.server.get_received_data(stream_id, server_recv, sizeof(server_recv));
    EXPECT_EQ(std::string((char *)server_recv + 1, server_recv_len - 1), client_data);

    // Verify client received server's data
    uint8_t client_recv[256];
    size_t client_recv_len = ctx.client.get_received_data(stream_id, client_recv, sizeof(client_recv));
    EXPECT_EQ(std::string((char *)client_recv, client_recv_len), server_data);
}
