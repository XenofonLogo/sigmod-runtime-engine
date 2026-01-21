#include <catch2/catch_test_macros.hpp>
#include <thread>
#include <vector>
#include <atomic>
#include <set>
#include "work_stealing.h"

using namespace Contest;

// ============================================================================
// WORK STEALING COORDINATOR TESTS
// ============================================================================

TEST_CASE("WorkStealingCoordinator: steal_block with valid work range", "[work-stealing][steal]") {
    WorkStealingConfig config;
    config.total_work = 1000;
    config.num_threads = 4;
    config.min_block_size = 256;
    config.blocks_per_thread = 16;
    
    WorkStealingCoordinator coordinator(config);
    
    size_t begin, end;
    bool got_work = coordinator.steal_block(begin, end);
    
    REQUIRE(got_work);
    REQUIRE(begin < end);
    REQUIRE(begin >= 0);
    REQUIRE(end <= config.total_work);
}

TEST_CASE("WorkStealingCoordinator: sequential block stealing", "[work-stealing][sequential]") {
    WorkStealingConfig config;
    config.total_work = 1000;
    config.num_threads = 1;
    config.min_block_size = 100;
    config.blocks_per_thread = 10;
    
    WorkStealingCoordinator coordinator(config);
    
    std::vector<std::pair<size_t, size_t>> blocks;
    size_t begin, end;
    
    while (coordinator.steal_block(begin, end)) {
        blocks.push_back({begin, end});
    }
    
    // Should have stolen several blocks
    REQUIRE(blocks.size() > 0);
    
    // Blocks should be contiguous
    REQUIRE(blocks[0].first == 0);
    for (size_t i = 1; i < blocks.size(); ++i) {
        REQUIRE(blocks[i].first == blocks[i-1].second);
    }
    REQUIRE(blocks.back().second == config.total_work);
}

TEST_CASE("WorkStealingCoordinator: block boundaries", "[work-stealing][boundaries]") {
    WorkStealingConfig config;
    config.total_work = 500;
    config.num_threads = 2;
    config.min_block_size = 50;
    config.blocks_per_thread = 4;
    
    WorkStealingCoordinator coordinator(config);
    
    size_t begin, end;
    REQUIRE(coordinator.steal_block(begin, end));
    REQUIRE(begin >= 0);
    REQUIRE(end > begin);
    REQUIRE(end <= config.total_work);
}

TEST_CASE("WorkStealingCoordinator: exhaustion returns false", "[work-stealing][exhaustion]") {
    WorkStealingConfig config;
    config.total_work = 100;
    config.num_threads = 1;
    config.min_block_size = 50;
    config.blocks_per_thread = 2;
    
    WorkStealingCoordinator coordinator(config);
    
    // Steal until no work left
    size_t total_work_stolen = 0;
    size_t begin, end;
    int steal_count = 0;
    
    while (coordinator.steal_block(begin, end)) {
        total_work_stolen += (end - begin);
        steal_count++;
    }
    
    // Should have stolen approximately the total work
    REQUIRE(total_work_stolen >= config.total_work);
    REQUIRE(steal_count > 0);
    
    // Next steal should fail
    REQUIRE(!coordinator.steal_block(begin, end));
}

TEST_CASE("WorkStealingCoordinator: concurrent stealing (2 threads)", "[work-stealing][concurrent]") {
    WorkStealingConfig config;
    config.total_work = 1000;
    config.num_threads = 2;
    config.min_block_size = 50;
    config.blocks_per_thread = 8;
    
    WorkStealingCoordinator coordinator(config);
    std::atomic<size_t> total_work_stolen(0);
    std::vector<std::thread> threads;
    
    for (int t = 0; t < 2; ++t) {
        threads.emplace_back([&] {
            size_t begin, end;
            while (coordinator.steal_block(begin, end)) {
                total_work_stolen += (end - begin);
            }
        });
    }
    
    for (auto& t : threads) t.join();
    
    // All work should be stolen
    REQUIRE(total_work_stolen >= config.total_work);
}

TEST_CASE("WorkStealingCoordinator: concurrent stealing (4 threads)", "[work-stealing][concurrent][stress]") {
    WorkStealingConfig config;
    config.total_work = 10000;
    config.num_threads = 4;
    config.min_block_size = 100;
    config.blocks_per_thread = 20;
    
    WorkStealingCoordinator coordinator(config);
    std::atomic<size_t> total_work_stolen(0);
    std::vector<std::thread> threads;
    
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&] {
            size_t begin, end;
            while (coordinator.steal_block(begin, end)) {
                total_work_stolen += (end - begin);
            }
        });
    }
    
    for (auto& t : threads) t.join();
    
    // All work should be stolen despite contention
    REQUIRE(total_work_stolen >= config.total_work);
}

TEST_CASE("WorkStealingCoordinator: work distribution fairness", "[work-stealing][balance]") {
    WorkStealingConfig config;
    config.total_work = 2000;
    config.num_threads = 4;
    config.min_block_size = 100;
    config.blocks_per_thread = 10;
    
    WorkStealingCoordinator coordinator(config);
    std::vector<std::atomic<size_t>> thread_work(4);
    std::vector<std::thread> threads;
    
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&, t] {
            size_t begin, end;
            while (coordinator.steal_block(begin, end)) {
                thread_work[t] += (end - begin);
            }
        });
    }
    
    for (auto& t : threads) t.join();
    
    // Verify total work was distributed
    size_t total_work_done = 0;
    for (int t = 0; t < 4; ++t) {
        total_work_done += thread_work[t].load();
    }
    
    REQUIRE(total_work_done >= config.total_work);
}

TEST_CASE("WorkStealingCoordinator: get_block_size() respects config", "[work-stealing][config]") {
    WorkStealingConfig config;
    config.total_work = 1000;
    config.num_threads = 4;
    config.min_block_size = 256;
    config.blocks_per_thread = 10;
    
    size_t block_size = config.get_block_size();
    
    // Block size should respect min_block_size
    REQUIRE(block_size >= config.min_block_size);
}

TEST_CASE("WorkStealingCoordinator: no work skipped or duplicated", "[work-stealing][coverage]") {
    WorkStealingConfig config;
    config.total_work = 500;
    config.num_threads = 2;
    config.min_block_size = 50;
    config.blocks_per_thread = 5;
    
    WorkStealingCoordinator coordinator(config);
    
    std::set<size_t> covered_items;
    size_t begin, end;
    
    while (coordinator.steal_block(begin, end)) {
        for (size_t i = begin; i < end; ++i) {
            // Check no duplicates
            REQUIRE(covered_items.find(i) == covered_items.end());
            covered_items.insert(i);
        }
    }
    
    // Should cover all work items
    REQUIRE(covered_items.size() >= config.total_work);
}

