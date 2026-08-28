#include "EventHeader.h"
#include "DT5730Timing.h"
#include "DT5730Status.h"
#include "ConfigParser.h"
#include "DAQConfig.h"
#include "RaceSafeCleanup.h"
#include "Sha256.h"
#include "WaveformDsp.h"
#include <TApplication.h>
#include <TCanvas.h>
#include <TFile.h>
#include <TGraph.h>
#include <TTree.h>
#include <TMacro.h>
#include <TMemFile.h>
#include <TObjString.h>
#include <TParameter.h>
#include <TDatime.h>
#include <TSystem.h> 
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstring>
#include <fstream>
#include <getopt.h>
#include <iostream>
#include <iomanip>
#include <limits>
#include <filesystem>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <chrono>
#include <csignal>
#include <numeric>
#include <sys/select.h> 
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#ifndef CPNR_GIT_COMMIT
#define CPNR_GIT_COMMIT "unknown"
#endif

#ifndef CPNR_BUILD_TIMESTAMP
#define CPNR_BUILD_TIMESTAMP "unknown"
#endif

#ifdef __ROOTCLING__
#pragma link C++ class std::vector<uint16_t>+;
#endif

volatile std::sig_atomic_t g_running = 1;

void sig_handler(int) {
    g_running = 0;
}

namespace {

namespace fs = std::filesystem;

struct FileIdentity {
    uint64_t device = 0U;
    uint64_t inode = 0U;
};

class ScopedDescriptor {
 public:
    ScopedDescriptor() = default;
    explicit ScopedDescriptor(int descriptor) : descriptor_(descriptor) {}
    ~ScopedDescriptor() {
        if (descriptor_ >= 0) ::close(descriptor_);
    }
    ScopedDescriptor(const ScopedDescriptor&) = delete;
    ScopedDescriptor& operator=(const ScopedDescriptor&) = delete;
    ScopedDescriptor(ScopedDescriptor&& other) noexcept
        : descriptor_(other.descriptor_) {
        other.descriptor_ = -1;
    }
    ScopedDescriptor& operator=(ScopedDescriptor&& other) noexcept {
        if (this != &other) {
            reset(other.descriptor_);
            other.descriptor_ = -1;
        }
        return *this;
    }
    int get() const { return descriptor_; }
    void reset(int descriptor) {
        if (descriptor_ >= 0) ::close(descriptor_);
        descriptor_ = descriptor;
    }

 private:
    int descriptor_ = -1;
};

struct TemporaryRootOutput {
    std::string path;
    FileIdentity identity;
    ScopedDescriptor descriptor;
};

bool PathMatchesIdentity(const std::string& path,
                         const FileIdentity& identity) {
    struct stat status {};
    return ::lstat(path.c_str(), &status) == 0 && S_ISREG(status.st_mode) &&
           static_cast<uint64_t>(status.st_dev) == identity.device &&
           static_cast<uint64_t>(status.st_ino) == identity.inode;
}

TemporaryRootOutput ReserveTemporaryRootOutput(
    const std::string& final_path) {
    std::string pattern = final_path + ".partial.XXXXXX";
    std::vector<char> writable_pattern(pattern.begin(), pattern.end());
    writable_pattern.push_back('\0');
    const int descriptor = ::mkstemp(writable_pattern.data());
    if (descriptor < 0) {
        throw std::runtime_error(
            "Cannot reserve temporary ROOT output beside " + final_path +
            " (" + std::strerror(errno) + ")");
    }
    struct stat status {};
    if (::fstat(descriptor, &status) != 0) {
        const int status_error = errno;
        ::close(descriptor);
        throw std::runtime_error(
            "Cannot inspect temporary ROOT output (" +
            std::string(std::strerror(status_error)) +
            "); preserving it because its identity could not be established: " +
            writable_pattern.data());
    }
    return {writable_pattern.data(),
            {static_cast<uint64_t>(status.st_dev),
             static_cast<uint64_t>(status.st_ino)},
            ScopedDescriptor(descriptor)};
}

void InitializeTemporaryRootReservation(TemporaryRootOutput* temporary) {
    if (temporary == nullptr || temporary->descriptor.get() < 0) {
        throw std::runtime_error("Temporary ROOT reservation is not open");
    }
    TMemFile seed("cpnr_reserved_root", "RECREATE");
    if (seed.IsZombie() || seed.Write() < 0) {
        throw std::runtime_error(
            "Cannot initialize an in-memory ROOT reservation image");
    }
    // TMemFile::GetSize() reports its allocated block capacity (typically
    // megabytes), not the ROOT logical EOF.  Copy only the valid file image so
    // no trailing reservation bytes survive into the published archive.
    const Long64_t signed_size = seed.GetEND();
    if (signed_size <= 0 ||
        static_cast<ULong64_t>(signed_size) >
            static_cast<ULong64_t>(std::numeric_limits<size_t>::max())) {
        throw std::runtime_error("Invalid in-memory ROOT reservation size");
    }
    const size_t image_size = static_cast<size_t>(signed_size);
    std::vector<char> image(image_size);
    if (seed.CopyTo(image.data(), signed_size) != signed_size) {
        throw std::runtime_error(
            "Cannot materialize the ROOT reservation image");
    }
    if (::ftruncate(temporary->descriptor.get(), 0) != 0) {
        throw std::runtime_error(
            "Cannot truncate temporary ROOT reservation (" +
            std::string(std::strerror(errno)) + ")");
    }
    size_t written = 0U;
    while (written < image.size()) {
        const ssize_t result = ::pwrite(
            temporary->descriptor.get(), image.data() + written,
            image.size() - written, static_cast<off_t>(written));
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0) {
            throw std::runtime_error(
                "Cannot initialize temporary ROOT reservation (" +
                std::string(std::strerror(errno)) + ")");
        }
        written += static_cast<size_t>(result);
    }
}

bool RemoveTemporaryRootOutput(TemporaryRootOutput* temporary) {
    const bool removed = temporary != nullptr && !temporary->path.empty() &&
        cpnr::RemovePathIfSameNode(
            temporary->path, static_cast<dev_t>(temporary->identity.device),
            static_cast<ino_t>(temporary->identity.inode), "[Production]");
    if (temporary != nullptr) {
        temporary->descriptor.reset(-1);
    }
    return removed;
}

void LinkDescriptorNoReplace(int descriptor, const std::string& target,
                             const std::string& description) {
    // AT_EMPTY_PATH binds the destination directly to the inode held by the
    // descriptor.  This prevents a concurrent pathname replacement from
    // changing which bytes are published after they have been validated.
    constexpr int kAtEmptyPath = 0x1000;
    if (::linkat(descriptor, "", AT_FDCWD, target.c_str(), kAtEmptyPath) == 0) {
        return;
    }
    const int descriptor_link_error = errno;

    // Some older kernels/filesystems reject AT_EMPTY_PATH for unprivileged
    // callers.  /proc/self/fd still names this exact open descriptor; following
    // that symlink in linkat preserves the same inode-safe semantics.
    const std::string descriptor_path =
        "/proc/self/fd/" + std::to_string(descriptor);
    if (::linkat(AT_FDCWD, descriptor_path.c_str(), AT_FDCWD, target.c_str(),
                 AT_SYMLINK_FOLLOW) == 0) {
        return;
    }
    const int proc_link_error = errno;
    throw std::runtime_error(
        "Cannot publish " + description + " without overwrite: " + target +
        " (descriptor link: " + std::strerror(descriptor_link_error) +
        ", /proc fallback: " + std::strerror(proc_link_error) + ")");
}

void SyncParentDirectory(const std::string& path) {
    const std::string parent =
        fs::path(path).parent_path().empty()
            ? std::string(".")
            : fs::path(path).parent_path().string();
    const int descriptor = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY);
    if (descriptor < 0) {
        throw std::runtime_error(
            "Cannot open ROOT output directory for sync (" +
            std::string(std::strerror(errno)) + ")");
    }
    const int sync_result = ::fsync(descriptor);
    const int sync_error = errno;
    ::close(descriptor);
    if (sync_result != 0) {
        throw std::runtime_error(
            "Cannot sync ROOT output directory (" +
            std::string(std::strerror(sync_error)) + ")");
    }
}

void PublishTemporaryRootOutput(TemporaryRootOutput* temporary,
                                const std::string& final_path) {
    if (temporary == nullptr || temporary->descriptor.get() < 0 ||
        !PathMatchesIdentity(temporary->path, temporary->identity)) {
        throw std::runtime_error(
            "Temporary ROOT output inode changed before publication");
    }
    struct stat temporary_status {};
    if (::fstat(temporary->descriptor.get(), &temporary_status) != 0) {
        throw std::runtime_error(
            "Cannot identify temporary ROOT output for publication (" +
            std::string(std::strerror(errno)) + ")");
    }
    if (static_cast<uint64_t>(temporary_status.st_dev) !=
            temporary->identity.device ||
        static_cast<uint64_t>(temporary_status.st_ino) !=
            temporary->identity.inode) {
        throw std::runtime_error(
            "Temporary ROOT output descriptor identity changed before "
            "publication");
    }
    if (::fsync(temporary->descriptor.get()) != 0) {
        throw std::runtime_error(
            "Cannot durably sync temporary ROOT output (" +
            std::string(std::strerror(errno)) + ")");
    }

    LinkDescriptorNoReplace(temporary->descriptor.get(), final_path,
                            "ROOT output");
    if (!PathMatchesIdentity(final_path, temporary->identity)) {
        throw std::runtime_error(
            "Published ROOT output identity changed before verification");
    }
    try {
        SyncParentDirectory(final_path);
    } catch (...) {
        if (cpnr::RemovePathIfSameNode(
                final_path,
                static_cast<dev_t>(temporary->identity.device),
                static_cast<ino_t>(temporary->identity.inode),
                "[Production]")) {
            try {
                SyncParentDirectory(final_path);
            } catch (...) {
                // Preserve the original publication failure below.
            }
        }
        throw;
    }
    if (!PathMatchesIdentity(final_path, temporary->identity)) {
        throw std::runtime_error(
            "Published ROOT output identity changed during durable commit");
    }
    if (!RemoveTemporaryRootOutput(temporary)) {
        std::cerr << "[Warning] Published ROOT output, but temporary hard-link "
                     "cleanup was unsafe or failed: "
                  << temporary->path << "\n";
    } else {
        try {
            SyncParentDirectory(final_path);
        } catch (const std::exception& error) {
            // The final hard link was already durably committed by the first
            // directory fsync. A cleanup-sync failure can at worst resurrect
            // the temporary name after a crash, not invalidate final_path.
            std::cerr << "[Warning] ROOT output is durably published, but "
                         "temporary-name cleanup sync failed: "
                      << error.what() << "\n";
        }
    }
}

int ParsePositiveRunNumber(const std::string& text,
                           const std::string& source) {
    try {
        size_t consumed = 0;
        const long long value = std::stoll(text, &consumed, 10);
        if (consumed != text.size() || value <= 0 || value > INT_MAX) {
            throw std::invalid_argument("out of range");
        }
        return static_cast<int>(value);
    } catch (const std::exception&) {
        throw std::runtime_error(
            "Invalid positive run number from " + source + ": '" + text + "'");
    }
}

