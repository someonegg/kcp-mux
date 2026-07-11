#include "kcpmux_e2e_utils.h"

using namespace kcpmux_e2e;

// ============================================================================
// Test Fixture
// ============================================================================

class kcpmux_e2e_stream : public ::testing::Test {
protected:
    void SetUp() override {
        ctx.setup();
        // Establish connection for all stream tests
        ctx.client.connect_to(ctx.client.peer_addr);
        ASSERT_TRUE(ctx.wait_conn_state(ctx.client, KCPMUX_CONN_STATE_CONNECTED, 3000));
        ASSERT_TRUE(ctx.wait_conn_state(ctx.server, KCPMUX_CONN_STATE_CONNECTED, 3000));
    }

    void TearDown() override { ctx.teardown(); }

    E2EContext ctx;
};

// ============================================================================
// Multi-Stream Tests
// ============================================================================

TEST_F(kcpmux_e2e_stream, create_multiple_streams) {
    const int NUM_STREAMS = 5;

    // Create multiple streams from client
    for (int i = 0; i < NUM_STREAMS; i++) {
        ctx.client.create_stream();
        ASSERT_TRUE(ctx.wait_stream_state(ctx.client, KCPMUX_STREAM_STATE_OPEN, 3000));
        uint8_t bootstrap = (uint8_t)(0x80 + i);
        ctx.client.send_data(&bootstrap, 1);
        // Small delay between stream creation
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // Wait for server to accept all streams
    ASSERT_TRUE(ctx.wait_until([&]() {
        std::lock_guard<std::mutex> lock(ctx.server.mutex);
        return ctx.server.accepted_streams.size() >= NUM_STREAMS;
    }, 5000));

    // Verify all streams are created
    {
        std::lock_guard<std::mutex> lock(ctx.server.mutex);
        EXPECT_EQ(ctx.server.accepted_streams.size(), NUM_STREAMS);
    }
}

TEST_F(kcpmux_e2e_stream, independent_data_per_stream) {
    // Create first stream
    ctx.client.create_stream();
    ASSERT_TRUE(ctx.wait_stream_state(ctx.client, KCPMUX_STREAM_STATE_OPEN, 3000));

    kcpmux_stream_t *client_stream1 = ctx.client.stream;
    ASSERT_NE(client_stream1, nullptr);
    uint32_t stream_id1 = kcpmux_stream_id(client_stream1);
    uint8_t bootstrap1 = 0x11;
    ctx.client.send_data(&bootstrap1, 1);

    // Wait for server to accept first stream
    ASSERT_TRUE(ctx.wait_until([&]() {
        std::lock_guard<std::mutex> lock(ctx.server.mutex);
        return ctx.server.accepted_streams.size() >= 1;
    }, 3000));

    // Reset for second stream
    ctx.client.stream = nullptr;
    ctx.client.stream_state.store(-1);

    // Create second stream
    ctx.client.create_stream();
    ASSERT_TRUE(ctx.wait_stream_state(ctx.client, KCPMUX_STREAM_STATE_OPEN, 3000));

    kcpmux_stream_t *client_stream2 = ctx.client.stream;
    ASSERT_NE(client_stream2, nullptr);
    uint32_t stream_id2 = kcpmux_stream_id(client_stream2);
    uint8_t bootstrap2 = 0x22;
    ctx.client.send_data(&bootstrap2, 1);

    ASSERT_NE(stream_id1, stream_id2);

    // Wait for server to accept second stream
    ASSERT_TRUE(ctx.wait_until([&]() {
        std::lock_guard<std::mutex> lock(ctx.server.mutex);
        return ctx.server.accepted_streams.size() >= 2;
    }, 3000));

    // Give streams time to fully establish
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Send different data on each stream
    std::string data1 = "Stream1-Data";
    std::string data2 = "Stream2-Data";

    // Use queue_action to send on specific streams
    ctx.client.queue_action([client_stream1, &data1]() {
        kcpmux_stream_send(client_stream1, (const uint8_t *)data1.c_str(), data1.size(), 1);
    });
    ctx.client.queue_action([client_stream2, &data2]() {
        kcpmux_stream_send(client_stream2, (const uint8_t *)data2.c_str(), data2.size(), 1);
    });

    // Wait for data on both streams
    ASSERT_TRUE(ctx.wait_data_received(ctx.server, stream_id1, data1.size() + 1, 3000));
    ASSERT_TRUE(ctx.wait_data_received(ctx.server, stream_id2, data2.size() + 1, 3000));

    // Verify isolation
    uint8_t recv1[256], recv2[256];
    size_t len1 = ctx.server.get_received_data(stream_id1, recv1, sizeof(recv1));
    size_t len2 = ctx.server.get_received_data(stream_id2, recv2, sizeof(recv2));

    EXPECT_EQ(std::string((char *)recv1 + 1, len1 - 1), data1);
    EXPECT_EQ(std::string((char *)recv2 + 1, len2 - 1), data2);
}

TEST_F(kcpmux_e2e_stream, close_one_stream_others_unaffected) {
    // Create two streams
    ctx.client.create_stream();
    ASSERT_TRUE(ctx.wait_stream_state(ctx.client, KCPMUX_STREAM_STATE_OPEN, 3000));

    kcpmux_stream_t *client_stream1 = ctx.client.stream;
    (void)client_stream1;  // Used for closing, not for data transfer
    uint8_t bootstrap1 = 0x31;
    ctx.client.send_data(&bootstrap1, 1);

    // Wait for server to accept first stream
    ASSERT_TRUE(ctx.wait_until([&]() {
        std::lock_guard<std::mutex> lock(ctx.server.mutex);
        return ctx.server.accepted_streams.size() >= 1;
    }, 3000));

    ctx.client.stream = nullptr;
    ctx.client.stream_state.store(-1);

    ctx.client.create_stream();
    ASSERT_TRUE(ctx.wait_stream_state(ctx.client, KCPMUX_STREAM_STATE_OPEN, 3000));

    kcpmux_stream_t *client_stream2 = ctx.client.stream;
    uint32_t stream_id2 = kcpmux_stream_id(client_stream2);
    uint8_t bootstrap2 = 0x32;
    ctx.client.send_data(&bootstrap2, 1);

    // Wait for server to accept second stream
    ASSERT_TRUE(ctx.wait_until([&]() {
        std::lock_guard<std::mutex> lock(ctx.server.mutex);
        return ctx.server.accepted_streams.size() >= 2;
    }, 3000));

    // Give streams time to fully establish
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Close first stream
    ctx.client.queue_action([client_stream1]() { kcpmux_stream_close(client_stream1); });

    // Wait for stream1 to close
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Second stream should still work
    std::string test_data = "Stream2-Still-Works";
    ctx.client.queue_action([client_stream2, &test_data]() {
        kcpmux_stream_send(client_stream2, (const uint8_t *)test_data.c_str(), test_data.size(), 1);
    });

    ASSERT_TRUE(ctx.wait_data_received(ctx.server, stream_id2, test_data.size() + 1, 3000));

    uint8_t recv[256];
    size_t len = ctx.server.get_received_data(stream_id2, recv, sizeof(recv));
    EXPECT_EQ(std::string((char *)recv + 1, len - 1), test_data);
}

TEST_F(kcpmux_e2e_stream, server_creates_stream) {
    // Server creates stream
    ctx.server.create_stream();
    ASSERT_TRUE(ctx.wait_stream_state(ctx.server, KCPMUX_STREAM_STATE_OPEN, 3000));
    uint8_t bootstrap = 0x55;
    ctx.server.send_data(&bootstrap, 1);

    // Client should accept the stream
    ASSERT_TRUE(ctx.wait_until([&]() {
        std::lock_guard<std::mutex> lock(ctx.client.mutex);
        return ctx.client.accepted_streams.size() >= 1;
    }, 3000));

    // Get stream ID
    uint32_t stream_id = kcpmux_stream_id(ctx.server.stream);

    // Server sends data
    std::string test_data = "Server-Created-Stream";
    ctx.server.send_data((const uint8_t *)test_data.c_str(), test_data.size());

    // Client receives
    ASSERT_TRUE(ctx.wait_data_received(ctx.client, stream_id, test_data.size() + 1, 3000));

    uint8_t recv[256];
    size_t len = ctx.client.get_received_data(stream_id, recv, sizeof(recv));
    EXPECT_EQ(std::string((char *)recv + 1, len - 1), test_data);
}
