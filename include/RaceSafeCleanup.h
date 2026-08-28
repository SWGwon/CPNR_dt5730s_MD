#ifndef CPNR_RACE_SAFE_CLEANUP_H
#define CPNR_RACE_SAFE_CLEANUP_H

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <vector>

namespace cpnr {

inline int RenameNoReplaceForCleanup(int old_directory, const char* old_path,
                                     int new_directory,
                                     const char* new_path) noexcept {
#ifdef SYS_renameat2
  constexpr unsigned int kRenameNoReplace = 1U;
  return static_cast<int>(::syscall(SYS_renameat2, old_directory, old_path,
                                    new_directory, new_path,
                                    kRenameNoReplace));
#else
  (void)old_directory;
  (void)old_path;
  (void)new_directory;
  (void)new_path;
  errno = ENOSYS;
  return -1;
#endif
}

// POSIX has no unlink-by-inode operation. Atomically moving the public name
// into a private, pinned quarantine closes the lstat(path)->unlink(path) race.
// A mismatching node is restored without overwrite, or preserved in quarantine
// if another process has already reused the public name.
inline bool RemovePathIfSameNodeImpl(const std::string& path,
                                     dev_t expected_device,
                                     ino_t expected_inode,
                                     const char* diagnostic_prefix) {
  const auto target = std::filesystem::path(path);
  const auto parent = target.parent_path().empty()
                          ? std::filesystem::path(".")
                          : target.parent_path();
  std::string quarantine_template =
      (parent / ".cpnr-cleanup-XXXXXX").string();
  std::vector<char> quarantine_buffer(quarantine_template.begin(),
                                      quarantine_template.end());
  quarantine_buffer.push_back('\0');
  char* const created = ::mkdtemp(quarantine_buffer.data());
  if (created == nullptr) return false;
  const std::string quarantine_path(created);

  const int quarantine_descriptor =
      ::open(quarantine_path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (quarantine_descriptor < 0) {
    ::rmdir(quarantine_path.c_str());
    return false;
  }

  constexpr const char* kCandidate = "candidate";
  const auto close_empty_quarantine = [&]() {
    ::close(quarantine_descriptor);
    if (::rmdir(quarantine_path.c_str()) != 0 && errno != ENOENT) {
      std::cerr << diagnostic_prefix
                << " Cannot remove empty cleanup quarantine: "
                << quarantine_path << " (" << std::strerror(errno) << ")\n";
    }
  };
  const auto preserve_or_restore = [&]() {
    if (RenameNoReplaceForCleanup(quarantine_descriptor, kCandidate, AT_FDCWD,
                                  path.c_str()) == 0) {
      close_empty_quarantine();
      return;
    }
    const int restore_error = errno;
    ::close(quarantine_descriptor);
    std::cerr << diagnostic_prefix << " Preserved a cleanup-race entry in "
              << quarantine_path << "/" << kCandidate
              << " because its public name could not be restored without "
                 "overwrite: "
              << path << " (" << std::strerror(restore_error) << ")\n";
  };

  if (RenameNoReplaceForCleanup(AT_FDCWD, path.c_str(), quarantine_descriptor,
                                kCandidate) != 0) {
    close_empty_quarantine();
    return false;
  }

  struct stat status {};
  if (::fstatat(quarantine_descriptor, kCandidate, &status,
                AT_SYMLINK_NOFOLLOW) != 0) {
    preserve_or_restore();
    return false;
  }
  if (status.st_dev != expected_device || status.st_ino != expected_inode) {
    preserve_or_restore();
    return false;
  }
  if (::unlinkat(quarantine_descriptor, kCandidate, 0) != 0) {
    preserve_or_restore();
    return false;
  }
  close_empty_quarantine();
  return true;
}

inline bool RemovePathIfSameNode(const std::string& path, dev_t expected_device,
                                 ino_t expected_inode,
                                 const char* diagnostic_prefix) noexcept {
  try {
    return RemovePathIfSameNodeImpl(path, expected_device, expected_inode,
                                    diagnostic_prefix);
  } catch (...) {
    // Cleanup is best-effort and is frequently called while preserving another
    // exception. Allocation/path-construction failures must not terminate the
    // process or turn preservation into deletion.
    return false;
  }
}

}  // namespace cpnr

#endif  // CPNR_RACE_SAFE_CLEANUP_H
