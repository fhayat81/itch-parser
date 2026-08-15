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
    
    // --- Throughput Calculations ---
    double seconds = duration.count() / 1000.0;
    double file_size_mb = parser.file_size() / (1024.0 * 1024.0);
    
    double msgs_per_sec = parser.total_messages() / seconds;
    double mb_per_sec = file_size_mb / seconds;
    double avg_latency_ns = (seconds * 1'000'000'000.0) / parser.total_messages();

    std::cout << "\n========================================\n";
    std::cout << "   NASDAQ ITCH 5.0 Feed Handler Engine   \n";
    std::cout << "========================================\n";
    std::cout << "Total Messages Scanned: " << parser.total_messages() << "\n";
    std::cout << "Add Orders ('A'/'F'):   " << parser.add_orders() << "\n";
    std::cout << "Executions ('E'/'C'):   " << parser.executed_orders() << "\n";
    std::cout << "Cancels/Deletes ('X/D'):" << parser.cancelled_orders() << "\n";
    std::cout << "Replaces ('U'):        " << parser.replaced_orders() << "\n";
    std::cout << "----------------------------------------\n";
    std::cout << "Processing Time:        " << std::fixed << std::setprecision(2) << duration.count() << " ms\n";
    std::cout << "Message Throughput:     " << std::fixed << std::setprecision(2) << (msgs_per_sec / 1'000'000.0) << " Million msgs/sec\n";
    std::cout << "Data Throughput:        " << std::fixed << std::setprecision(2) << mb_per_sec << " MB/sec\n";
    std::cout << "Avg Latency (Wall):     " << std::fixed << std::setprecision(2) << avg_latency_ns << " ns / msg\n";
    std::cout << "========================================\n";

    parser.print_latency_metrics();

    return 0;
}