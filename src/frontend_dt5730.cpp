#include "DAQManager.h"
#include <iostream>
#include <fstream>
#include <getopt.h>
#include <csignal>
#include <iomanip>
#include <atomic>
#include <filesystem>

#ifndef CPNR_GIT_COMMIT
#define CPNR_GIT_COMMIT "unknown"
#endif

#ifndef CPNR_BUILD_TIMESTAMP
#define CPNR_BUILD_TIMESTAMP "unknown"
#endif

// 스레드 안전한 종료 플래그
std::atomic<bool> g_is_running{true};

void sig_handler(int) {
    g_is_running.store(false, std::memory_order_relaxed);
}

void PrintConfigContent(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) return;
    std::cout << "\n\033[1;36m=== [ Config Details : " << filepath << " ] ===\033[0m\n";
    std::string line;
    while (std::getline(file, line)) {
        std::cout << "  " << line << "\n";
    }
    std::cout << "\033[1;36m====================================================\033[0m\n\n";
}

std::string ExecutablePath(const char* argv0) {
    std::error_code error;
    const auto executing_path =
        std::filesystem::read_symlink("/proc/self/exe", error);
    if (!error && !executing_path.empty()) return executing_path.string();
    return std::filesystem::absolute(
               std::filesystem::path(argv0 == nullptr ? "" : argv0))
        .lexically_normal().string();
}

int main(int argc, char** argv) {
    std::string config_file = "config/dt5730s_inorganic.conf";
    std::string output_file = "../data/data_run.dat";
    std::string metadata_file;
    int max_events = 0;       
    int run_time_sec = 0;     
    int run_number = 0;

    int opt;
    while ((opt = getopt(argc, argv, "c:o:n:t:r:m:")) != -1) {
        switch (opt) {
            case 'c': config_file = optarg; break;
            case 'o': output_file = optarg; break;
            case 'n': max_events = std::stoi(optarg); break;
            case 't': run_time_sec = std::stoi(optarg); break;
            case 'r': run_number = std::stoi(optarg); break;
            case 'm': metadata_file = optarg; break;
            default:
                std::cerr << "Usage: " << argv[0]
                          << " [-c config] [-o output] [-n events] [-t seconds]"
                          << " [-r run_number] [-m runtime_metadata.json]\n";
                return 2;
        }
    }

    if (run_number <= 0) {
        std::cerr << "Run number must be a positive integer (-r).\n";
        return 2;
    }

    const auto absolute_path = [](const std::string& path) {
        return std::filesystem::absolute(std::filesystem::path(path))
            .lexically_normal().string();
    };
    config_file = absolute_path(config_file);
    output_file = absolute_path(output_file);
    if (metadata_file.empty()) metadata_file = output_file + ".run.json";
    metadata_file = absolute_path(metadata_file);
    const std::string executable_path = ExecutablePath(argv[0]);

    std::signal(SIGINT, sig_handler);
    std::signal(SIGTERM, sig_handler);

    try {
        PrintConfigContent(config_file);
        auto t = std::time(nullptr);
        auto tm = *std::localtime(&t);
        std::cout << "\033[1;32m[Frontend] System Boot Time : \033[0m" << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << "\n"
                  << "\033[1;34m[Frontend] Executable       : \033[0m" << executable_path << "\n"
                  << "\033[1;34m[Frontend] Build Commit     : \033[0m" << CPNR_GIT_COMMIT << "\n"
                  << "\033[1;34m[Frontend] Build Timestamp  : \033[0m" << CPNR_BUILD_TIMESTAMP << "\n"
                  << "\033[1;34m[Frontend] Config           : \033[0m" << config_file << "\n"
                  << "\033[1;34m[Frontend] Output Target    : \033[0m" << output_file << "\n"
                  << "\033[1;34m[Frontend] Runtime Metadata : \033[0m" << metadata_file << "\n"
                  << "\033[1;34m[Frontend] Run Number       : \033[0m" << run_number << "\n";

        DAQManager daq(config_file, output_file, max_events, run_time_sec,
                       run_number, metadata_file, executable_path,
                       CPNR_GIT_COMMIT, CPNR_BUILD_TIMESTAMP,
                       &g_is_running);
        
        // 메인 루프에 플래그 전달
        daq.Start(g_is_running);
        if (!g_is_running.load(std::memory_order_relaxed)) {
            std::cout << "\n\033[1;33m[Interrupt] DAQ stopped and runtime artifacts "
                         "were finalized.\033[0m\n";
        }

    } catch (const std::exception& e) {
        std::cerr << "\n\033[1;31m[Fatal Error]\033[0m " << e.what() << "\n";
        return 1;
    }
    return 0;
}
