#include "RootValidator.h"

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <getopt.h>
#include <iomanip>
#include <iostream>
#include <linux/fs.h>
#include <limits>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;

constexpr int kExitPass = 0;
constexpr int kExitWarn = 1;
constexpr int kExitFail = 2;
constexpr int kExitCancelled = 3;
constexpr int kExitUsage = 64;
constexpr int kExitCannotCreate = 73;
constexpr int kExitSoftware = 70;

std::atomic_bool g_cancelled{false};

extern "C" void HandleSignal(int) { g_cancelled.store(true); }

void PrintUsage(const char* program) {
  std::cerr << "Usage: " << program
            << " -i FILE [--max-events N] [--raw-fidelity]"
               " [--report NEW_FILE]\n"
            << "Exit status: PASS=0, WARN=1, FAIL=2, CANCELLED=3, "
               "usage=64, report-create=73, fatal=70.\n";
}

std::uint64_t ParseMaxEvents(const char* text) {
  if (text == nullptr || *text == '\0' || *text == '-') {
    throw std::invalid_argument("--max-events must be a positive integer");
  }
  errno = 0;
  char* end = nullptr;
  const unsigned long long parsed = std::strtoull(text, &end, 10);
  if (errno == ERANGE || end == text || end == nullptr || *end != '\0' ||
      parsed == 0ULL ||
      parsed > static_cast<unsigned long long>(
                   std::numeric_limits<std::uint64_t>::max())) {
    throw std::invalid_argument("--max-events must be a positive integer");
  }
  return static_cast<std::uint64_t>(parsed);
}

std::string AbsolutePath(const std::string& path) {
  std::error_code error;
  fs::path absolute = fs::absolute(fs::path(path), error);
  if (error) {
    throw std::runtime_error("Cannot resolve path '" + path + "': " +
                             error.message());
  }
  fs::path canonical = fs::weakly_canonical(absolute, error);
  return (error ? absolute.lexically_normal() : canonical).string();
}

bool SameInode(const struct stat& left, const struct stat& right) {
  return left.st_dev == right.st_dev && left.st_ino == right.st_ino;
}

void UnlinkIfSameInode(const std::string& path,
                       const struct stat& expected) noexcept {
  struct stat observed {};
  if (::lstat(path.c_str(), &observed) == 0 &&
      SameInode(observed, expected)) {
    (void)::unlink(path.c_str());
  }
}

void WriteAll(int descriptor, const std::string& payload) {
  std::size_t offset = 0U;
  while (offset < payload.size()) {
    const ssize_t written = ::write(
        descriptor, payload.data() + offset, payload.size() - offset);
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) {
      throw std::runtime_error(
          "Cannot write validation report: " +
          std::string(written < 0 ? std::strerror(errno)
                                  : "zero-byte write"));
    }
    offset += static_cast<std::size_t>(written);
  }
}

void SyncDirectory(const fs::path& directory) {
  const int descriptor = ::open(
      directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) {
    throw std::runtime_error("Cannot open report directory for fsync: " +
                             std::string(std::strerror(errno)));
  }
  const int result = ::fsync(descriptor);
  const int saved_errno = errno;
  (void)::close(descriptor);
  if (result != 0) {
    throw std::runtime_error("Cannot fsync report directory: " +
                             std::string(std::strerror(saved_errno)));
  }
}

void PublishReportNoClobber(const std::string& requested_path,
                            const std::string& input_path,
                            const std::string& payload) {
  if (requested_path.empty()) {
    throw std::runtime_error("--report path is empty");
  }
  const fs::path output(AbsolutePath(requested_path));
  const fs::path input(AbsolutePath(input_path));
  if (output == input) {
    throw std::runtime_error(
        "Report path is the input ROOT path; overwrite is forbidden");
  }

  struct stat input_status {};
  if (::stat(input.c_str(), &input_status) != 0) {
    throw std::runtime_error("Cannot stat input ROOT before report publish: " +
                             std::string(std::strerror(errno)));
  }
  struct stat existing_status {};
  if (::lstat(output.c_str(), &existing_status) == 0) {
    struct stat followed_status {};
    const bool hard_link_to_input =
        ::stat(output.c_str(), &followed_status) == 0 &&
        SameInode(input_status, followed_status);
    throw std::runtime_error(
        hard_link_to_input
            ? "Report path is a hard link to the input ROOT; overwrite is forbidden"
            : "Report path already exists; existing files are never replaced");
  }
  if (errno != ENOENT) {
    throw std::runtime_error("Cannot inspect report path: " +
                             std::string(std::strerror(errno)));
  }

  const fs::path directory = output.parent_path();
  struct stat directory_status {};
  if (::stat(directory.c_str(), &directory_status) != 0 ||
      !S_ISDIR(directory_status.st_mode)) {
    throw std::runtime_error("Report parent is not an existing directory: " +
                             directory.string());
  }

  int descriptor = -1;
  fs::path temporary;
  for (unsigned int attempt = 0U; attempt < 128U; ++attempt) {
    temporary = directory /
        ("." + output.filename().string() + ".cpnr-" +
         std::to_string(static_cast<unsigned long long>(::getpid())) + "-" +
         std::to_string(attempt) + ".tmp");
    descriptor = ::open(temporary.c_str(),
                        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                        S_IRUSR | S_IWUSR | S_IRGRP);
    if (descriptor >= 0) break;
    if (errno != EEXIST) {
      throw std::runtime_error("Cannot create temporary report: " +
                               std::string(std::strerror(errno)));
    }
  }
  if (descriptor < 0) {
    throw std::runtime_error(
        "Cannot allocate a unique temporary report path");
  }

  struct stat created_status {};
  if (::fstat(descriptor, &created_status) != 0) {
    const int saved_errno = errno;
    (void)::close(descriptor);
    (void)::unlink(temporary.c_str());
    throw std::runtime_error("Cannot identify temporary report: " +
                             std::string(std::strerror(saved_errno)));
  }

  bool published = false;
  try {
    WriteAll(descriptor, payload);
    if (::fsync(descriptor) != 0) {
      throw std::runtime_error("Cannot fsync validation report: " +
                               std::string(std::strerror(errno)));
    }
    struct stat final_descriptor_status {};
    struct stat final_path_status {};
    if (::fstat(descriptor, &final_descriptor_status) != 0 ||
        ::lstat(temporary.c_str(), &final_path_status) != 0 ||
        !SameInode(created_status, final_descriptor_status) ||
        !SameInode(created_status, final_path_status) ||
        final_descriptor_status.st_size !=
            static_cast<off_t>(payload.size())) {
      throw std::runtime_error(
          "Temporary report identity changed before publication");
    }

    if (::link(temporary.c_str(), output.c_str()) == 0) {
      published = true;
      UnlinkIfSameInode(temporary.string(), created_status);
    } else {
      const int link_errno = errno;
#if defined(SYS_renameat2) && defined(RENAME_NOREPLACE)
      if ((link_errno == EPERM || link_errno == EOPNOTSUPP ||
           link_errno == ENOTSUP || link_errno == ENOSYS) &&
          ::syscall(SYS_renameat2, AT_FDCWD, temporary.c_str(), AT_FDCWD,
                    output.c_str(), RENAME_NOREPLACE) == 0) {
        published = true;
      } else
#endif
      {
        const int publish_errno = errno == 0 ? link_errno : errno;
        if (publish_errno == EEXIST) {
          throw std::runtime_error(
              "Report path appeared during publication; nothing was replaced");
        }
        throw std::runtime_error("Cannot atomically publish report: " +
                                 std::string(std::strerror(publish_errno)));
      }
    }
    SyncDirectory(directory);
  } catch (...) {
    (void)::close(descriptor);
    if (!published) UnlinkIfSameInode(temporary.string(), created_status);
    throw;
  }
  if (::close(descriptor) != 0) {
    throw std::runtime_error("Cannot close published report descriptor: " +
                             std::string(std::strerror(errno)));
  }
}

