// work_stealing.cpp — απλή υλοποίηση συντονιστή work stealing
#include "work_stealing.h"   // Δηλώσεις WorkStealingConfig/Coordinator
#include <algorithm>          // std::max, std::min

namespace Contest {

size_t WorkStealingConfig::get_block_size() const {
    if (total_work == 0 || num_threads == 0) return 0; // Χωρίς δουλειά ή threads
    // Υπολογισμός ισορροπημένου μεγέθους block
    const size_t calculated = total_work / (num_threads * blocks_per_thread);
    return std::max(min_block_size, calculated);       // Ελάχιστο κατώφλι ασφαλείας
}

WorkStealingCoordinator::WorkStealingCoordinator(const WorkStealingConfig& config)
    : total_work_(config.total_work)              // Συνολικό έργο
    , block_size_(config.get_block_size())        // Μέγεθος block προς κλοπή
    , work_counter_(0)                            // Atomic μετρητής προόδου
{
}

bool WorkStealingCoordinator::steal_block(size_t& begin, size_t& end) {
    // Atomic αύξηση για να πάρουμε το επόμενο block
    begin = work_counter_.fetch_add(block_size_, std::memory_order_acquire);
    if (begin >= total_work_) return false;                 // Δεν απέμεινε δουλειά
    end = std::min(total_work_, begin + block_size_);       // Υπολογισμός τέλους block
    return true;
}

} // namespace Contest
