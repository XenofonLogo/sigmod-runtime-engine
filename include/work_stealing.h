#ifndef WORK_STEALING_H
#define WORK_STEALING_H

#include <atomic>
#include <cstddef>
#include <vector>
#include <functional>

namespace Contest {

// Ρυθμίσεις για το work stealing (κατανομή εργασίας μεταξύ νημάτων)
struct WorkStealingConfig {
    size_t total_work;           // Συνολικός αριθμός αντικειμένων προς επεξεργασία
    size_t num_threads;          // Αριθμός νημάτων εργασίας
    size_t min_block_size;       // Ελάχιστο μέγεθος block (προεπιλογή: 256)
    size_t blocks_per_thread;    // Στόχος blocks ανά νήμα (προεπιλογή: 16)
    
    // Υπολογισμός βέλτιστου μεγέθους block για ισοζύγιση φορτίου
    size_t get_block_size() const;
};

// Συντονιστής work stealing με χρήση atomic counter
class WorkStealingCoordinator {
public:
    explicit WorkStealingCoordinator(const WorkStealingConfig& config);
    
    // Προσπάθεια να "κλέψει" ένα block εργασίας
    // Επιστρέφει true αν κλάπηκε εργασία, false αν έχει ολοκληρωθεί όλη η εργασία
    bool steal_block(size_t& begin, size_t& end);
    
private:
    size_t total_work_;
    size_t block_size_;
    std::atomic<size_t> work_counter_;
};

} // namespace Contest

#endif // WORK_STEALING_H
