#include "work_stealing.h"
#include <algorithm>

namespace Contest {

size_t WorkStealingConfig::get_block_size() const {
    if (total_work == 0 || num_threads == 0) return 0;
    
    // Balanced block size for load stealing
    const size_t calculated = total_work / (num_threads * blocks_per_thread);
    return std::max(min_block_size, calculated);
}

WorkStealingCoordinator::WorkStealingCoordinator(const WorkStealingConfig& config)
    : total_work_(config.total_work)
    , block_size_(config.get_block_size())
    , work_counter_(0)
{
}

bool WorkStealingCoordinator::steal_block(size_t& begin, size_t& end) {
    // Προσπαθεί να "κλέψει" ένα block εργασίας (atomic increment)
    begin = work_counter_.fetch_add(block_size_, std::memory_order_acquire);
    // Αν δεν υπάρχει άλλο work, επιστρέφει false
    if (begin >= total_work_) {
        return false;  // Όλη η εργασία έχει ολοκληρωθεί
    }
    // Υπολογίζει το τέλος του block
    end = std::min(total_work_, begin + block_size_);
    return true;
}

} // namespace Contest
