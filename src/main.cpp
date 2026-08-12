#include <iostream>
#include <iomanip>
#include <vector>
#include <string>
#include <chrono>
#include "ITCHParser.hpp"

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "Usage: " << argv[0] << " <path_to_binary_file>\n";
        return 1;
    }

    itch::ITCHParser parser;

    auto start = std::chrono::high_resolution_clock::now();
    bool success = parser.process_file(argv[1]);
    auto end = std::chrono::high_resolution_clock::now();

    if (!success) return 1;

    std::chrono::duration<double, std::milli> duration = end - start;

    std::cout << "\n========================================\n";
    std::cout << "   NASDAQ ITCH 5.0 Feed Handler Engine   \n";
    std::cout << "========================================\n";
    std::cout << "Total Messages Scanned: " << parser.total_messages() << "\n";
    std::cout << "Add Orders ('A'/'F'):   " << parser.add_orders() << "\n";
    std::cout << "Executions ('E'/'C'):   " << parser.executed_orders() << "\n";
    std::cout << "Cancels/Deletes ('X/D'):" << parser.cancelled_orders() << "\n";
    std::cout << "Replaces ('U'):        " << parser.replaced_orders() << "\n";
    std::cout << "Processing Time:        " << duration.count() << " ms\n";
    std::cout << "========================================\n\n";


    return 0;
}