int ExitCodeForStatus(const nlohmann::json& report) {
  if (!report.contains("overall_status") ||
      !report.at("overall_status").is_string()) {
    throw std::runtime_error("Validation report has no overall_status");
  }
  const std::string status = report.at("overall_status").get<std::string>();
  if (status == "PASS") return kExitPass;
  if (status == "WARN") return kExitWarn;
  if (status == "FAIL") return kExitFail;
  if (status == "CANCELLED") return kExitCancelled;
  throw std::runtime_error("Unknown validation overall_status: " + status);
}

}  // namespace

int main(int argc, char** argv) {
  std::string input_path;
  std::string report_path;
  bool report_requested = false;
  std::uint64_t max_events = 0U;
  bool verify_raw_fidelity = false;
  static const option long_options[] = {
      {"input", required_argument, nullptr, 'i'},
      {"max-events", required_argument, nullptr, 'n'},
      {"raw-fidelity", no_argument, nullptr, 'f'},
      {"report", required_argument, nullptr, 'o'},
      {"help", no_argument, nullptr, 'h'},
      {nullptr, 0, nullptr, 0}};

  int option_index = 0;
  int selected = 0;
  while ((selected =
              ::getopt_long(argc, argv, "i:n:fo:h", long_options,
                            &option_index)) != -1) {
    try {
      switch (selected) {
        case 'i':
          input_path = optarg == nullptr ? "" : optarg;
          break;
        case 'n':
          max_events = ParseMaxEvents(optarg);
          break;
        case 'f':
          verify_raw_fidelity = true;
          break;
        case 'o':
          report_path = optarg == nullptr ? "" : optarg;
          report_requested = true;
          if (report_path.empty()) {
            throw std::invalid_argument("--report requires a non-empty path");
          }
          break;
        case 'h':
          PrintUsage(argv[0]);
          return 0;
        default:
          PrintUsage(argv[0]);
          return kExitUsage;
      }
    } catch (const std::exception& error) {
      std::cerr << "[ValidationFatal] " << error.what() << '\n';
      PrintUsage(argv[0]);
      return kExitUsage;
    }
  }

  if (input_path.empty() || optind != argc) {
    PrintUsage(argv[0]);
    return kExitUsage;
  }

  std::signal(SIGINT, HandleSignal);
  std::signal(SIGTERM, HandleSignal);

  cpnr::RootValidationOptions options;
  options.max_events = max_events;
  options.verify_raw_fidelity = verify_raw_fidelity;
  options.cancelled = &g_cancelled;
  options.progress = [](double percent, std::string_view stage) {
    std::cerr << "[ValidationProgress] " << std::fixed
              << std::setprecision(1) << percent << "% | " << stage << '\n';
  };

  try {
    const nlohmann::json report =
        cpnr::ValidateRootFile(input_path, options);
    const std::string payload = report.dump(2) + "\n";
    if (!report_requested) {
      std::cout << payload;
    } else {
      try {
        PublishReportNoClobber(report_path, input_path, payload);
      } catch (const std::exception& error) {
        std::cerr << "[ValidationReportError] " << error.what() << '\n';
        return kExitCannotCreate;
      }
      std::cerr << "[ValidationReport] " << AbsolutePath(report_path) << '\n';
    }
    return ExitCodeForStatus(report);
  } catch (const std::exception& error) {
    std::cerr << "[ValidationFatal] " << error.what() << '\n';
    return kExitSoftware;
  }
}