bool IsReadableRegularFile(const std::string& path) {
    std::error_code error;
    return !path.empty() && fs::is_regular_file(fs::path(path), error) && !error;
}

std::string AbsolutePath(const std::string& path) {
    if (path.empty()) return "";
    std::error_code error;
    fs::path absolute = fs::absolute(fs::path(path), error);
    if (error) return path;
    fs::path canonical = fs::weakly_canonical(absolute, error);
    return error ? absolute.string() : canonical.string();
}

std::string ReadTextFile(const std::string& path,
                         const std::string& description) {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        throw std::runtime_error("Cannot open " + description + ": " + path);
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    if (input.bad()) {
        throw std::runtime_error("Cannot read " + description + ": " + path);
    }
    const std::string result = contents.str();
    if (result.empty()) {
        throw std::runtime_error(description + " is empty: " + path);
    }
    return result;
}

using Json = nlohmann::json;

Json RequireMetadataField(const Json& metadata, const std::string& key) {
    if (!metadata.contains(key)) {
        throw std::runtime_error("Runtime metadata is missing " + key);
    }
    return metadata.at(key);
}

std::string RequireMetadataString(const Json& metadata,
                                  const std::string& key) {
    const Json field = RequireMetadataField(metadata, key);
    if (!field.is_string()) {
        throw std::runtime_error("Runtime metadata field " + key +
                                 " must be a JSON string");
    }
    return field.get<std::string>();
}

uint64_t RequireMetadataUnsigned(const Json& metadata,
                                 const std::string& key,
                                 uint64_t minimum, uint64_t maximum) {
    const Json field = RequireMetadataField(metadata, key);
    uint64_t value = 0U;
    if (field.is_number_unsigned()) {
        value = field.get<uint64_t>();
    } else if (field.is_number_integer()) {
        const int64_t signed_value = field.get<int64_t>();
        if (signed_value < 0) {
            throw std::runtime_error("Runtime metadata field " + key +
                                     " must be non-negative");
        }
        value = static_cast<uint64_t>(signed_value);
    } else {
        throw std::runtime_error("Runtime metadata field " + key +
                                 " must be an integer");
    }
    if (value < minimum || value > maximum) {
        throw std::runtime_error("Runtime metadata field " + key +
                                 " is out of range");
    }
    return value;
}

bool RequireMetadataBool(const Json& metadata, const std::string& key) {
    const Json field = RequireMetadataField(metadata, key);
    if (!field.is_boolean()) {
        throw std::runtime_error("Runtime metadata field " + key +
                                 " must be boolean");
    }
    return field.get<bool>();
}

double RequireMetadataNumber(const Json& metadata,
                             const std::string& key,
                             double minimum, double maximum) {
    const Json field = RequireMetadataField(metadata, key);
    if (!field.is_number()) {
        throw std::runtime_error("Runtime metadata field " + key +
                                 " must be numeric");
    }
    const double value = field.get<double>();
    if (!std::isfinite(value) || value < minimum || value > maximum) {
        throw std::runtime_error("Runtime metadata field " + key +
                                 " is out of range");
    }
    return value;
}

void RequireMetadataNull(const Json& metadata, const std::string& key) {
    if (!RequireMetadataField(metadata, key).is_null()) {
        throw std::runtime_error("Runtime metadata field " + key +
                                 " must be null in this threshold mode");
    }
}

