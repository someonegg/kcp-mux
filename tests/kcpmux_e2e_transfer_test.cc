#include "kcpmux_e2e_utils.h"
#include <numeric>
#include <random>

static uint32_t kcpmux_test_checksum(const uint8_t *data, size_t size, uint32_t seed)
{
    uint32_t hash = seed ? seed : 2166136261u;
    for (size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 16777619u;
    }
    return hash;
}

using namespace kcpmux_e2e;

// ============================================================================
// Test Fixture
// ============================================================================

class kcpmux_e2e_transfer : public ::testing::Test {
protected:
    void SetUp() override {
        ctx.setup();
        // Establish connection
        ctx.client.connect_to(ctx.client.peer_addr);
        ASSERT_TRUE(ctx.wait_conn_state(ctx.client, KCPMUX_CONN_STATE_CONNECTED, 3000));
        ASSERT_TRUE(ctx.wait_conn_state(ctx.server, KCPMUX_CONN_STATE_CONNECTED, 3000));

        // Create stream
        ctx.client.create_stream();
        ASSERT_TRUE(ctx.wait_stream_state(ctx.client, KCPMUX_STREAM_STATE_OPEN, 3000));
        kcpmux_stream_t *stream = ctx.client.get_primary_stream();
        ASSERT_NE(stream, nullptr);
        stream_id = kcpmux_stream_id(stream);

        // Bootstrap payload to create server-side stream.
        uint8_t bootstrap = 0x7f;
        ctx.client.send_data(&bootstrap, 1);
        ASSERT_TRUE(ctx.wait_stream_state(ctx.server, KCPMUX_STREAM_STATE_OPEN, 3000));
        ASSERT_TRUE(ctx.wait_data_received(ctx.server, stream_id, 1, 3000));

        uint8_t recvboot = 0;
        ctx.server.get_received_data(stream_id, &recvboot, 1);
        ASSERT_TRUE(recvboot == bootstrap);

        ctx.server.clear_received_data(stream_id);
    }

    void TearDown() override { ctx.teardown(); }

    // Generate test data with pattern
    std::vector<uint8_t> generate_test_data(size_t size) {
        std::vector<uint8_t> data(size);
        for (size_t i = 0; i < size; i++) {
            data[i] = (uint8_t)(i & 0xff);
        }
        return data;
    }

    E2EContext ctx;
    uint32_t stream_id = 0;
};

TEST_F(kcpmux_e2e_transfer, transfer_concurrent_streams) {
    const size_t DATA_SIZE = 128 * 1024;  // 128 KB per stream
    const int NUM_STREAMS = 8;

    // Create additional streams
    std::vector<uint32_t> stream_ids;
    std::vector<std::vector<uint8_t>> send_data_list;
    std::vector<uint32_t> send_crcs;

    // First stream already created in SetUp
    stream_ids.push_back(stream_id);

    // Create more streams
    for (int i = 1; i < NUM_STREAMS; i++) {
        ctx.client.reset_primary_stream();
        ctx.client.create_stream();
        ASSERT_TRUE(ctx.wait_stream_state(ctx.client, KCPMUX_STREAM_STATE_OPEN, 3000));
        kcpmux_stream_t *stream = ctx.client.get_primary_stream();
        ASSERT_NE(stream, nullptr);
        stream_ids.push_back(kcpmux_stream_id(stream));
    }

    // Generate unique data for each stream
    for (int i = 0; i < NUM_STREAMS; i++) {
        std::vector<uint8_t> data(DATA_SIZE);
        for (size_t j = 0; j < DATA_SIZE; j++) {
            data[j] = (uint8_t)((i * 37 + j) & 0xff);  // Different pattern per stream
        }
        send_data_list.push_back(data);
        send_crcs.push_back(kcpmux_test_checksum(data.data(), data.size(), 0));
    }

    // Queue all streams before waiting so their KCP sessions progress together.
    for (int i = 0; i < NUM_STREAMS; i++) {
        ctx.client.send_data_on_stream_with_retry(
            stream_ids[i],
            send_data_list[i].data(),
            send_data_list[i].size());
    }

    ASSERT_TRUE(ctx.wait_pending_send_complete(ctx.client, 15000));

    // Wait for all streams to receive data
    for (int i = 0; i < NUM_STREAMS; i++) {
        ASSERT_TRUE(ctx.wait_data_received(ctx.server, stream_ids[i], DATA_SIZE, 15000))
            << "Stream " << i << " did not receive all data";
    }

    // Verify each stream's data integrity
    for (int i = 0; i < NUM_STREAMS; i++) {
        std::vector<uint8_t> recv_data(DATA_SIZE);
        size_t recv_len = ctx.server.get_received_data(stream_ids[i], recv_data.data(), DATA_SIZE);
        ASSERT_EQ(recv_len, DATA_SIZE) << "Stream " << i << " received wrong size";

        uint32_t recv_crc = kcpmux_test_checksum(recv_data.data(), recv_len, 0);
        EXPECT_EQ(recv_crc, send_crcs[i]) << "Stream " << i << " CRC mismatch";
    }
}

TEST_F(kcpmux_e2e_transfer, write_block_and_resume) {
    // This test verifies that write blocking and resuming works correctly.
    // When send buffer is full, kcpmux_stream_send returns 0 and data should be
    // buffered until stream_write_notify callback is triggered.
    const size_t TOTAL_SIZE = 1024 * 1024;  // 1 MB
    std::vector<uint8_t> send_data = generate_test_data(TOTAL_SIZE);
    uint32_t send_crc = kcpmux_test_checksum(send_data.data(), send_data.size(), 0);

    // Send data using send_data_with_retry which handles write block correctly
    ctx.client.send_data_with_retry(send_data.data(), send_data.size());

    // Wait for pending send to complete
    ASSERT_TRUE(ctx.wait_pending_send_complete(ctx.client, 30000));

    // Wait for all data to be received
    ASSERT_TRUE(ctx.wait_data_received(ctx.server, stream_id, TOTAL_SIZE, 30000));

    // Verify size and data integrity
    std::vector<uint8_t> recv_data(TOTAL_SIZE);
    size_t recv_len = ctx.server.get_received_data(stream_id, recv_data.data(), TOTAL_SIZE);
    EXPECT_EQ(recv_len, TOTAL_SIZE);

    uint32_t recv_crc = kcpmux_test_checksum(recv_data.data(), recv_len, 0);
    EXPECT_EQ(recv_crc, send_crc);
}
