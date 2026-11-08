#include "vqf/vqf_core.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>

int main() {
    try {
        vqf::VQFEngine engine(3);
        if (!engine.insert(12345) || !engine.insert(7) || !engine.insert(99)) {
            std::cerr << "Failed to insert within capacity\n";
            return 1;
        }
        if (!engine.query(12345) || !engine.query(7) || !engine.query(99)) {
            std::cerr << "Inserted value was not found\n";
            return 1;
        }
        if (engine.query(42) || engine.insert(42) || engine.size() != 3) {
            std::cerr << "Unexpected behavior for a missing value or full table\n";
            return 1;
        }
        if (!engine.insert(7) || engine.size() != 3) {
            std::cerr << "Duplicate insertion changed the item count\n";
            return 1;
        }

        const vqf::value_type items[] = {7, 42, 99};
        std::uint8_t results[] = {0, 0, 0};
        engine.batch_process(items, 3, results);
        if (results[0] != 1 || results[1] != 0 || results[2] != 1) {
            std::cerr << "Batch query returned incorrect results\n";
            return 1;
        }

        bool rejected_zero_capacity = false;
        try {
            vqf::VQFEngine invalid(0);
        } catch (const std::invalid_argument&) {
            rejected_zero_capacity = true;
        }
        if (!rejected_zero_capacity) {
            std::cerr << "Zero capacity was not rejected\n";
            return 1;
        }

        std::cout << "[PASS] Baseline correctness tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Unexpected exception: " << error.what() << '\n';
        return 1;
    }
}
