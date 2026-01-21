#ifndef WORK_STEALING_H
#define WORK_STEALING_H

#include <atomic>
#include <cstddef>
#include <vector>
#include <functional>

namespace Contest {

// Configuration for work stealing
struct WorkStealingConfig {
    size_t total_work;           // Total number of items to process
    size_t num_threads;          // Number of worker threads
    size_t min_block_size;       // Minimum block size (default: 256)
    size_t blocks_per_thread;    // Target blocks per thread (default: 16)
    
    // Calculate optimal block size for load balancing
    size_t get_block_size() const;
};

// Work stealing coordinator using atomic counter
class WorkStealingCoordinator {
public:
    explicit WorkStealingCoordinator(const WorkStealingConfig& config);
    
    // Try to steal a block of work
    // Returns true if work was stolen, false if all work is done
    bool steal_block(size_t& begin, size_t& end);
    
private:
    size_t total_work_;
    size_t block_size_;
    std::atomic<size_t> work_counter_;
};

} // namespace Contest

#endif // WORK_STEALING_H
