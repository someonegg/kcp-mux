#include "kcpmux_e2e_utils.h"
#include <memory>
extern "C" {
#include "kcpmux_conn.h"
}

using namespace kcpmux_e2e;

namespace {
class MultiConnectionUdpTest : public ::testing::Test {
protected:
    void SetUp() override { ctx.setup(); }
    void TearDown() override { ctx.teardown(); }
    E2EContext ctx;

    bool wait_connections(E2EEndpoint &ep, size_t count) {
        return ctx.wait_until([&]() {
            std::lock_guard<std::mutex> lock(ep.mutex);
            if (ep.active_conns.size() != count) return false;
            for (auto &item : ep.active_conns) {
                auto state = ep.connection_states.find(item.first);
                if (state == ep.connection_states.end() || state->second != KCPMUX_CONN_STATE_CONNECTED)
                    return false;
            }
            return true;
        });
    }
};

TEST_F(MultiConnectionUdpTest, one_socket_pair_multiple_connections_with_equal_stream_ids) {
    // All connections use exactly the same two sockets and endpoint addresses.
    for (size_t count = 1; count <= 3; ++count) {
        ctx.client.connect_to(ctx.client.peer_addr);
        ASSERT_TRUE(wait_connections(ctx.client, count));
        ASSERT_TRUE(wait_connections(ctx.server, count));
    }
    struct CreatedStreams {
        std::vector<StreamKey> client_keys, server_keys;
        std::atomic<bool> ready{false};
    };
    auto state = std::make_shared<CreatedStreams>();
    ctx.client.queue_action([this, state]() {
        std::vector<kcpmux_conn_t *> connections;
        {
            std::lock_guard<std::mutex> lock(ctx.client.mutex);
            for (auto &item : ctx.client.active_conns) connections.push_back(item.second);
        }
        for (auto *connection : connections) {
            connection->next_stream_id = 1; // Force the collision across connections.
            kcpmux_stream_callbacks_t callbacks{};
            callbacks.stream_read_notify = e2e_stream_read_notify;
            callbacks.stream_write_notify = e2e_stream_write_notify;
            callbacks.stream_close_notify = e2e_stream_close_notify;
            auto *stream = kcpmux_stream_create(connection, nullptr, &callbacks, &ctx.client);
            if (!stream) continue;
            StreamKey key = stream_key(stream);
            state->client_keys.push_back(key);
            state->server_keys.emplace_back(kcpmux_conn_get_peer_id(connection), key.second);
            std::lock_guard<std::mutex> lock(ctx.client.mutex);
            ctx.client.active_streams[key] = stream;
        }
        state->ready.store(true);
    });
    ASSERT_TRUE(ctx.wait_until([&]() { return state->ready.load(); }));
    const auto &client_keys = state->client_keys;
    const auto &server_keys = state->server_keys;
    ASSERT_EQ(client_keys.size(), 3u);
    std::vector<std::vector<uint8_t>> messages;
    for (size_t i = 0; i < client_keys.size(); ++i) {
        EXPECT_EQ(client_keys[i].second, 1u);
        messages.emplace_back(8192 + i * 17, static_cast<uint8_t>('A' + i));
        ctx.client.send_data_on_stream_with_retry(client_keys[i], messages[i].data(), messages[i].size());
    }
    for (size_t i = 0; i < server_keys.size(); ++i) {
        ASSERT_TRUE(ctx.wait_until([&]() {
            return ctx.server.get_received_data_size(server_keys[i]) == messages[i].size();
        }));
        std::vector<uint8_t> received(messages[i].size());
        ASSERT_EQ(ctx.server.get_received_data(server_keys[i], received.data(), received.size()), received.size());
        EXPECT_EQ(received, messages[i]);
        // Reverse traffic also uses the same stream ID on every connection.
        ctx.server.send_data_on_stream_with_retry(server_keys[i], messages[i].data(), messages[i].size());
    }
    for (size_t i = 0; i < client_keys.size(); ++i) {
        ASSERT_TRUE(ctx.wait_until([&]() {
            return ctx.client.get_received_data_size(client_keys[i]) == messages[i].size();
        }));
        std::vector<uint8_t> received(messages[i].size());
        ASSERT_EQ(ctx.client.get_received_data(client_keys[i], received.data(), received.size()), received.size());
        EXPECT_EQ(received, messages[i]);
    }
    ctx.client.queue_action([this, id = client_keys[0].first]() {
        kcpmux_conn_t *connection;
        {
            std::lock_guard<std::mutex> lock(ctx.client.mutex);
            connection = ctx.client.active_conns.at(id);
        }
        kcpmux_conn_close(connection);
    });
    ASSERT_TRUE(wait_connections(ctx.client, 2));
    ASSERT_TRUE(wait_connections(ctx.server, 2));
    const uint8_t tail[] = {'t', 'a', 'i', 'l'};
    ctx.client.send_data_on_stream_with_retry(client_keys[1], tail, sizeof(tail));
    ASSERT_TRUE(ctx.wait_until([&]() {
        return ctx.server.get_received_data_size(server_keys[1]) == messages[1].size() + sizeof(tail);
    }));
    std::vector<uint8_t> received(messages[1].size() + sizeof(tail));
    ctx.server.get_received_data(server_keys[1], received.data(), received.size());
    EXPECT_EQ(memcmp(received.data() + messages[1].size(), tail, sizeof(tail)), 0);
}

TEST_F(MultiConnectionUdpTest, simultaneous_bidirectional_connects_share_one_socket_pair) {
    ctx.client.connect_to(ctx.client.peer_addr);
    ctx.server.connect_to(ctx.server.peer_addr);
    ASSERT_TRUE(wait_connections(ctx.client, 2));
    ASSERT_TRUE(wait_connections(ctx.server, 2));
    {
        std::lock_guard<std::mutex> lock(ctx.client.mutex);
        EXPECT_EQ(ctx.client.accepted_conns.size(), 1u);
    }
    {
        std::lock_guard<std::mutex> lock(ctx.server.mutex);
        EXPECT_EQ(ctx.server.accepted_conns.size(), 1u);
    }
}
} // namespace
