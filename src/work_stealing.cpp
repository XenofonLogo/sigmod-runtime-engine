// work_stealing.cpp - simple work-stealing coordinator implementation
#include "work_stealing.h"   // Declarations: WorkStealingConfig / WorkStealingCoordinator
#include <algorithm>          // std::max, std::min

namespace Contest {

size_t WorkStealingConfig::get_block_size() const {
    if (total_work == 0 || num_threads == 0) return 0; // No work or no threads
    // Compute a balanced block size
    const size_t calculated = total_work / (num_threads * blocks_per_thread);
    return std::max(min_block_size, calculated);       // Minimum safety threshold
}

WorkStealingCoordinator::WorkStealingCoordinator(const WorkStealingConfig& config)
    : total_work_(config.total_work)              // Total work
    , block_size_(config.get_block_size())        // Block size to steal
    , work_counter_(0)                            // Atomic progress counter
{
}

bool WorkStealingCoordinator::steal_block(size_t& begin, size_t& end) {
    // Atomic increment to claim the next block
    begin = work_counter_.fetch_add(block_size_, std::memory_order_acquire);
    if (begin >= total_work_) return false;                 // No work left
    end = std::min(total_work_, begin + block_size_);       // Compute block end
    return true;
}

} // namespace Contest
