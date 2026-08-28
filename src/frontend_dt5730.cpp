#include "DAQManager.h"
#include <charconv>
#include <iostream>
#include <fstream>
#include <getopt.h>
#include <csignal>
#include <iomanip>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>

#ifndef CPNR_GIT_COMMIT
#define CPNR_GIT_COMMIT "unknown"
#endif

#ifndef CPNR_BUILD_TIMESTAMP
#define CPNR_BUILD_TIMESTAMP "unknown"
#endif

namespace {

volatile std::sig_atomic_t g_stop_requested = 0;

void sig_handler(int) noexcept {
    // Assignment to volatile sig_atomic_t is safe in an asynchronous signal
    // handler.  A normal thread bridges this flag to DAQManager's atomic flag.
    g_stop_requested = 1;
}

class SignalFlagBridge {
public:
    explicit SignalFlagBridge(std::atomic<bool>& daq_running)
        : daq_running_(daq_running), worker_([this]() { Run(); }) {}

    ~SignalFlagBridge() {
        finished_.store(true, std::memory_order_relaxed);
        if (worker_.joinable()) worker_.join();
    }

    SignalFlagBridge(const SignalFlagBridge&) = delete;
    SignalFlagBridge& operator=(const SignalFlagBridge&) = delete;

private:
    void Run() {
        while (!finished_.load(std::memory_order_relaxed)) {
            if (g_stop_requested != 0) {
                daq_running_.store(false, std::memory_order_relaxed);
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    std::atomic<bool>& daq_running_;
    std::atomic<bool> finished_{false};
    std::thread worker_;
};

struct FrontendOptions {
    std::string config_file = "config/dt5730s_inorganic.conf";
    std::string output_file = "../data/data_run.dat";
    std::string metadata_file;
    int max_events = 0;
    int run_time_sec = 0;
    int run_number = 0;
};

class CommandLineError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

void PrintUsage(std::ostream& output, const char* program) {
    output << "Usage: " << (program == nullptr ? "frontend_dt5730" : program)
           << " [-c config] [-o output] [-n events] [-t seconds]"
              " -r run_number [-m runtime_metadata.json]\n"
           << "  -r must be positive; -n and -t must be nonnegative and "
              "cannot both be positive.\n"
           << "  -n is additionally limited by the 32-bit EventHeader "
              "EventID and this build's signed DAQ limit.\n";
}

long long ParseStrictInteger(const char* text, char option) {
    if (text == nullptr || *text == '\0') {
        throw CommandLineError(std::string("Option -") + option +
                               " requires a non-empty base-10 integer");
    }

    const char* const end = text + std::char_traits<char>::length(text);
    long long value = 0;
    const auto result = std::from_chars(text, end, value, 10);
    if (result.ec == std::errc::result_out_of_range) {
        throw CommandLineError(std::string("Option -") + option +
                               " integer is outside the supported range: " +
                               text);
    }
    if (result.ec != std::errc{} || result.ptr != end) {
        throw CommandLineError(std::string("Option -") + option +
                               " requires a complete base-10 integer: " +
                               text);
    }
    return value;
}

int ParseBoundedInteger(const char* text, char option, int minimum,
                        int maximum) {
    const long long value = ParseStrictInteger(text, option);
    if (value < minimum || value > maximum) {
        throw CommandLineError(
            std::string("Option -") + option + " must be in the range " +
            std::to_string(minimum) + ".." + std::to_string(maximum));
    }
    return static_cast<int>(value);
}

int ParseMaxEvents(const char* text) {
    const long long value = ParseStrictInteger(text, 'n');
    if (value < 0) {
        throw CommandLineError("Option -n must be nonnegative");
    }
    constexpr auto kEventHeaderLimit =
        static_cast<unsigned long long>(std::numeric_limits<uint32_t>::max());
    if (static_cast<unsigned long long>(value) > kEventHeaderLimit) {
        throw CommandLineError(
            "Option -n exceeds the 32-bit EventHeader EventID capacity");
    }
    if (value > std::numeric_limits<int>::max()) {
        throw CommandLineError(
            "Option -n exceeds this build's signed DAQ event limit (" +
            std::to_string(std::numeric_limits<int>::max()) + ")");
    }
    return static_cast<int>(value);
}

std::string ParseNonEmptyText(const char* text, char option) {
    if (text == nullptr || *text == '\0') {
        throw CommandLineError(std::string("Option -") + option +
                               " requires a non-empty value");
    }
    return text;
}

FrontendOptions ParseCommandLine(int argc, char** argv) {
    FrontendOptions options;
    opterr = 0;
    int option = 0;
    while ((option = getopt(argc, argv, ":c:o:n:t:r:m:")) != -1) {
        switch (option) {
            case 'c': options.config_file = ParseNonEmptyText(optarg, 'c'); break;
            case 'o': options.output_file = ParseNonEmptyText(optarg, 'o'); break;
            case 'n': options.max_events = ParseMaxEvents(optarg); break;
            case 't':
                options.run_time_sec = ParseBoundedInteger(
                    optarg, 't', 0, std::numeric_limits<int>::max());
                break;
            case 'r':
                options.run_number = ParseBoundedInteger(
                    optarg, 'r', 1, std::numeric_limits<int>::max());
                break;
            case 'm': options.metadata_file = ParseNonEmptyText(optarg, 'm'); break;
            case ':':
                throw CommandLineError(std::string("Option -") +
                                       static_cast<char>(optopt) +
                                       " requires a value");
            case '?':
                throw CommandLineError(std::string("Unknown option: -") +
                                       static_cast<char>(optopt));
            default:
                throw CommandLineError("Cannot parse command line");
        }
    }

    if (optind != argc) {
        throw CommandLineError("Unexpected positional argument: " +
                               std::string(argv[optind]));
    }
    if (options.run_number <= 0) {
        throw CommandLineError(
            "Run number (-r) must be provided as a positive integer");
    }
    if (options.max_events > 0 && options.run_time_sec > 0) {
        throw CommandLineError(
            "Options -n and -t cannot both be positive");
    }
    return options;
}

}  // namespace

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
    FrontendOptions options;
    try {
        options = ParseCommandLine(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "Command-line error: " << error.what() << "\n";
        PrintUsage(std::cerr, argc > 0 ? argv[0] : nullptr);
        return 2;
    }

    const auto absolute_path = [](const std::string& path) {
        return std::filesystem::absolute(std::filesystem::path(path))
            .lexically_normal().string();
    };
    options.config_file = absolute_path(options.config_file);
    options.output_file = absolute_path(options.output_file);
    if (options.metadata_file.empty()) {
        options.metadata_file = options.output_file + ".run.json";
    }
    options.metadata_file = absolute_path(options.metadata_file);
    const std::string executable_path = ExecutablePath(argv[0]);

    if (std::signal(SIGINT, sig_handler) == SIG_ERR ||
        std::signal(SIGTERM, sig_handler) == SIG_ERR) {
        std::cerr << "Cannot install SIGINT/SIGTERM handlers.\n";
        return 1;
    }

    try {
        std::atomic<bool> is_running{true};
        SignalFlagBridge signal_bridge(is_running);

        PrintConfigContent(options.config_file);
        auto t = std::time(nullptr);
        auto tm = *std::localtime(&t);
        std::cout << "\033[1;32m[Frontend] System Boot Time : \033[0m" << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << "\n"
                  << "\033[1;34m[Frontend] Executable       : \033[0m" << executable_path << "\n"
                  << "\033[1;34m[Frontend] Build Commit     : \033[0m" << CPNR_GIT_COMMIT << "\n"
                  << "\033[1;34m[Frontend] Build Timestamp  : \033[0m" << CPNR_BUILD_TIMESTAMP << "\n"
                  << "\033[1;34m[Frontend] Config           : \033[0m" << options.config_file << "\n"
                  << "\033[1;34m[Frontend] Output Target    : \033[0m" << options.output_file << "\n"
                  << "\033[1;34m[Frontend] Runtime Metadata : \033[0m" << options.metadata_file << "\n"
                  << "\033[1;34m[Frontend] Run Number       : \033[0m" << options.run_number << "\n";

        DAQManager daq(options.config_file, options.output_file,
                       options.max_events, options.run_time_sec,
                       options.run_number, options.metadata_file, executable_path,
                       CPNR_GIT_COMMIT, CPNR_BUILD_TIMESTAMP,
                       &is_running);
        
        daq.Start(is_running);
        if (!is_running.load(std::memory_order_relaxed)) {
            std::cout << "\n\033[1;33m[Interrupt] DAQ stopped and runtime artifacts "
                         "were finalized.\033[0m\n";
        }

    } catch (const DAQSetupCancelled& e) {
        std::cout << "\n\033[1;33m[Interrupt] " << e.what()
                  << "; terminal metadata was recorded.\033[0m\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n\033[1;31m[Fatal Error]\033[0m " << e.what() << "\n";
        return 1;
    }
    return 0;
}