void ValidateRuntimeMetadataAgainstConfig(
    const Json& metadata, const DAQHardwareSettings& settings) {
    const Json hardware = RequireMetadataField(metadata, "hardware");
    const Json channels = RequireMetadataField(metadata, "channels");
    if (!hardware.is_object() || !channels.is_array()) {
        throw std::runtime_error(
            "Runtime metadata hardware/channels schema is malformed");
    }

    for (const char* key : {"model", "roc_firmware", "amc_firmware"}) {
        if (RequireMetadataString(hardware, key).empty()) {
            throw std::runtime_error(
                std::string("Runtime metadata hardware field is empty: ") + key);
        }
    }
    const std::string board_model =
        RequireMetadataString(hardware, "model");
    const uint64_t board_serial =
        RequireMetadataUnsigned(hardware, "serial_number", 0U, UINT32_MAX);
    const uint64_t input_range =
        RequireMetadataUnsigned(hardware, "input_range_mvpp", 500U, 2000U);
    const uint64_t adc_bits =
        RequireMetadataUnsigned(hardware, "adc_bits", 14U, 14U);
    const uint64_t dc_offset_bits =
        RequireMetadataUnsigned(hardware, "dc_offset_dac_bits", 16U, 16U);
    (void)dc_offset_bits;
    const uint64_t schema = RequireMetadataUnsigned(
        metadata, "schema_version", 1U, 2U);
    std::optional<uint32_t> latest_status_register;
    std::optional<uint32_t> latest_failure_register;
    if (schema >= 2U) {
        if (RequireMetadataNumber(hardware, "trigger_time_tag_raw_lsb_ns",
                                  0.0, 1000.0) !=
                dt5730_timing::kTriggerTimeTagRawLsbNs ||
            RequireMetadataNumber(
                hardware, "trigger_time_tag_observable_resolution_ns", 0.0,
                1000.0) !=
                dt5730_timing::kTriggerTimeTagObservableResolutionNs ||
            RequireMetadataNumber(hardware, "adc_sample_period_ns", 0.0,
                                  1000.0) !=
                dt5730_timing::kAdcSamplePeriodNs ||
            RequireMetadataBool(hardware,
                                "dead_time_measurement_available") ||
            RequireMetadataString(hardware, "dead_time_method") !=
                "unavailable_no_hardware_busy_or_livetime_scaler") {
            throw std::runtime_error(
                "Runtime metadata v2 timing/dead-time semantics are invalid");
        }
        latest_status_register = static_cast<uint32_t>(
            RequireMetadataUnsigned(
                hardware, "latest_acquisition_status_register", 0U,
                UINT32_MAX));
        latest_failure_register = static_cast<uint32_t>(
            RequireMetadataUnsigned(
                hardware, "latest_board_failure_status_register", 0U,
                UINT32_MAX));
    }
    const uint64_t clock_source =
        RequireMetadataUnsigned(hardware, "clock_source", 0U, 1U);
    const uint64_t clock_readback =
        RequireMetadataUnsigned(hardware, "clock_source_readback", 0U, 1U);
    const uint64_t run_sync =
        RequireMetadataUnsigned(hardware, "run_sync_mode", 0U, 4U);
    const uint64_t run_sync_readback =
        RequireMetadataUnsigned(hardware, "run_sync_mode_readback", 0U, 4U);
    if (schema >= 2U) {
        const auto status = dt5730_status::DecodeAcquisitionStatus(
            *latest_status_register);
        const auto failure = dt5730_status::DecodeBoardFailureStatus(
            *latest_failure_register);
        const bool observed_external =
            status.clock_source == dt5730_status::ClockSource::kExternal;
        const bool expected_external = clock_source != 0U;
        if (status.run || status.event_full ||
            status.HasFatalHealthFault() || failure.Any() ||
            observed_external != expected_external) {
            throw std::runtime_error(
                "Runtime metadata v2 latest board-health registers are "
                "incompatible with a verified stopped/completed acquisition");
        }
    }
    const std::string polarity =
        RequireMetadataString(hardware, "trigger_polarity");
    if (polarity != "falling" && polarity != "rising") {
        throw std::runtime_error(
            "Runtime metadata trigger_polarity must be falling or rising");
    }
    const uint32_t record_mask = static_cast<uint32_t>(
        RequireMetadataUnsigned(hardware, "record_mask", 1U, 0xFFU));
    const uint32_t record_mask_readback = static_cast<uint32_t>(
        RequireMetadataUnsigned(hardware, "record_mask_readback", 1U,
                                0xFFU));
    const uint32_t record_length = static_cast<uint32_t>(
        RequireMetadataUnsigned(hardware, "record_length", 128U, 102400U));
    const uint32_t post_trigger = static_cast<uint32_t>(
        RequireMetadataUnsigned(hardware, "post_trigger_percent", 0U, 100U));
    if (schema >= 2U) {
        const uint32_t post_trigger_readback = static_cast<uint32_t>(
            RequireMetadataUnsigned(hardware,
                                    "post_trigger_readback_percent", 0U,
                                    100U));
        if (post_trigger_readback != post_trigger) {
            throw std::runtime_error(
                "Runtime post-trigger readback differs from the request");
        }
    }
    const uint32_t external_mode = static_cast<uint32_t>(
        RequireMetadataUnsigned(hardware, "external_trigger_mode", 0U, 1U));
    const uint32_t self_mode = static_cast<uint32_t>(
        RequireMetadataUnsigned(hardware, "self_trigger_mode", 0U, 1U));
    const uint32_t self_mask = static_cast<uint32_t>(
        RequireMetadataUnsigned(hardware, "self_trigger_mask", 0U, 0xFFU));
    const std::string pair_logic =
        RequireMetadataString(hardware, "pair_logic");
    const bool explicit_routing =
        RequireMetadataBool(hardware, "explicit_trigger_routing");
    const uint32_t global_readback = static_cast<uint32_t>(
        RequireMetadataUnsigned(hardware, "global_trigger_mask_readback",
                                0U, UINT32_MAX));
    const Json pair_readbacks =
        RequireMetadataField(hardware, "pair_logic_readback");
    if (!pair_readbacks.is_array() || pair_readbacks.size() != 4U) {
        throw std::runtime_error(
            "Runtime metadata pair_logic_readback must contain four fields");
    }

    const std::string configured_polarity =
        settings.trigger_polarity == 1 ? "falling" : "rising";
    const std::string configured_pair_logic =
        settings.pair_logic == DAQPairLogic::kAnd ? "AND" : "OR";
    if (input_range != settings.input_range_mv ||
        adc_bits != settings.adc_bits ||
        clock_source != static_cast<uint64_t>(settings.clock_source) ||
        clock_readback != clock_source ||
        run_sync != static_cast<uint64_t>(settings.run_sync_mode) ||
        run_sync_readback != run_sync || polarity != configured_polarity ||
        record_mask != settings.channel_mask ||
        record_mask_readback != record_mask ||
        record_length != settings.record_length ||
        record_length % 8U != 0U || post_trigger != settings.post_trigger ||
        external_mode != static_cast<uint32_t>(settings.ext_trigger_mode) ||
        self_mode != static_cast<uint32_t>(settings.self_trigger_mode) ||
        self_mask != settings.self_trigger_mask ||
        (self_mask & ~record_mask) != 0U ||
        pair_logic != configured_pair_logic ||
        explicit_routing != settings.explicit_trigger_routing) {
        throw std::runtime_error(
            "Runtime hardware metadata does not match the frozen config/readback");
    }
    if (schema >= 2U) {
        if (RequireMetadataString(hardware, "connection_type") !=
                settings.connection.type ||
            RequireMetadataUnsigned(hardware, "connection_link", 0U, 127U) !=
                static_cast<uint64_t>(settings.connection.link) ||
            RequireMetadataUnsigned(hardware, "connection_node", 0U,
                                    UINT32_MAX) !=
                static_cast<uint64_t>(settings.connection.node) ||
            RequireMetadataUnsigned(hardware, "connection_base_address", 0U,
                                    UINT32_MAX) !=
                settings.connection.base_address ||
            RequireMetadataString(hardware, "expected_model") !=
                settings.connection.expected_model ||
            board_model.rfind(settings.connection.expected_model, 0U) != 0U) {
            throw std::runtime_error(
                "Runtime board connection/identity metadata does not match "
                "the frozen config");
        }
        const Json expected_serial_field =
            RequireMetadataField(hardware, "expected_serial");
        if (settings.connection.has_expected_serial) {
            const uint64_t expected_serial = RequireMetadataUnsigned(
                hardware, "expected_serial", 1U, UINT32_MAX);
            if (expected_serial != settings.connection.expected_serial ||
                board_serial != expected_serial) {
                throw std::runtime_error(
                    "Runtime board serial identity does not match the frozen "
                    "config");
            }
        } else if (!expected_serial_field.is_null()) {
            throw std::runtime_error(
                "Runtime metadata unexpectedly claims an ExpectedSerial");
        }
        const auto& dsp = settings.software_dsp.waveform;
        if (RequireMetadataUnsigned(hardware, "waveform_dsp_schema", 1U,
                                    1U) != 1U ||
            RequireMetadataUnsigned(hardware, "dsp_baseline_samples", 1U,
                                    UINT32_MAX) != dsp.baseline_samples ||
            RequireMetadataUnsigned(hardware, "dsp_short_gate_samples", 1U,
                                    UINT32_MAX) != dsp.short_gate_samples ||
            RequireMetadataUnsigned(hardware, "dsp_long_gate_samples", 1U,
                                    UINT32_MAX) != dsp.long_gate_samples ||
            std::abs(RequireMetadataNumber(
                         hardware, "dsp_pulse_start_threshold_adc", 0.0,
                         16383.0) -
                     dsp.pulse_start_threshold_adc) > 1e-12 ||
            RequireMetadataUnsigned(
                hardware, "software_coincidence_window_ns", 1U,
                UINT32_MAX) != settings.software_dsp.coincidence_window_ns) {
            throw std::runtime_error(
                "Runtime SoftwareDSP metadata does not match the frozen config");
        }
    }
    if (schema >= 2U) {
        const uint64_t expected_event_bytes =
            sizeof(EventHeader) +
            2U * static_cast<uint64_t>(record_length) *
                static_cast<uint64_t>(__builtin_popcount(record_mask));
        if (RequireMetadataUnsigned(metadata, "raw_event_bytes", 1U,
                                    UINT64_MAX) != expected_event_bytes) {
            throw std::runtime_error(
                "Runtime metadata v2 raw_event_bytes differs from config");
        }
        const Json storage = RequireMetadataField(metadata, "storage");
        if (!storage.is_object() ||
            RequireMetadataUnsigned(storage, "minimum_free_bytes", 0U,
                                    UINT64_MAX) !=
                settings.storage.minimum_free_bytes ||
            RequireMetadataUnsigned(storage, "stop_free_bytes", 0U,
                                    UINT64_MAX) !=
                settings.storage.stop_free_bytes) {
            throw std::runtime_error(
                "Runtime metadata v2 storage watermarks differ from config");
        }
        const uint64_t recorded_events = RequireMetadataUnsigned(
            metadata, "recorded_events", 0U, UINT64_MAX);
        const uint64_t lost_events = RequireMetadataUnsigned(
            metadata, "lost_events", 0U, UINT64_MAX);
        if (cpnr::LostEventPolicyExceeded(
                recorded_events, lost_events,
                settings.lost_event_policy)) {
            throw std::runtime_error(
                "Runtime metadata completed a run that exceeds the "
                "configured accepted-trigger loss policy");
        }
    }

    uint32_t expected_pair_requests = 0U;
    for (int pair = 0; pair < 4; ++pair) {
        const uint32_t pair_bits = (self_mask >> (pair * 2)) & 0x3U;
        if (pair_bits != 0U) expected_pair_requests |= 1U << pair;
        const uint32_t pair_readback = static_cast<uint32_t>(
            RequireMetadataUnsigned(
                Json{{"value", pair_readbacks.at(pair)}}, "value", 0U, 7U));
        if (explicit_routing && pair_bits != 0U) {
            const uint32_t expected_pair_field =
                pair_bits == 1U ? 5U
                : pair_bits == 2U ? 6U
                : pair_logic == "AND" ? 4U : 7U;
            if (pair_readback != expected_pair_field) {
                throw std::runtime_error(
                    "Runtime pair trigger readback does not match requested logic");
            }
        }
    }
    const uint32_t expected_sources =
        expected_pair_requests |
        (external_mode != 0U ? (1U << 30) : 0U) |
        (!explicit_routing ? (1U << 31) : 0U);
    if ((global_readback & 0xC000000FU) != expected_sources ||
        (global_readback & (0x7U << 24)) != 0U) {
        throw std::runtime_error(
            "Runtime global trigger readback does not match trigger sources");
    }

    std::size_t expected_channel_count = 0U;
    bool measured_batch_expected = false;
    for (int ch = 0; ch < MAX_CH; ++ch) {
        if (((record_mask >> ch) & 1U) != 0U) ++expected_channel_count;
        if (((self_mask >> ch) & 1U) != 0U &&
            settings.channels[ch].threshold_is_relative_mv) {
            measured_batch_expected = true;
        }
    }
    if (channels.size() != expected_channel_count) {
        throw std::runtime_error(
            "Runtime metadata channel count does not match record_mask");
    }

    std::set<uint32_t> seen_channels;
    for (const Json& channel : channels) {
        if (!channel.is_object()) {
            throw std::runtime_error(
                "Runtime metadata channel entries must be objects");
        }
        const uint32_t ch = static_cast<uint32_t>(
            RequireMetadataUnsigned(channel, "channel", 0U, MAX_CH - 1U));
        if (!seen_channels.insert(ch).second ||
            ((record_mask >> ch) & 1U) == 0U) {
            throw std::runtime_error(
                "Runtime metadata has a duplicate or disabled channel entry");
        }
        const bool trigger_enabled =
            RequireMetadataBool(channel, "trigger_enabled");
        const bool expected_trigger = ((self_mask >> ch) & 1U) != 0U;
        const uint32_t expected_range_readback = input_range == 500U ? 1U : 0U;
        const uint32_t expected_range_register = 0x1028U + 0x100U * ch;
        if (trigger_enabled != expected_trigger ||
            RequireMetadataUnsigned(channel, "input_range_register", 0U,
                                    UINT32_MAX) != expected_range_register ||
            RequireMetadataUnsigned(channel, "input_range_readback", 0U, 1U) !=
                expected_range_readback ||
            RequireMetadataUnsigned(channel, "requested_dc_offset", 0U,
                                    65535U) != settings.channels[ch].dc_offset ||
            RequireMetadataUnsigned(channel, "readback_dc_offset", 0U,
                                    65535U) != settings.channels[ch].dc_offset ||
            RequireMetadataString(channel, "polarity_readback") != polarity) {
            throw std::runtime_error(
                "Runtime channel analog/readback metadata does not match config");
        }

        const std::string threshold_mode =
            RequireMetadataString(channel, "threshold_mode");
        const std::string expected_mode =
            !expected_trigger
                ? "not_used_record_only"
                : settings.channels[ch].threshold_is_relative_mv
                      ? "baseline_relative_mv"
                      : "legacy_absolute_adc";
        if (threshold_mode != expected_mode) {
            throw std::runtime_error(
                "Runtime channel threshold_mode does not match config");
        }

        const Json baseline_field =
            RequireMetadataField(channel, "measured_baseline_adc");
        double baseline = 0.0;
        if (measured_batch_expected) {
            baseline = RequireMetadataNumber(
                channel, "measured_baseline_adc", 0.0, 16383.0);
        } else if (!baseline_field.is_null()) {
            baseline = RequireMetadataNumber(
                channel, "measured_baseline_adc", 0.0, 16383.0);
        }

        if (!expected_trigger) {
            RequireMetadataNull(channel, "requested_threshold_mv");
            RequireMetadataNull(channel, "delta_adc");
            RequireMetadataNull(channel, "written_threshold_adc");
            RequireMetadataNull(channel, "readback_threshold_adc");
            RequireMetadataNull(channel, "effective_threshold_mv");
            continue;
        }

        const uint32_t written = static_cast<uint32_t>(
            RequireMetadataUnsigned(channel, "written_threshold_adc", 0U,
                                    16383U));
        const uint32_t readback = static_cast<uint32_t>(
            RequireMetadataUnsigned(channel, "readback_threshold_adc", 0U,
                                    16383U));
        if (written != readback) {
            throw std::runtime_error(
                "Runtime threshold write/readback metadata differs");
        }
        if (settings.channels[ch].threshold_is_relative_mv) {
            const double requested = RequireMetadataNumber(
                channel, "requested_threshold_mv", 0.0,
                static_cast<double>(input_range));
            const uint32_t delta = static_cast<uint32_t>(
                RequireMetadataUnsigned(channel, "delta_adc", 1U, 16383U));
            const uint32_t expected_delta = static_cast<uint32_t>(std::llround(
                requested * 16384.0 / static_cast<double>(input_range)));
            const long long rounded_baseline = std::llround(baseline);
            const long long expected_written =
                polarity == "falling"
                    ? rounded_baseline - static_cast<long long>(delta)
                    : rounded_baseline + static_cast<long long>(delta);
            const double effective = RequireMetadataNumber(
                channel, "effective_threshold_mv", 0.0,
                static_cast<double>(input_range));
            const double expected_effective =
                std::abs(baseline - static_cast<double>(written)) *
                static_cast<double>(input_range) / 16384.0;
            if (std::abs(requested -
                         settings.channels[ch].trigger_threshold_mv) > 1e-9 ||
                delta != expected_delta || expected_written != written ||
                std::abs(effective - expected_effective) > 1e-6) {
                throw std::runtime_error(
                    "Runtime baseline-relative threshold metadata is inconsistent");
            }
        } else {
            RequireMetadataNull(channel, "requested_threshold_mv");
            RequireMetadataNull(channel, "delta_adc");
            RequireMetadataNull(channel, "effective_threshold_mv");
            if (written != settings.channels[ch].trigger_threshold) {
                throw std::runtime_error(
                    "Runtime legacy absolute threshold does not match config");
            }
        }
    }
}

