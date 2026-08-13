#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <random>
#include <set>
#include <tuple>
#include <vector>

extern "C" {
#include "kcpmux_timer.h"
}

namespace {

struct TimerNode {
    kcpmux_timer_node_t timer;
    int id;
};

static void NoopTimerCallback(kcpmux_timer_node_t *, int64_t)
{
}

static void InitNode(TimerNode *node, int id)
{
    node->id = id;
    kcpmux_timer_node_init(&node->timer, node, NoopTimerCallback);
}

static void ExpectHeapInvariant(const kcpmux_timer_manager_t &manager)
{
    for (size_t i = 0; i < manager.size; ++i) {
        const kcpmux_timer_node_t *node = manager.heap[i];
        ASSERT_NE(node, nullptr);
        EXPECT_EQ(node->heap_index, i);
        EXPECT_EQ(node->state, KCPMUX_TIMER_HEAP);
        if (i == 0) {
            continue;
        }
        const kcpmux_timer_node_t *parent = manager.heap[(i - 1) / 2];
        EXPECT_TRUE(parent->deadline_ms < node->deadline_ms ||
                    (parent->deadline_ms == node->deadline_ms &&
                     parent->insertion_sequence < node->insertion_sequence));
    }
}

static std::vector<int> DrainDue(kcpmux_timer_manager_t *manager, int64_t now_ms) {
    list_head due;
    std::vector<int> result;
    INIT_LIST_HEAD(&due);
    kcpmux_timer_collect_due(manager, now_ms, &due);
    while (!list_empty(&due)) {
        list_head *link = list_dequeue(&due);
        auto *timer = list_entry(link, kcpmux_timer_node_t, due_link);
        auto *node = static_cast<TimerNode *>(timer->owner);
        INIT_LIST_HEAD(&timer->due_link);
        result.push_back(node->id);
        EXPECT_EQ(timer->state, KCPMUX_TIMER_DUE);
        kcpmux_timer_cancel(manager, timer);
    }
    return result;
}

class kcpmux_timer : public ::testing::Test {
protected:
    void SetUp() override { ASSERT_EQ(kcpmux_timer_manager_init(&manager_), KCPMUX_ERR_OK); }
    void TearDown() override { kcpmux_timer_manager_destroy(&manager_); }

