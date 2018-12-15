#include "test.h"

struct mock_graph : tw::graph<int> {
    std::shared_ptr<tw::task<int>> task = tw::make_task(tw::root, []{ return 42; });
    const std::shared_ptr<tw::task<int>>& final_task() const override {
        return task;
    }
};

std::shared_ptr<mock_graph> make_mock_graph() {
    return std::make_shared<mock_graph>();
}

TEST_CASE("graph_pool_constructor") {
    tw::graph_pool<mock_graph> pool(make_mock_graph, 3, 100);
    REQUIRE(3 == pool.size());
    REQUIRE(3 == pool.idle_count());
    REQUIRE(0 == pool.busy_count());
    REQUIRE(3 == pool.minimum_size());
    REQUIRE(100 == pool.maximum_size());
}

TEST_CASE("graph_pool_constructor_overload") {
    tw::graph_pool<mock_graph> pool(make_mock_graph, 3, 5);
    REQUIRE(3 == pool.size());
    REQUIRE(3 == pool.idle_count());
    REQUIRE(0 == pool.busy_count());
    REQUIRE(3 == pool.minimum_size());
    REQUIRE(5 == pool.maximum_size());
}

TEST_CASE("graph_pool_constructor_throws_for_invalid_minimum") {
    auto lambda = []{ tw::graph_pool<mock_graph>{make_mock_graph, 0, 100}; };
    REQUIRE_THROWS_AS(lambda(), tw::invalid_parameter);
}

TEST_CASE("graph_pool_constructor_throws_for_invalid_minimum_maximum") {
    auto lambda = []{ tw::graph_pool<mock_graph>{make_mock_graph, 3, 2}; };
    REQUIRE_THROWS_AS(lambda(), tw::invalid_parameter);
}

TEST_CASE("graph_pool_next_idle_graph") {
    tw::graph_pool<mock_graph> pool(make_mock_graph, 2, 100);
    REQUIRE(2 == pool.size());
    auto g1 = pool.next_idle_graph();
    REQUIRE(g1);
    REQUIRE(1 == pool.idle_count());
    REQUIRE(1 == pool.busy_count());
    auto g2 = pool.next_idle_graph();
    REQUIRE(g2);
    REQUIRE(0 == pool.idle_count());
    REQUIRE(2 == pool.busy_count());
    auto g3 = pool.next_idle_graph();
    REQUIRE(g3);
    REQUIRE(1 == pool.idle_count());
    REQUIRE(3 == pool.busy_count());
    REQUIRE(4 == pool.size());
    auto g4 = pool.next_idle_graph();
    REQUIRE(g4);
    REQUIRE(0 == pool.idle_count());
    REQUIRE(4 == pool.busy_count());
    REQUIRE(4 == pool.size());
    g1->task->schedule();
    g2->task->schedule();
    g3->task->schedule();
    g4->task->schedule();
    auto g5 = pool.next_idle_graph();
    REQUIRE(g5);
    REQUIRE(3 == pool.idle_count());
    REQUIRE(1 == pool.busy_count());
    REQUIRE(4 == pool.size());
}

TEST_CASE("graph_pool_next_idle_graph_with_nullptr") {
    tw::graph_pool<mock_graph> pool(make_mock_graph, 1, 2);
    REQUIRE(1 == pool.size());
    auto g1 = pool.next_idle_graph();
    REQUIRE(g1);
    REQUIRE(0 == pool.idle_count());
    REQUIRE(1 == pool.busy_count());
    auto g2 = pool.next_idle_graph();
    REQUIRE(g2);
    REQUIRE(0 == pool.idle_count());
    REQUIRE(2 == pool.busy_count());
    auto g3 = pool.next_idle_graph();
    REQUIRE_FALSE(g3); // got a nullptr
    REQUIRE(0 == pool.idle_count());
    REQUIRE(2 == pool.busy_count());
}

TEST_CASE("graph_pool_resize") {
    tw::graph_pool<mock_graph> pool(make_mock_graph, 2, 100);
    REQUIRE(2 == pool.size());
    pool.resize(4);
    REQUIRE(4 == pool.size());
    pool.resize(1);
    REQUIRE(2 == pool.size());
}

TEST_CASE("graph_pool_resize_with_max") {
    tw::graph_pool<mock_graph> pool(make_mock_graph, 2, 5);
    REQUIRE(2 == pool.size());
    pool.resize(6);
    REQUIRE(5 == pool.size());
}

TEST_CASE("graph_pool_reclaim") {
    tw::graph_pool<mock_graph> pool(make_mock_graph, 2, 4);
    REQUIRE(2 == pool.size());
    auto g1 = pool.next_idle_graph();
    REQUIRE(g1);
    auto g2 = pool.next_idle_graph();
    REQUIRE(g2);
    auto g3 = pool.next_idle_graph();
    REQUIRE(g3);
    auto g4 = pool.next_idle_graph();
    REQUIRE(g4);
    REQUIRE(4 == pool.size());
    pool.resize(2);
    REQUIRE(4 == pool.size());
    g1->task->schedule();
    g2->task->schedule();
    g3->task->schedule();
    g4->task->schedule();
    pool.resize(2); // calls reclaim
    REQUIRE(2 == pool.size());
}