void ValidateCompletedRuntimeMetadataV2(const Json& metadata) {
    if (RequireMetadataString(metadata, "acquisition_status") !=
        "completed") {
        throw std::runtime_error(
            "Runtime metadata v2 must report acquisition_status=completed");
    }
    const std::string termination_reason =
        RequireMetadataString(metadata, "termination_reason");
    const std::set<std::string> completed_reasons = {
        "event_limit", "time_limit", "operator_stop", "completed"};
    if (completed_reasons.count(termination_reason) == 0U) {
        throw std::runtime_error(
            "Runtime metadata v2 has an invalid completed termination_reason");
    }
    RequireMetadataNull(metadata, "failure_reason");
    RequireMetadataNull(metadata, "raw_finalization_error");

    const uint64_t requested_max_events = RequireMetadataUnsigned(
        metadata, "requested_max_events", 0U, UINT64_MAX);
    const uint64_t requested_run_time = RequireMetadataUnsigned(
        metadata, "requested_run_time_sec", 0U, UINT64_MAX);
    const uint64_t hardware_verified = RequireMetadataUnsigned(
        metadata, "hardware_verified_unix_time", 1U, UINT64_MAX);
    const uint64_t acquisition_start = RequireMetadataUnsigned(
        metadata, "acquisition_start_unix_time", 1U, UINT64_MAX);
    const uint64_t acquisition_end = RequireMetadataUnsigned(
        metadata, "acquisition_end_unix_time", 1U, UINT64_MAX);
    const uint64_t created = RequireMetadataUnsigned(
        metadata, "created_unix_time", 1U, UINT64_MAX);
    if (hardware_verified > acquisition_start ||
        acquisition_start > acquisition_end || acquisition_end > created) {
        throw std::runtime_error(
            "Runtime metadata v2 timestamps are not ordered");
    }

    const uint64_t recorded_events = RequireMetadataUnsigned(
        metadata, "recorded_events", 0U, UINT64_MAX);
    RequireMetadataUnsigned(metadata, "lost_events", 0U, UINT64_MAX);
    if (termination_reason == "event_limit" &&
        (requested_max_events == 0U ||
         recorded_events != requested_max_events)) {
        throw std::runtime_error(
            "Runtime metadata v2 event-limit termination/count mismatch");
    }
    if (termination_reason == "time_limit" && requested_run_time == 0U) {
        throw std::runtime_error(
            "Runtime metadata v2 time-limit termination lacks a time limit");
    }
    if (requested_max_events != 0U &&
        recorded_events > requested_max_events) {
        throw std::runtime_error(
            "Runtime metadata v2 recorded_events exceeds its request");
    }

    const std::string raw_path =
        RequireMetadataString(metadata, "raw_output_path");
    const std::string requested_raw_path =
        RequireMetadataString(metadata, "requested_raw_output_path");
    if (!fs::path(raw_path).is_absolute() ||
        !fs::path(requested_raw_path).is_absolute() ||
        raw_path != requested_raw_path) {
        throw std::runtime_error(
            "Runtime metadata v2 completed raw paths are not identical absolute paths");
    }
    if (!RequireMetadataBool(metadata, "raw_output_published") ||
        !RequireMetadataBool(metadata, "raw_output_finalized")) {
        throw std::runtime_error(
            "Runtime metadata v2 completed run is not finalized and published");
    }
    const std::string digest_method =
        RequireMetadataString(metadata, "raw_digest_method");
    if (digest_method !=
        "streaming_sha256_verified_by_descriptor_sha256") {
        throw std::runtime_error(
            "Runtime metadata v2 completed run lacks dual raw-digest verification");
    }
    if (RequireMetadataBool(metadata, "raw_recovery_performed")) {
        throw std::runtime_error(
            "Runtime metadata v2 cannot publish a recovered prefix as completed");
    }
    RequireMetadataNull(metadata, "raw_events_before_recovery");
    if (!RequireMetadataBool(metadata, "lost_events_exact")) {
        throw std::runtime_error(
            "Runtime metadata v2 completed lost-event count is not exact");
    }
    RequireMetadataUnsigned(metadata, "raw_format_version", 1U, 1U);
    RequireMetadataUnsigned(metadata, "raw_event_header_bytes",
                            sizeof(EventHeader), sizeof(EventHeader));
    const uint64_t raw_event_bytes = RequireMetadataUnsigned(
        metadata, "raw_event_bytes", sizeof(EventHeader) + sizeof(uint16_t),
        UINT64_MAX);
    const uint64_t raw_size = RequireMetadataUnsigned(
        metadata, "raw_output_size_bytes", 0U, UINT64_MAX);
    const uint64_t complete_offset = RequireMetadataUnsigned(
        metadata, "last_complete_offset", 0U, UINT64_MAX);
    if (raw_size != complete_offset || raw_size % raw_event_bytes != 0U ||
        recorded_events != raw_size / raw_event_bytes) {
        throw std::runtime_error(
            "Runtime metadata v2 raw size/event-boundary accounting is inconsistent");
    }

    const Json storage = RequireMetadataField(metadata, "storage");
    if (!storage.is_object()) {
        throw std::runtime_error(
            "Runtime metadata v2 storage must be an object");
    }
    const uint64_t free_at_start = RequireMetadataUnsigned(
        storage, "free_bytes_at_start", 0U, UINT64_MAX);
    RequireMetadataUnsigned(storage, "free_bytes_at_end", 0U, UINT64_MAX);
    const uint64_t expected_raw = RequireMetadataUnsigned(
        storage, "expected_raw_bytes", 0U, UINT64_MAX);
    const uint64_t minimum_free = RequireMetadataUnsigned(
        storage, "minimum_free_bytes", 0U, UINT64_MAX);
    const uint64_t stop_free = RequireMetadataUnsigned(
        storage, "stop_free_bytes", 0U, UINT64_MAX);
    if (minimum_free < stop_free ||
        expected_raw > UINT64_MAX - minimum_free ||
        free_at_start < expected_raw + minimum_free) {
        throw std::runtime_error(
            "Runtime metadata v2 storage preflight accounting is inconsistent");
    }
    const uint64_t expected_from_request =
        requested_max_events == 0U
            ? 0U
            : requested_max_events <= UINT64_MAX / raw_event_bytes
                  ? requested_max_events * raw_event_bytes
                  : throw std::runtime_error(
                        "Runtime metadata v2 requested raw size overflows");
    if (expected_raw != expected_from_request) {
        throw std::runtime_error(
            "Runtime metadata v2 expected raw size differs from the request");
    }

    const Json counters = RequireMetadataField(metadata, "runtime_counters");
    if (!counters.is_object()) {
        throw std::runtime_error(
            "Runtime metadata v2 runtime_counters must be an object");
    }
    RequireMetadataUnsigned(counters, "readout_errors", 0U, UINT64_MAX);
    const uint64_t health_checks = RequireMetadataUnsigned(
        counters, "health_checks", 1U, UINT64_MAX);
    RequireMetadataUnsigned(counters, "health_read_errors", 0U,
                            health_checks);
    RequireMetadataUnsigned(counters, "zmq_nonblocking_send_failures", 0U,
                            UINT64_MAX);
    RequireMetadataUnsigned(counters, "zmq_send_errors", 0U, UINT64_MAX);
    const uint64_t hwm_messages = RequireMetadataUnsigned(
        counters, "zmq_send_hwm_messages", 1U, UINT64_MAX);
    const uint64_t hwm_bytes = RequireMetadataUnsigned(
        counters, "zmq_send_hwm_approx_bytes", raw_event_bytes, UINT64_MAX);
    if (hwm_messages > UINT64_MAX / raw_event_bytes ||
        hwm_bytes != hwm_messages * raw_event_bytes ||
        hwm_bytes > 64U * 1024U * 1024U) {
        throw std::runtime_error(
            "Runtime metadata v2 ZeroMQ watermark accounting is inconsistent");
    }
    RequireMetadataUnsigned(counters, "runtime_configuration_checks", 1U,
                            UINT64_MAX);
    if (RequireMetadataString(counters, "subscriber_delivery_evidence") !=
        "unavailable_pub_socket_may_drop_silently") {
        throw std::runtime_error(
            "Runtime metadata v2 overstates ZeroMQ subscriber-delivery evidence");
    }
    const Json temperatures =
        RequireMetadataField(counters, "max_temperature_c");
    if (!temperatures.is_array() || temperatures.size() != MAX_CH) {
        throw std::runtime_error(
            "Runtime metadata v2 max_temperature_c must have eight entries");
    }
    const Json hardware = RequireMetadataField(metadata, "hardware");
    const uint32_t record_mask = static_cast<uint32_t>(
        RequireMetadataUnsigned(hardware, "record_mask", 1U, 0xFFU));
    for (std::size_t channel = 0U; channel < temperatures.size(); ++channel) {
        const Json& value = temperatures.at(channel);
        const bool active = ((record_mask >> channel) & 1U) != 0U;
        if (value.is_null()) {
            if (active) {
                throw std::runtime_error(
                    "Runtime metadata v2 lacks a temperature observation for "
                    "an active channel");
            }
            continue;
        }
        const Json wrapper = {{"temperature", value}};
        const double temperature = RequireMetadataNumber(
            wrapper, "temperature", 0.0, 150.0);
        if (active && temperature >= 82.0) {
            throw std::runtime_error(
                "Runtime metadata v2 completed run reached the 82 C software "
                "shutdown limit");
        }
    }

    const uint32_t record_length = static_cast<uint32_t>(
        RequireMetadataUnsigned(hardware, "record_length", 1U,
                                UINT32_MAX));
    const Json timing = RequireMetadataField(metadata, "timing_summary");
    if (!timing.is_object()) {
        throw std::runtime_error(
            "Runtime metadata v2 timing_summary must be an object");
    }
    const double expected_window =
        dt5730_timing::RecordedWindowSumSeconds(recorded_events,
                                                record_length);
    const double observed_window = RequireMetadataNumber(
        timing, "recorded_window_sum_sec", 0.0,
        std::numeric_limits<double>::max());
    const auto approximately_equal = [](double left, double right) {
        return std::abs(left - right) <=
               std::max(1.0e-12,
                        1.0e-9 * std::max(std::abs(left), std::abs(right)));
    };
    if (!approximately_equal(observed_window, expected_window)) {
        throw std::runtime_error(
            "Runtime metadata v2 recorded-window sum is inconsistent");
    }
    const Json first_field = RequireMetadataField(timing,
                                                  "first_extended_ttt");
    const Json last_field = RequireMetadataField(timing,
                                                 "last_extended_ttt");
    const Json elapsed_field = RequireMetadataField(timing,
                                                    "elapsed_time_sec");
    const Json ratio_field = RequireMetadataField(
        timing, "recorded_window_to_elapsed_pct");
    const Json rate_field = RequireMetadataField(
        timing, "average_recorded_event_rate_hz");
    if (recorded_events == 0U) {
        if (!first_field.is_null() || !last_field.is_null() ||
            !elapsed_field.is_null() || !ratio_field.is_null() ||
            !rate_field.is_null()) {
            throw std::runtime_error(
                "Runtime metadata v2 empty run has non-null TTT timing");
        }
    } else {
        const uint64_t first = RequireMetadataUnsigned(
            timing, "first_extended_ttt", 0U, UINT64_MAX);
        const uint64_t last = RequireMetadataUnsigned(
            timing, "last_extended_ttt", first, UINT64_MAX);
        const double elapsed = RequireMetadataNumber(
            timing, "elapsed_time_sec", 0.0,
            std::numeric_limits<double>::max());
        const double expected_elapsed =
            dt5730_timing::ElapsedSeconds(first, last);
        if (!approximately_equal(elapsed, expected_elapsed)) {
            throw std::runtime_error(
                "Runtime metadata v2 TTT elapsed time is inconsistent");
        }
        if (elapsed > 0.0) {
            const double ratio = RequireMetadataNumber(
                timing, "recorded_window_to_elapsed_pct", 0.0,
                std::numeric_limits<double>::max());
            const double rate = RequireMetadataNumber(
                timing, "average_recorded_event_rate_hz", 0.0,
                std::numeric_limits<double>::max());
            if (!approximately_equal(
                    ratio,
                    dt5730_timing::RecordedWindowToElapsedPercent(
                        expected_window, elapsed)) ||
                !approximately_equal(
                    rate, dt5730_timing::AverageRecordedEventRateHz(
                              recorded_events, elapsed))) {
                throw std::runtime_error(
                    "Runtime metadata v2 timing ratios are inconsistent");
            }
        } else if (!ratio_field.is_null() || !rate_field.is_null()) {
            throw std::runtime_error(
                "Runtime metadata v2 zero elapsed interval must not claim a rate");
        }
    }
}