    kcpmux_timer_manager_t manager_{};
};

TEST_F(kcpmux_timer, insert_and_peek_preserve_heap_invariant) {
    TimerNode nodes[5];
    const int64_t deadlines[] = {50, 10, 40, 20, 30};
    for (int i = 0; i < 5; ++i) {
        InitNode(&nodes[i], i);
        ASSERT_EQ(kcpmux_timer_schedule(&manager_, &nodes[i].timer, deadlines[i]),
                  KCPMUX_ERR_OK);
        ExpectHeapInvariant(manager_);
    }
    ASSERT_NE(kcpmux_timer_peek(&manager_), nullptr);
    EXPECT_EQ(kcpmux_timer_peek(&manager_)->owner, &nodes[1]);
}

TEST_F(kcpmux_timer, adjusts_deadline_earlier_and_later_without_duplicate_entry) {
    TimerNode first;
    TimerNode second;
    InitNode(&first, 1);
    InitNode(&second, 2);
    ASSERT_EQ(kcpmux_timer_schedule(&manager_, &first.timer, 100), KCPMUX_ERR_OK);
    ASSERT_EQ(kcpmux_timer_schedule(&manager_, &second.timer, 200), KCPMUX_ERR_OK);

    ASSERT_EQ(kcpmux_timer_schedule(&manager_, &second.timer, 50), KCPMUX_ERR_OK);
    EXPECT_EQ(manager_.size, 2u);
    EXPECT_EQ(kcpmux_timer_peek(&manager_), &second.timer);
    ExpectHeapInvariant(manager_);

    ASSERT_EQ(kcpmux_timer_schedule(&manager_, &second.timer, 300), KCPMUX_ERR_OK);
    EXPECT_EQ(manager_.size, 2u);
    EXPECT_EQ(kcpmux_timer_peek(&manager_), &first.timer);
    ExpectHeapInvariant(manager_);
}

TEST_F(kcpmux_timer, removes_root_middle_and_tail_by_index) {
    TimerNode nodes[7];
    for (int i = 0; i < 7; ++i) {
        InitNode(&nodes[i], i);
        ASSERT_EQ(kcpmux_timer_schedule(&manager_, &nodes[i].timer, i * 10), KCPMUX_ERR_OK);
    }

    kcpmux_timer_node_t *root = manager_.heap[0];
    kcpmux_timer_node_t *middle = manager_.heap[2];
    kcpmux_timer_node_t *tail = manager_.heap[manager_.size - 1];
    kcpmux_timer_cancel(&manager_, middle);
    EXPECT_EQ(middle->state, KCPMUX_TIMER_IDLE);
    ExpectHeapInvariant(manager_);
    kcpmux_timer_cancel(&manager_, tail);
    EXPECT_EQ(tail->state, KCPMUX_TIMER_IDLE);
    ExpectHeapInvariant(manager_);
    kcpmux_timer_cancel(&manager_, root);
    EXPECT_EQ(root->state, KCPMUX_TIMER_IDLE);
    ExpectHeapInvariant(manager_);
    EXPECT_EQ(manager_.size, 4u);
}

TEST_F(kcpmux_timer, cancel_is_idempotent_for_heap_due_and_idle_nodes) {
    TimerNode node;
    InitNode(&node, 1);
    kcpmux_timer_cancel(&manager_, &node.timer);
    ASSERT_EQ(kcpmux_timer_schedule(&manager_, &node.timer, 10), KCPMUX_ERR_OK);
    kcpmux_timer_cancel(&manager_, &node.timer);
    kcpmux_timer_cancel(&manager_, &node.timer);
    EXPECT_EQ(manager_.size, 0u);
    EXPECT_EQ(node.timer.state, KCPMUX_TIMER_IDLE);

    ASSERT_EQ(kcpmux_timer_schedule(&manager_, &node.timer, 10), KCPMUX_ERR_OK);
    list_head due;
    INIT_LIST_HEAD(&due);
    kcpmux_timer_collect_due(&manager_, 10, &due);
    ASSERT_EQ(node.timer.state, KCPMUX_TIMER_DUE);
    kcpmux_timer_cancel(&manager_, &node.timer);
    kcpmux_timer_cancel(&manager_, &node.timer);
    EXPECT_TRUE(list_empty(&due));
    EXPECT_EQ(node.timer.state, KCPMUX_TIMER_IDLE);
}

TEST_F(kcpmux_timer, equal_deadlines_follow_stable_insertion_order) {
    TimerNode nodes[4];
    for (int i = 0; i < 4; ++i) {
        InitNode(&nodes[i], i);
        ASSERT_EQ(kcpmux_timer_schedule(&manager_, &nodes[i].timer, 100), KCPMUX_ERR_OK);
    }
    EXPECT_EQ(DrainDue(&manager_, 100), (std::vector<int>{0, 1, 2, 3}));
}

TEST_F(kcpmux_timer, collect_due_stops_at_first_future_deadline) {
    TimerNode nodes[5];
    const int64_t deadlines[] = {80, 110, 90, 120, 100};
    for (int i = 0; i < 5; ++i) {
        InitNode(&nodes[i], i);
        ASSERT_EQ(kcpmux_timer_schedule(&manager_, &nodes[i].timer, deadlines[i]),
                  KCPMUX_ERR_OK);
    }
    EXPECT_EQ(DrainDue(&manager_, 100), (std::vector<int>{0, 2, 4}));
    ASSERT_NE(kcpmux_timer_peek(&manager_), nullptr);
    EXPECT_EQ(kcpmux_timer_peek(&manager_)->deadline_ms, 110);
    EXPECT_EQ(manager_.size, 2u);
    ExpectHeapInvariant(manager_);
}

TEST_F(kcpmux_timer, due_node_can_be_rescheduled_out_of_snapshot) {
    TimerNode node;
    InitNode(&node, 1);
    ASSERT_EQ(kcpmux_timer_schedule(&manager_, &node.timer, 10), KCPMUX_ERR_OK);
    list_head due;
    INIT_LIST_HEAD(&due);
    kcpmux_timer_collect_due(&manager_, 10, &due);
    ASSERT_EQ(node.timer.state, KCPMUX_TIMER_DUE);

    ASSERT_EQ(kcpmux_timer_schedule(&manager_, &node.timer, 20), KCPMUX_ERR_OK);
    EXPECT_TRUE(list_empty(&due));
    EXPECT_EQ(node.timer.state, KCPMUX_TIMER_HEAP);
    EXPECT_EQ(kcpmux_timer_peek(&manager_), &node.timer);
}

TEST_F(kcpmux_timer, random_operations_match_ordered_reference_model) {
    constexpr int kNodeCount = 64;
    constexpr int kOperationCount = 10000;
    TimerNode nodes[kNodeCount];
    std::vector<bool> scheduled(kNodeCount, false);
    std::vector<int64_t> model_deadline(kNodeCount, 0);
    std::vector<uint64_t> model_sequence(kNodeCount, 0);
    uint64_t model_next_sequence = 0;
    std::mt19937 rng(0x4d504b43u);
    std::uniform_int_distribution<int> node_dist(0, kNodeCount - 1);
    std::uniform_int_distribution<int64_t> deadline_dist(-100, 1000);
    std::uniform_int_distribution<int> operation_dist(0, 9);

    for (int i = 0; i < kNodeCount; ++i) {
        InitNode(&nodes[i], i);
    }

    for (int operation = 0; operation < kOperationCount; ++operation) {
        int selected = node_dist(rng);
        int choice = operation_dist(rng);
        if (choice < 6) {
            int64_t deadline = deadline_dist(rng);
            if (!scheduled[selected]) {
                model_sequence[selected] = model_next_sequence++;
            }
            model_deadline[selected] = deadline;
            ASSERT_EQ(kcpmux_timer_schedule(
                          &manager_, &nodes[selected].timer, deadline),
                      KCPMUX_ERR_OK);
            scheduled[selected] = true;
        } else if (choice < 8) {
            kcpmux_timer_cancel(&manager_, &nodes[selected].timer);
            scheduled[selected] = false;
        } else {
            int64_t now = deadline_dist(rng);
            std::vector<int> expected;
            std::multiset<std::tuple<int64_t, uint64_t, int>> reference;
            for (int i = 0; i < kNodeCount; ++i) {
                if (scheduled[i]) {
                    reference.emplace(model_deadline[i], model_sequence[i], i);
                }
            }
            for (const auto &[deadline, sequence, id] : reference) {
                (void)sequence;
                if (deadline > now) {
                    break;
                }
                expected.push_back(id);
            }
            EXPECT_EQ(DrainDue(&manager_, now), expected);
            for (int id : expected) {
                scheduled[id] = false;
            }
        }

        size_t expected_size =
            static_cast<size_t>(std::count(scheduled.begin(), scheduled.end(), true));
        ASSERT_EQ(manager_.size, expected_size);
        ExpectHeapInvariant(manager_);
        if (expected_size > 0) {
            std::multiset<std::tuple<int64_t, uint64_t, int>> reference;
            for (int i = 0; i < kNodeCount; ++i) {
                if (scheduled[i]) {
                    reference.emplace(model_deadline[i], model_sequence[i], i);
                }
            }
            int expected_root = std::get<2>(*reference.begin());
            EXPECT_EQ(kcpmux_timer_peek(&manager_), &nodes[expected_root].timer);
        }
    }
}

}  // namespace