Json ParseRuntimeMetadata(const std::string& contents) {
    std::vector<std::set<std::string>> object_keys;
    std::string duplicate_key;
    const Json::parser_callback_t callback =
        [&](int, Json::parse_event_t event, Json& parsed) {
            if (event == Json::parse_event_t::object_start) {
                object_keys.emplace_back();
            } else if (event == Json::parse_event_t::key) {
                if (object_keys.empty()) {
                    throw std::runtime_error(
                        "Malformed runtime metadata object structure");
                }
                const std::string key = parsed.get<std::string>();
                if (!object_keys.back().insert(key).second &&
                    duplicate_key.empty()) {
                    duplicate_key = key;
                }
            } else if (event == Json::parse_event_t::object_end) {
                if (object_keys.empty()) {
                    throw std::runtime_error(
                        "Malformed runtime metadata object structure");
                }
                object_keys.pop_back();
            }
            return true;
        };

    Json metadata;
    try {
        metadata = Json::parse(contents, callback, true, false);
    } catch (const nlohmann::json::exception& error) {
        throw std::runtime_error(
            "Runtime metadata is not strict JSON: " + std::string(error.what()));
    }
    if (!duplicate_key.empty()) {
        throw std::runtime_error(
            "Runtime metadata contains duplicate JSON key: " + duplicate_key);
    }
    if (!object_keys.empty() || !metadata.is_object()) {
        throw std::runtime_error("Runtime metadata root must be one JSON object");
    }

    const Json schema = RequireMetadataField(metadata, "schema_version");
    if (!schema.is_number_integer() || schema.get<int>() < 1 ||
        schema.get<int>() > 2) {
        throw std::runtime_error("Unsupported runtime metadata schema_version");
    }
    const Json run = RequireMetadataField(metadata, "run_number");
    if (!run.is_number_integer()) {
        throw std::runtime_error("Runtime metadata run_number must be an integer");
    }
    ParsePositiveRunNumber(run.dump(), "runtime metadata");
    for (const char* key : {"acquisition_status", "raw_output_path",
                            "metadata_path", "config_path", "config_sha256",
                            "source_config_path", "binary_path",
                            "binary_sha256", "git_commit", "build_timestamp"}) {
        RequireMetadataString(metadata, key);
    }
    const std::regex sha256_pattern("^[0-9a-f]{64}$");
    if (!std::regex_match(
            RequireMetadataString(metadata, "config_sha256"), sha256_pattern) ||
        !std::regex_match(
            RequireMetadataString(metadata, "binary_sha256"), sha256_pattern)) {
        throw std::runtime_error(
            "Runtime metadata SHA-256 fields must be 64 lowercase hex digits");
    }
    if (!RequireMetadataField(metadata, "hardware").is_object() ||
        !RequireMetadataField(metadata, "channels").is_array()) {
        throw std::runtime_error(
            "Runtime metadata hardware/channels schema is malformed");
    }
    const Json raw_size =
        RequireMetadataField(metadata, "raw_output_size_bytes");
    if ((!raw_size.is_number_unsigned() && !raw_size.is_number_integer()) ||
        (raw_size.is_number_integer() && raw_size.get<long long>() < 0)) {
        throw std::runtime_error(
            "Runtime metadata raw_output_size_bytes must be non-negative");
    }
    const std::string raw_sha256 =
        RequireMetadataString(metadata, "raw_output_sha256");
    if (!std::regex_match(raw_sha256, sha256_pattern)) {
        throw std::runtime_error(
            "Runtime metadata raw_output_sha256 must be 64 lowercase hex digits");
    }
    if (schema.get<int>() >= 2) {
        ValidateCompletedRuntimeMetadataV2(metadata);
    }
    return metadata;
}

int RunNumberFromMetadata(const Json& metadata) {
    return ParsePositiveRunNumber(
        RequireMetadataField(metadata, "run_number").dump(),
        "runtime metadata");
}

bool MetadataPathWasRelocated(const Json& metadata,
                              const std::string& field_name,
                              const std::string& selected_path) {
    const std::string recorded = RequireMetadataString(metadata, field_name);
    return AbsolutePath(recorded) != AbsolutePath(selected_path);
}

std::optional<int> RunNumberFromFilename(const std::string& input_file) {
    const std::string filename = fs::path(input_file).filename().string();
    const std::regex run_token(
        R"((^|[^A-Za-z0-9])run[_-]?([0-9]+)([^0-9]|$))",
        std::regex_constants::icase);
    std::smatch match;
    if (!std::regex_search(filename, match, run_token)) return std::nullopt;
    return ParsePositiveRunNumber(match[2].str(), "input filename");
}

void RequireConsistentRunNumber(const std::optional<int>& candidate,
                                int selected,
                                const std::string& source) {
    if (candidate && *candidate != selected) {
        throw std::runtime_error(
            "Run-number mismatch: selected " + std::to_string(selected) +
            " but " + source + " says " + std::to_string(*candidate));
    }
}

std::string ExecutablePath(const char* argv0) {
    std::error_code error;
    const fs::path proc_path = fs::read_symlink("/proc/self/exe", error);
    if (!error && !proc_path.empty()) return proc_path.string();
    return AbsolutePath(argv0 == nullptr ? "" : argv0);
}

std::string CommandLine(int argc, char** argv) {
    std::ostringstream command;
    for (int i = 0; i < argc; ++i) {
        if (i != 0) command << ' ';
        command << std::quoted(argv[i] == nullptr ? "" : argv[i]);
    }
    return command.str();
}

int WriteStringObject(const char* name, const std::string& value) {
    TObjString object(value.c_str());
    return object.Write(name);
}

}  // namespace

int main(int argc, char **argv) {
    std::string input_file = "";
    std::string output_file = "";
    std::string config_file = "";
    std::string metadata_file = "";
    int debug_event_id = -1;
    std::optional<int> requested_run_number;
    bool save_waveform = false; 

    int opt;
    while ((opt = getopt(argc, argv, "i:o:c:m:r:d:w")) != -1) {
        switch (opt) {
            case 'i': input_file = optarg; break;
            case 'o': output_file = optarg; break;
            case 'c': config_file = optarg; break;
            case 'm': metadata_file = optarg; break;
            case 'r': {
                try {
                    requested_run_number =
                        ParsePositiveRunNumber(optarg, "-r command-line option");
                } catch (const std::exception& error) {
                    std::cerr << "[Error] " << error.what() << "\n";
                    return 1;
                }
                break;
            }
            case 'd': {
                size_t consumed = 0;
                try {
                    debug_event_id = std::stoi(optarg, &consumed, 10);
                } catch (const std::exception&) {
                    std::cerr << "[Error] Invalid debug event ID: " << optarg << "\n";
                    return 1;
                }
                if (consumed != std::string(optarg).size() || debug_event_id < 0) {
                    std::cerr << "[Error] Invalid debug event ID: " << optarg << "\n";
                    return 1;
                }
                break;
            }
            case 'w': save_waveform = true; break;
            default:
                std::cerr << "Usage: " << argv[0]
                          << " [input.dat] [-o output.root] [-c config.conf]"
                             " [-m runtime.run.json] [-r run_number]"
                             " [-d event_id] [-w]\n";
                return 1;
        }
    }

    if (input_file.empty() && optind < argc) input_file = argv[optind];
    if (input_file.empty()) {
        std::cerr << "Usage: " << argv[0]
                  << " [input.dat] [-o output.root] [-c config.conf]"
                     " [-m runtime.run.json] [-r run_number]"
                     " [-d event_id] [-w]\n";
        return 1;
    }

    std::string metadata_contents;
    std::string config_contents;
    Json runtime_metadata;
    uint64_t recorded_raw_size_bytes = 0U;
    std::string recorded_raw_sha256;
    uint32_t expected_record_length = 0U;
    uint32_t expected_channel_mask = 0U;
    uint64_t expected_recorded_events = 0U;
    uint64_t expected_lost_events = 0U;
    bool verify_runtime_stream_counters = false;
    DAQHardwareSettings selected_settings;
    bool trigger_is_falling = true;
    std::string recorded_raw_path;
    std::string recorded_config_path;
    std::string recorded_metadata_path;
    bool raw_path_relocated = false;
    bool config_path_relocated = false;
    bool metadata_path_relocated = false;
    int run_number = -1;
    std::string run_number_source;

    try {
        if (!IsReadableRegularFile(input_file)) {
            throw std::runtime_error("Cannot open input file: " + input_file);
        }

        {
            if (config_file.empty()) config_file = input_file + ".config.conf";
            if (metadata_file.empty()) metadata_file = input_file + ".run.json";

            if (!IsReadableRegularFile(config_file)) {
                throw std::runtime_error(
                    "Runtime config snapshot not found. Pass -c or provide " +
                    input_file + ".config.conf");
            }
            if (!IsReadableRegularFile(metadata_file)) {
                throw std::runtime_error(
                    "Runtime metadata sidecar not found. Pass -m or provide " +
                    input_file + ".run.json");
            }

            metadata_contents =
                ReadTextFile(metadata_file, "runtime metadata sidecar");
            config_contents =
                ReadTextFile(config_file, "runtime config snapshot");
            runtime_metadata = ParseRuntimeMetadata(metadata_contents);
            selected_settings = LoadDAQHardwareSettings(ConfigParser::FromText(
                config_contents, AbsolutePath(config_file)));
            ValidateRuntimeMetadataAgainstConfig(runtime_metadata,
                                                 selected_settings);
            expected_record_length = selected_settings.record_length;
            expected_channel_mask = selected_settings.channel_mask;
            verify_runtime_stream_counters =
                RequireMetadataUnsigned(runtime_metadata, "schema_version",
                                        1U, 2U) >= 2U;
            if (verify_runtime_stream_counters) {
                expected_recorded_events = RequireMetadataUnsigned(
                    runtime_metadata, "recorded_events", 0U, UINT64_MAX);
                expected_lost_events = RequireMetadataUnsigned(
                    runtime_metadata, "lost_events", 0U, UINT64_MAX);
            }
            trigger_is_falling =
                RequireMetadataString(
                    RequireMetadataField(runtime_metadata, "hardware"),
                    "trigger_polarity") == "falling";
            const std::string acquisition_status =
                RequireMetadataString(runtime_metadata, "acquisition_status");
            if (acquisition_status != "completed") {
                throw std::runtime_error(
                    "Runtime metadata does not report acquisition_status=completed");
            }
            recorded_raw_path =
                RequireMetadataString(runtime_metadata, "raw_output_path");
            recorded_config_path =
                RequireMetadataString(runtime_metadata, "config_path");
            recorded_metadata_path =
                RequireMetadataString(runtime_metadata, "metadata_path");
            if (!fs::path(recorded_raw_path).is_absolute() ||
                !fs::path(recorded_config_path).is_absolute() ||
                !fs::path(recorded_metadata_path).is_absolute()) {
                throw std::runtime_error(
                    "RunMetadata raw/config/metadata locators must be absolute paths");
            }
            raw_path_relocated = MetadataPathWasRelocated(
                runtime_metadata, "raw_output_path", input_file);
            config_path_relocated = MetadataPathWasRelocated(
                runtime_metadata, "config_path", config_file);
            metadata_path_relocated = MetadataPathWasRelocated(
                runtime_metadata, "metadata_path", metadata_file);
            const std::string recorded_config_sha256 =
                RequireMetadataString(runtime_metadata, "config_sha256");
            const std::string actual_config_sha256 = Sha256Hex(config_contents);
            if (recorded_config_sha256 != actual_config_sha256) {
                throw std::runtime_error(
                    "Runtime config SHA-256 mismatch: sidecar records " +
                    recorded_config_sha256 + ", selected snapshot is " +
                    actual_config_sha256);
            }
            if (config_path_relocated) {
                std::cerr
                    << "[Warning] Runtime config relocated: RunMetadata records "
                    << recorded_config_path << ", selected "
                    << AbsolutePath(config_file)
                    << "; selected config content SHA-256 is authenticated.\n";
            }
            if (metadata_path_relocated) {
                std::cerr
                    << "[Warning] Runtime metadata sidecar relocated: "
                       "RunMetadata records "
                    << recorded_metadata_path << ", selected "
                    << AbsolutePath(metadata_file)
                    << "; selected metadata bytes will be embedded with their "
                       "SHA-256.\n";
            }
            recorded_raw_size_bytes = RequireMetadataField(
                runtime_metadata, "raw_output_size_bytes").get<uint64_t>();
            recorded_raw_sha256 = RequireMetadataString(
                runtime_metadata, "raw_output_sha256");
            const std::optional<int> metadata_run =
                RunNumberFromMetadata(runtime_metadata);
            const std::optional<int> filename_run =
                RunNumberFromFilename(input_file);

            if (requested_run_number) {
                run_number = *requested_run_number;
                run_number_source = "-r";
            } else if (metadata_run) {
                run_number = *metadata_run;
                run_number_source = "RunMetadata";
            } else if (filename_run) {
                run_number = *filename_run;
                run_number_source = "input filename";
            } else {
                throw std::runtime_error(
                    "Cannot determine a positive run number. Pass -r, include "
                    "run_number in the runtime metadata, or use _runNNN in the "
                    "input filename");
            }

            RequireConsistentRunNumber(metadata_run, run_number, "RunMetadata");
            RequireConsistentRunNumber(filename_run, run_number,
                                       "input filename");
        }
    } catch (const std::exception& error) {
        std::cerr << "[Error] " << error.what() << "\n";
        return 1;
    }

    if (output_file.empty() && debug_event_id < 0) {
        size_t last_dot = input_file.find_last_of(".");
        size_t last_slash = input_file.find_last_of("/\\");
        if (last_dot == std::string::npos || (last_slash != std::string::npos && last_dot < last_slash)) {
            output_file = input_file + "_prod.root";
        } else {
            output_file = input_file.substr(0, last_dot) + "_prod.root";
        }
    }

    if (debug_event_id < 0) {
        output_file = AbsolutePath(output_file);
        std::error_code output_status_error;
        const auto output_status =
            fs::symlink_status(output_file, output_status_error);
        if (output_status_error &&
            output_status_error != std::errc::no_such_file_or_directory) {
            std::cerr << "[Error] Cannot inspect output ROOT path: "
                      << output_file << " (" << output_status_error.message()
                      << ")\n";
            return 1;
        }
        if (!output_status_error &&
            output_status.type() != fs::file_type::not_found) {
            std::cerr << "[Error] Output ROOT already exists; refusing to "
                         "overwrite: " << output_file << "\n";
            return 1;
        }
    }

    std::signal(SIGINT, sig_handler);
    std::signal(SIGTERM, sig_handler);

    std::ifstream ifs;
    ScopedDescriptor raw_input_descriptor;
    FileIdentity raw_input_identity;
    struct stat raw_input_open_status {};
    struct stat raw_input_authenticated_status {};
    std::vector<char> read_buffer(4 * 1024 * 1024);
    ifs.rdbuf()->pubsetbuf(read_buffer.data(), read_buffer.size());
    
    const int opened_raw_descriptor =
        ::open(input_file.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (opened_raw_descriptor < 0 ||
        ::fstat(opened_raw_descriptor, &raw_input_open_status) != 0 ||
        !S_ISREG(raw_input_open_status.st_mode) ||
        raw_input_open_status.st_size < 0) {
        const int open_error = errno;
        if (opened_raw_descriptor >= 0) ::close(opened_raw_descriptor);
        std::cerr << "[Error] Cannot securely open input file: " << input_file
                  << " (" << std::strerror(open_error) << ")\n";
        return 1;
    }
    raw_input_descriptor.reset(opened_raw_descriptor);
    raw_input_identity = {
        static_cast<uint64_t>(raw_input_open_status.st_dev),
        static_cast<uint64_t>(raw_input_open_status.st_ino)};
    if (static_cast<uint64_t>(raw_input_open_status.st_size) !=
        recorded_raw_size_bytes) {
        std::cerr << "[Error] Raw input size does not match RunMetadata\n";
        return 1;
    }
    try {
        std::cout << "[Production] Authenticating raw input SHA-256...\n";
        if (Sha256FileDescriptorHex(raw_input_descriptor.get(),
                                    recorded_raw_size_bytes) !=
            recorded_raw_sha256) {
            std::cerr << "[Error] Raw input SHA-256 does not match RunMetadata\n";
            return 1;
        }
    } catch (const std::exception& error) {
        std::cerr << "[Error] Cannot authenticate raw input: " << error.what()
                  << "\n";
        return 1;
    }
    if (::fstat(raw_input_descriptor.get(),
                &raw_input_authenticated_status) != 0 ||
        raw_input_authenticated_status.st_dev != raw_input_open_status.st_dev ||
        raw_input_authenticated_status.st_ino != raw_input_open_status.st_ino ||
        raw_input_authenticated_status.st_size != raw_input_open_status.st_size ||
        raw_input_authenticated_status.st_mtim.tv_sec !=
            raw_input_open_status.st_mtim.tv_sec ||
        raw_input_authenticated_status.st_mtim.tv_nsec !=
            raw_input_open_status.st_mtim.tv_nsec ||
        !PathMatchesIdentity(input_file, raw_input_identity)) {
        std::cerr << "[Error] Raw input changed while it was being authenticated\n";
        return 1;
    }
    if (raw_path_relocated) {
        std::cerr
            << "[Warning] Raw input relocated: RunMetadata records "
            << recorded_raw_path << ", selected " << AbsolutePath(input_file)
            << "; opened inode size and SHA-256 are authenticated.\n";
    }
    const std::string descriptor_path =
        "/proc/self/fd/" + std::to_string(raw_input_descriptor.get());
    ifs.open(descriptor_path, std::ios::binary);
    if (!ifs.is_open()) {
        std::cerr << "[Error] Cannot attach a stream to the authenticated raw "
                     "input: "
                  << input_file << "\n";
        return 1;
    }

    const size_t total_bytes =
        static_cast<size_t>(raw_input_open_status.st_size);
    size_t processed_bytes = 0;

    TApplication *app = nullptr;
    TCanvas *c1 = nullptr;
    if (debug_event_id >= 0) {
        app = new TApplication("App", &argc, argv);
        c1 = new TCanvas("c1", "Interactive Debugger", 1000, 600);
    }

    TFile *fOut = nullptr;
    TTree *tOut = nullptr;
    TemporaryRootOutput temporary_root_output;
    EventHeader header;
    
    uint32_t record_len_branch = 0; 
    std::vector<uint16_t> wave_ch[8];
    double short_charge_ch[8] = {0.0};
    double charge_ch[8] = {0.0};
    double pulse_height_ch[8] = {0.0};
    double pulse_start_time_ch[8] = {0.0}; 
    double baseline_ch[8] = {0.0}; 

    if (debug_event_id < 0) {
        try {
            temporary_root_output = ReserveTemporaryRootOutput(output_file);
            InitializeTemporaryRootReservation(&temporary_root_output);
        } catch (const std::exception& error) {
            std::cerr << "[Error] " << error.what() << "\n";
            RemoveTemporaryRootOutput(&temporary_root_output);
            return 1;
        }
        const std::string temporary_descriptor_path =
            "/proc/self/fd/" +
            std::to_string(temporary_root_output.descriptor.get());
        // Open ROOT through the still-open mkstemp descriptor.  The inode was
        // seeded with a minimal valid ROOT image above, so UPDATE preserves
        // that exact reservation instead of deleting/recreating its pathname.
        fOut = new TFile(temporary_descriptor_path.c_str(), "UPDATE");
        if (!fOut || fOut->IsZombie() ||
            !PathMatchesIdentity(temporary_root_output.path,
                                 temporary_root_output.identity)) {
            std::cerr << "[Error] Cannot create isolated temporary ROOT file "
                      << "for " << output_file << "\n";
            if (fOut) fOut->Close();
            delete fOut;
            RemoveTemporaryRootOutput(&temporary_root_output);
            return 1;
        }

        TMacro config_macro("RunConfig", "Runtime DAQ configuration");
        std::istringstream config_lines(config_contents);
        std::string config_line;
        while (std::getline(config_lines, config_line)) {
            config_macro.AddLine(config_line.c_str());
        }
        const std::string executable_path = ExecutablePath(argv[0]);
        std::string executable_sha256;
        try {
            // /proc/self/exe remains bound to the executing inode even if the
            // installed pathname is replaced while conversion is running.
            executable_sha256 = Sha256FileHex("/proc/self/exe");
        } catch (const std::exception& error) {
            std::cerr << "[Error] Cannot hash production executable: "
                      << error.what() << "\n";
            fOut->Close();
            delete fOut;
            RemoveTemporaryRootOutput(&temporary_root_output);
            return 1;
        }

        bool initial_write_failed =
            config_macro.Write("RunConfig") <= 0 ||
            WriteStringObject("RunConfigExact", config_contents) <= 0 ||
            WriteStringObject("RunConfigSha256", Sha256Hex(config_contents)) <= 0 ||
            WriteStringObject("RunMetadata", metadata_contents) <= 0 ||
            WriteStringObject("RunMetadataSha256",
                              Sha256Hex(metadata_contents)) <= 0 ||
            WriteStringObject("InputFile", AbsolutePath(input_file)) <= 0 ||
            WriteStringObject("ConfigFile", AbsolutePath(config_file)) <= 0 ||
            WriteStringObject("MetadataFile", AbsolutePath(metadata_file)) <= 0 ||
            WriteStringObject("RecordedRawOutputPath", recorded_raw_path) <= 0 ||
            WriteStringObject("ResolvedRawInputPath",
                              AbsolutePath(input_file)) <= 0 ||
            WriteStringObject("RecordedConfigPath", recorded_config_path) <= 0 ||
            WriteStringObject("ResolvedConfigPath",
                              AbsolutePath(config_file)) <= 0 ||
            WriteStringObject("RecordedMetadataPath",
                              recorded_metadata_path) <= 0 ||
            WriteStringObject("ResolvedMetadataPath",
                              AbsolutePath(metadata_file)) <= 0 ||
            WriteStringObject("ExecutablePath", executable_path) <= 0 ||
            WriteStringObject("ExecutableSha256", executable_sha256) <= 0 ||
            WriteStringObject("ExecutableBuildTime", CPNR_BUILD_TIMESTAMP) <= 0 ||
            WriteStringObject("ExecutableGitCommit", CPNR_GIT_COMMIT) <= 0 ||
            WriteStringObject("CommandLine", CommandLine(argc, argv)) <= 0 ||
            WriteStringObject("RunNumberSource", run_number_source) <= 0 ||
            fOut->TestBit(TFile::kWriteError);

        TParameter<int> p_run_num("RunNumber", run_number);
        TParameter<int> p_dsp_schema("WaveformDspSchema", 1);
        TParameter<int> p_dsp_baseline(
            "DspBaselineSamples",
            static_cast<int>(selected_settings.software_dsp.waveform
                                 .baseline_samples));
        TParameter<int> p_dsp_short(
            "DspShortGateSamples",
            static_cast<int>(selected_settings.software_dsp.waveform
                                 .short_gate_samples));
        TParameter<int> p_dsp_long(
            "DspLongGateSamples",
            static_cast<int>(selected_settings.software_dsp.waveform
                                 .long_gate_samples));
        TParameter<double> p_dsp_start(
            "DspPulseStartThresholdAdc",
            selected_settings.software_dsp.waveform
                .pulse_start_threshold_adc);
        TParameter<int> p_coincidence_window(
            "SoftwareCoincidenceWindowNs",
            static_cast<int>(selected_settings.software_dsp
                                 .coincidence_window_ns));
        if (p_run_num.Write() <= 0 || p_dsp_schema.Write() <= 0 ||
            p_dsp_baseline.Write() <= 0 || p_dsp_short.Write() <= 0 ||
            p_dsp_long.Write() <= 0 || p_dsp_start.Write() <= 0 ||
            p_coincidence_window.Write() <= 0 ||
            fOut->TestBit(TFile::kWriteError)) {
            initial_write_failed = true;
        }
        if (initial_write_failed) {
            std::cerr << "[Error] ROOT provenance write failed before conversion\n";
            fOut->Close();
            delete fOut;
            RemoveTemporaryRootOutput(&temporary_root_output);
            return 1;
        }

        tOut = new TTree("phys_tree", "DT5730 Physics Data");
        tOut->Branch("EventID", &header.EventID, "EventID/i");
        tOut->Branch("SyncTime_TTT", &header.ExtendedTTT, "SyncTime_TTT/l");
        tOut->Branch("ChannelMask", &header.ChannelMask, "ChannelMask/s");
        tOut->Branch("RecordLength", &record_len_branch, "RecordLength/i"); 
        tOut->Branch("Pattern", &header.Pattern, "Pattern/s");
        tOut->Branch("BoardEventCounter", &header.BoardEventCounter,
                     "BoardEventCounter/i");

        for (int i = 0; i < 8; ++i) {
            tOut->Branch(Form("ShortCharge_CH%d", i), &short_charge_ch[i], Form("ShortCharge_CH%d/D", i));
            tOut->Branch(Form("Charge_CH%d", i), &charge_ch[i], Form("Charge_CH%d/D", i));
            tOut->Branch(Form("PulseHeight_CH%d", i), &pulse_height_ch[i], Form("PulseHeight_CH%d/D", i));
            tOut->Branch(Form("PulseStart_T0_CH%d", i), &pulse_start_time_ch[i], Form("PulseStart_T0_CH%d/D", i));
            tOut->Branch(Form("Baseline_CH%d", i), &baseline_ch[i], Form("Baseline_CH%d/D", i)); 
            if (save_waveform) tOut->Branch(Form("Waveform_CH%d", i), &wave_ch[i]);
        }
    }

    std::vector<uint16_t> raw_waveform_buffer;
    uint64_t current_event = 0;
    
    bool is_first_event = true;
    uint64_t first_ttt = 0, last_ttt = 0, total_acquired_samples = 0;
    uint32_t prev_board_counter = 0;
    uint64_t lost_events = 0;
    bool conversion_failed = false;

    auto start_time = std::chrono::steady_clock::now();
    std::cout << "\033[1;32m[Production] Starting Universal Conversion...\033[0m\n";
    if (save_waveform) std::cout << " - Mode: Charge/Height Spectrum + Waveform Archiving (-w ON)\n";
    else std::cout << " - Mode: Charge/Height Spectrum Only (Waveform Dropped)\n";

    while (g_running && ifs.read(reinterpret_cast<char *>(&header), sizeof(EventHeader))) {
        processed_bytes += sizeof(EventHeader);
        current_event++;
        record_len_branch = header.RecordLength; 

        if (header.RecordLength < 128U || header.RecordLength > 102400U ||
            header.RecordLength % 8U != 0U ||
            header.RecordLength != expected_record_length ||
            header.ChannelMask == 0U || (header.ChannelMask & ~0xFFU) != 0U ||
            header.ChannelMask != expected_channel_mask ||
            header.BoardEventCounter > 0xFFFFFFU) {
            std::cerr << "\n[Error] Invalid/corrupt raw event header at event "
                      << current_event - 1U << "\n";
            conversion_failed = true;
            g_running = 0;
            break;
        }
        if (header.EventID != current_event - 1U) {
            std::cerr << "\n[Error] Raw EventID is not the exact zero-based "
                         "stream index at event "
                      << current_event - 1U << " (observed="
                      << header.EventID << ")\n";
            conversion_failed = true;
            g_running = 0;
            break;
        }

        if (is_first_event) {
            first_ttt = header.ExtendedTTT;
            prev_board_counter = header.BoardEventCounter;
            is_first_event = false;
        } else {
            if (header.ExtendedTTT <= last_ttt) {
                std::cerr << "\n[Error] ExtendedTTT is not strictly "
                             "increasing at event "
                          << current_event - 1U << " (previous=" << last_ttt
                          << ", current=" << header.ExtendedTTT << ")\n";
                conversion_failed = true;
                g_running = 0;
                break;
            }
            const uint32_t diff =
                (header.BoardEventCounter - prev_board_counter) & 0xFFFFFFU;
            if (diff == 0U) {
                std::cerr << "\n[Error] Duplicate/stale BoardEventCounter at event "
                          << current_event - 1U << " (counter="
                          << header.BoardEventCounter << ")\n";
                conversion_failed = true;
                g_running = 0;
                break;
            }
            if (diff > 0x800000U) {
                std::cerr << "\n[Error] Backward/reset BoardEventCounter at event "
                          << current_event - 1U << " (previous="
                          << prev_board_counter << ", current="
                          << header.BoardEventCounter << ")\n";
                conversion_failed = true;
                g_running = 0;
                break;
            }
            if (diff > 1) {
                const uint64_t newly_lost = static_cast<uint64_t>(diff - 1U);
                if (lost_events >
                    std::numeric_limits<uint64_t>::max() - newly_lost) {
                    std::cerr << "\n[Error] Lost-event counter overflow at event "
                              << current_event - 1U << "\n";
                    conversion_failed = true;
                    g_running = 0;
                    break;
                }
                lost_events += newly_lost;
            }
        }
        last_ttt = header.ExtendedTTT;
        prev_board_counter = header.BoardEventCounter;
        total_acquired_samples += header.RecordLength;

        int active_ch = 0;
        for (int i = 0; i < 8; ++i) {
            if ((header.ChannelMask >> i) & 1) active_ch++;
            wave_ch[i].clear();
            short_charge_ch[i] = 0.0;
            charge_ch[i] = 0.0;
            pulse_height_ch[i] = 0.0; 
            pulse_start_time_ch[i] = -1.0;
            baseline_ch[i] = 0.0;
        }

        size_t wave_len = header.RecordLength * active_ch;
        size_t wave_bytes_size = wave_len * sizeof(uint16_t);

        raw_waveform_buffer.resize(wave_len);
        ifs.read(reinterpret_cast<char *>(raw_waveform_buffer.data()),
                 static_cast<std::streamsize>(wave_bytes_size));
        if (ifs.gcount() != static_cast<std::streamsize>(wave_bytes_size)) {
            std::cerr << "\n[Error] Truncated waveform payload at event "
                      << current_event - 1U << ": expected "
                      << wave_bytes_size << " bytes, read " << ifs.gcount()
                      << "\n";
            conversion_failed = true;
            g_running = 0;
            break;
        }
        processed_bytes += wave_bytes_size;

        int offset = 0;
        for (int ch = 0; ch < 8; ++ch) {
            if ((header.ChannelMask >> ch) & 1) {
                uint16_t* trace_ptr = raw_waveform_buffer.data() + offset;
                size_t trace_len = header.RecordLength;

                const cpnr::WaveformDspValues dsp = cpnr::ComputeWaveformDsp(
                    trace_ptr, trace_len, trigger_is_falling,
                    selected_settings.software_dsp.waveform);
                baseline_ch[ch] = dsp.baseline;
                short_charge_ch[ch] = dsp.short_charge;
                charge_ch[ch] = dsp.charge;
                pulse_height_ch[ch] = dsp.pulse_height;
                pulse_start_time_ch[ch] = dsp.pulse_start_ns;

                if (save_waveform || (debug_event_id >= 0 && (int)header.EventID == debug_event_id)) {
                    wave_ch[ch].assign(trace_ptr, trace_ptr + trace_len);
                }
                offset += trace_len;
            }
        }

        if (tOut && tOut->Fill() < 0) {
            std::cerr << "\n[Error] ROOT tree write failed at event "
                      << current_event - 1U << "\n";
            conversion_failed = true;
            g_running = 0;
            break;
        }

        if (current_event % 2000 == 0) {
            auto now = std::chrono::steady_clock::now();
            double elapsed_sec = std::chrono::duration_cast<std::chrono::duration<double>>(now - start_time).count();
            double progress = (static_cast<double>(processed_bytes) / total_bytes) * 100.0;
            double speed_bps = processed_bytes / elapsed_sec; 
            double eta_sec = (total_bytes - processed_bytes) / speed_bps;

            std::cout << "\r\033[K" << "[Progress] " << std::fixed << std::setprecision(1) << progress << "% | "
                      << "Events: " << current_event << " | Speed: " << std::setprecision(1) << (speed_bps / 1024.0 / 1024.0) << " MB/s | "
                      << "ETA: " << (int)eta_sec << " s" << std::flush;
        }

        if (debug_event_id >= 0 && (int)header.EventID == debug_event_id && active_ch > 0) {
            int disp_ch = 0;
            for (; disp_ch < 8; ++disp_ch) {
                if ((header.ChannelMask >> disp_ch) & 1) break;
            }
            std::vector<double> x(header.RecordLength), y(header.RecordLength);
            for (size_t i = 0; i < header.RecordLength; ++i) {
                x[i] = i * 2.0; y[i] = wave_ch[disp_ch][i];
            }
            TGraph *gr = new TGraph(header.RecordLength, x.data(), y.data());
            
            gr->SetTitle(Form("Event %d (CH%d) - Charge: %.1f, Height: %.1f, T0: %.1f ns;Time (ns);ADC Value", 
                              debug_event_id, disp_ch, charge_ch[disp_ch], pulse_height_ch[disp_ch], pulse_start_time_ch[disp_ch]));
            gr->SetLineColor(kBlue); gr->SetLineWidth(2); gr->Draw("AL");

            TGraph* bl_line = new TGraph(2);
            bl_line->SetPoint(0, 0, baseline_ch[disp_ch]);
            bl_line->SetPoint(1, header.RecordLength * 2.0, baseline_ch[disp_ch]);
            bl_line->SetLineColor(kRed); bl_line->SetLineStyle(2); bl_line->SetLineWidth(2); bl_line->Draw("L SAME");

            c1->Update();
            std::cout << "\n\n\033[1;33m[Debugger] Displaying Event " << debug_event_id << " CH" << disp_ch << "\033[0m\n"
                      << "RecordLength: " << header.RecordLength << " | Baseline: " << baseline_ch[disp_ch] << "\n"
                      << "[WAITING_CMD] Ready for Python GUI Input (p/n/j/q)...\n" << std::flush; 

            std::string cmd; bool continue_debug = true;
            while (continue_debug && g_running) {
                gSystem->ProcessEvents(); 
                fd_set readfds; FD_ZERO(&readfds); FD_SET(STDIN_FILENO, &readfds);
                struct timeval timeout; timeout.tv_sec = 0; timeout.tv_usec = 100000; 

                if (select(STDIN_FILENO + 1, &readfds, NULL, NULL, &timeout) > 0) {
                    std::cin >> cmd;
                    if (cmd == "q" || cmd == "quit") {
                        debug_event_id = -1; continue_debug = false;
                        if(c1) { c1->Close(); delete c1; c1 = nullptr; }
                    } 
                    else if (cmd == "n" || cmd == "next" || cmd == "p" || cmd == "prev") {
                        debug_event_id++; continue_debug = false;
                    } 
                    else if (cmd == "j" || cmd == "jump") {
                        int target; std::cin >> target;
                        if (target >= 0 &&
                            static_cast<uint64_t>(target) > current_event) {
                            debug_event_id = target;
                            continue_debug = false;
                        }
                    }
                }
            }
            if (debug_event_id >= 0) continue; 
        }
    }

    if (!ifs.eof() && ifs.bad()) {
        std::cerr << "\n[Error] Raw input read failed\n";
        conversion_failed = true;
        g_running = 0;
    } else if (ifs.eof() && ifs.gcount() != 0 &&
               ifs.gcount() != static_cast<std::streamsize>(sizeof(EventHeader))) {
        std::cerr << "\n[Error] Trailing partial event header in raw input\n";
        conversion_failed = true;
        g_running = 0;
    }

    if (g_running) {
        struct stat raw_input_final_status {};
        if (::fstat(raw_input_descriptor.get(), &raw_input_final_status) != 0 ||
            raw_input_final_status.st_size < 0 ||
            static_cast<uint64_t>(raw_input_final_status.st_size) !=
                recorded_raw_size_bytes ||
            raw_input_final_status.st_dev != raw_input_open_status.st_dev ||
            raw_input_final_status.st_ino != raw_input_open_status.st_ino ||
            raw_input_final_status.st_mtim.tv_sec !=
                raw_input_authenticated_status.st_mtim.tv_sec ||
            raw_input_final_status.st_mtim.tv_nsec !=
                raw_input_authenticated_status.st_mtim.tv_nsec ||
            !PathMatchesIdentity(input_file, raw_input_identity)) {
            std::cerr << "\n[Error] Raw input identity/size changed during "
                         "conversion\n";
            conversion_failed = true;
            g_running = 0;
        }
    }

    if (g_running && debug_event_id < 0 && verify_runtime_stream_counters &&
        (current_event != expected_recorded_events ||
         lost_events != expected_lost_events)) {
        std::cerr << "\n[Error] Raw stream counters disagree with authenticated "
                     "RunMetadata: recorded="
                  << current_event << "/" << expected_recorded_events
                  << ", lost=" << lost_events << "/"
                  << expected_lost_events << "\n";
        conversion_failed = true;
        g_running = 0;
    }

    if (g_running && debug_event_id < 0) {
        std::cout << "\r\033[K[Progress] 100.0% | Events: " << current_event << " | Done.          \n";

        const double real_time_sec =
            dt5730_timing::ElapsedSeconds(first_ttt, last_ttt);
        const double recorded_window_sum_sec =
            static_cast<double>(total_acquired_samples) *
            (dt5730_timing::kAdcSamplePeriodNs /
             dt5730_timing::kNanosecondsPerSecond);
        
        const long double total_triggers =
            static_cast<long double>(current_event) +
            static_cast<long double>(lost_events);
        double lost_events_pct =
            (total_triggers > 0.0L)
                ? static_cast<double>(static_cast<long double>(lost_events) /
                                      total_triggers * 100.0L)
                : 0.0;
        const double recorded_window_pct =
            dt5730_timing::RecordedWindowToElapsedPercent(
                recorded_window_sum_sec, real_time_sec);
        const double avg_rate =
            dt5730_timing::AverageRecordedEventRateHz(current_event,
                                                      real_time_sec);

        std::cout << "\n\033[1;36m========== [ ROOT Conversion Summary ] ==========\033[0m\n"
                  << " - Recorded Events : " << current_event << "\n"
                  << " - Lost Events     : " << lost_events << " (" << std::fixed << std::setprecision(3) << lost_events_pct << " %, accepted-trigger counter gaps; cause not inferred)\n"
                  << " - HW Real Time    : " << std::fixed << std::setprecision(2) << real_time_sec << " sec\n"
                  << " - Record Windows  : " << std::fixed << std::setprecision(6) << recorded_window_sum_sec << " sec (sum; overlaps possible)\n"
                  << " - Window / Time   : " << std::fixed << std::setprecision(5) << recorded_window_pct << " % (not dead time)\n"
                  << " - HW Dead Time    : N/A (no hardware busy/live-time scaler)\n"
                  << " - Avg Trig Rate   : " << std::fixed << std::setprecision(2) << avg_rate << " Hz\n"
                  << "\033[1;36m=================================================\033[0m\n\n";

        if (fOut) {
            fOut->cd();
            TParameter<int> p_timing_schema("TimingSummarySchema", 2);
            TParameter<double> p_ttt_lsb(
                "TriggerTimeTagRawLsb_ns",
                dt5730_timing::kTriggerTimeTagRawLsbNs);
            TParameter<double> p_ttt_resolution(
                "TriggerTimeTagObservableResolution_ns",
                dt5730_timing::kTriggerTimeTagObservableResolutionNs);
            TParameter<double> p_adc_period(
                "ADCSamplePeriod_ns", dt5730_timing::kAdcSamplePeriodNs);
            TParameter<double> p_real("RealTime_sec", real_time_sec);
            TParameter<double> p_window_sum(
                "RecordedWindowSum_sec", recorded_window_sum_sec);
            TParameter<double> p_window_ratio(
                "RecordedWindowToElapsed_pct", recorded_window_pct);
            TParameter<int> p_dead_available("DeadTimeMeasurementAvailable",
                                             0);
            TObjString p_dead_method(
                "unavailable_no_hardware_busy_or_livetime_scaler");
            TParameter<double> p_rate("TriggerRate_Hz", avg_rate); 
            const uint64_t long64_max = static_cast<uint64_t>(
                std::numeric_limits<Long64_t>::max());
            if (lost_events > long64_max || current_event > long64_max) {
                std::cerr << "[Error] Conversion summary exceeds ROOT Long64_t range\n";
                conversion_failed = true;
            } else {
                TParameter<Long64_t> p_lost(
                    "LostEvents_count", static_cast<Long64_t>(lost_events));
                TParameter<Long64_t> p_rec(
                    "RecordedEvents_count", static_cast<Long64_t>(current_event));

                if (p_timing_schema.Write() <= 0 ||
                    p_ttt_lsb.Write() <= 0 ||
                    p_ttt_resolution.Write() <= 0 ||
                    p_adc_period.Write() <= 0 ||
                    p_real.Write() <= 0 || p_window_sum.Write() <= 0 ||
                    p_window_ratio.Write() <= 0 ||
                    p_dead_available.Write() <= 0 ||
                    p_dead_method.Write("DeadTimeMethod") <= 0 ||
                    p_lost.Write() <= 0 ||
                    p_rec.Write() <= 0 || p_rate.Write() <= 0 ||
                    fOut->TestBit(TFile::kWriteError)) {
                    std::cerr << "[Error] ROOT summary write failed\n";
                    conversion_failed = true;
                }
            }
        }
    }

    if (fOut) {
        const int final_write_result = fOut->Write();
        if (final_write_result < 0 || fOut->TestBit(TFile::kWriteError)) {
            std::cerr << "[Error] Final ROOT file write failed\n";
            conversion_failed = true;
        }
        fOut->Close();
        if (fOut->TestBit(TFile::kWriteError)) {
            std::cerr << "[Error] ROOT file close reported a write failure\n";
            conversion_failed = true;
        }
        delete fOut;
        if (!conversion_failed && g_running) {
            try {
                PublishTemporaryRootOutput(&temporary_root_output, output_file);
                std::cout << "\033[1;32m[Production] Conversion complete. Saved to \033[0m"
                          << output_file << "\n";
            } catch (const std::exception& error) {
                std::cerr << "[Error] " << error.what() << "\n";
                conversion_failed = true;
                RemoveTemporaryRootOutput(&temporary_root_output);
            }
        } else {
            RemoveTemporaryRootOutput(&temporary_root_output);
        }
    }

    if (app) delete app;
    return (!conversion_failed && g_running) ? 0 : 1;
}
