#include "EventHeader.h"
#include "ConfigParser.h"
#include "DAQConfig.h"
#include "Sha256.h"
#include <TApplication.h>
#include <TCanvas.h>
#include <TFile.h>
#include <TGraph.h>
#include <TTree.h>
#include <TMacro.h>
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

struct TemporaryRootOutput {
    std::string path;
    FileIdentity identity;
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
    int get() const { return descriptor_; }
    void reset(int descriptor) {
        if (descriptor_ >= 0) ::close(descriptor_);
        descriptor_ = descriptor;
    }

 private:
    int descriptor_ = -1;
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
        ::unlink(writable_pattern.data());
        throw std::runtime_error(
            "Cannot inspect temporary ROOT output (" +
            std::string(std::strerror(status_error)) + ")");
    }
    if (::close(descriptor) != 0) {
        const int close_error = errno;
        ::unlink(writable_pattern.data());
        throw std::runtime_error(
            "Cannot close temporary ROOT reservation (" +
            std::string(std::strerror(close_error)) + ")");
    }
    return {writable_pattern.data(),
            {static_cast<uint64_t>(status.st_dev),
             static_cast<uint64_t>(status.st_ino)}};
}

bool RemoveTemporaryRootOutput(const TemporaryRootOutput& temporary) {
    if (!temporary.path.empty() &&
        PathMatchesIdentity(temporary.path, temporary.identity)) {
        return ::unlink(temporary.path.c_str()) == 0;
    }
    return false;
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

void PublishTemporaryRootOutput(const TemporaryRootOutput& temporary,
                                const std::string& final_path) {
    if (!PathMatchesIdentity(temporary.path, temporary.identity)) {
        throw std::runtime_error(
            "Temporary ROOT output inode changed before publication");
    }
    ScopedDescriptor temporary_descriptor(
        ::open(temporary.path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
    if (temporary_descriptor.get() < 0) {
        throw std::runtime_error(
            "Cannot open temporary ROOT output for publication (" +
            std::string(std::strerror(errno)) + ")");
    }
    struct stat temporary_status {};
    if (::fstat(temporary_descriptor.get(), &temporary_status) != 0) {
        throw std::runtime_error(
            "Cannot identify temporary ROOT output for publication (" +
            std::string(std::strerror(errno)) + ")");
    }
    if (static_cast<uint64_t>(temporary_status.st_dev) !=
            temporary.identity.device ||
        static_cast<uint64_t>(temporary_status.st_ino) !=
            temporary.identity.inode) {
        throw std::runtime_error(
            "Temporary ROOT output descriptor identity changed before "
            "publication");
    }
    if (::fsync(temporary_descriptor.get()) != 0) {
        throw std::runtime_error(
            "Cannot durably sync temporary ROOT output (" +
            std::string(std::strerror(errno)) + ")");
    }

    LinkDescriptorNoReplace(temporary_descriptor.get(), final_path,
                            "ROOT output");
    if (!PathMatchesIdentity(final_path, temporary.identity)) {
        throw std::runtime_error(
            "Published ROOT output identity changed before verification");
    }
    try {
        SyncParentDirectory(final_path);
    } catch (...) {
        if (PathMatchesIdentity(final_path, temporary.identity)) {
            ::unlink(final_path.c_str());
            try {
                SyncParentDirectory(final_path);
            } catch (...) {
                // Preserve the original publication failure below.
            }
        }
        throw;
    }
    if (!PathMatchesIdentity(final_path, temporary.identity)) {
        throw std::runtime_error(
            "Published ROOT output identity changed during durable commit");
    }
    if (!RemoveTemporaryRootOutput(temporary)) {
        std::cerr << "[Warning] Published ROOT output, but temporary hard-link "
                     "cleanup was unsafe or failed: "
                  << temporary.path << "\n";
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
    RequireMetadataUnsigned(hardware, "serial_number", 0U, UINT32_MAX);
    const uint64_t input_range =
        RequireMetadataUnsigned(hardware, "input_range_mvpp", 500U, 2000U);
    const uint64_t adc_bits =
        RequireMetadataUnsigned(hardware, "adc_bits", 14U, 14U);
    const uint64_t dc_offset_bits =
        RequireMetadataUnsigned(hardware, "dc_offset_dac_bits", 16U, 16U);
    (void)dc_offset_bits;
    const uint64_t clock_source =
        RequireMetadataUnsigned(hardware, "clock_source", 0U, 1U);
    const uint64_t clock_readback =
        RequireMetadataUnsigned(hardware, "clock_source_readback", 0U, 1U);
    const uint64_t run_sync =
        RequireMetadataUnsigned(hardware, "run_sync_mode", 0U, 4U);
    const uint64_t run_sync_readback =
        RequireMetadataUnsigned(hardware, "run_sync_mode_readback", 0U, 4U);
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
    if (!schema.is_number_integer() || schema.get<int>() != 1) {
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
    return metadata;
}

int RunNumberFromMetadata(const Json& metadata) {
    return ParsePositiveRunNumber(
        RequireMetadataField(metadata, "run_number").dump(),
        "runtime metadata");
}

void RequireMetadataPath(const Json& metadata,
                         const std::string& field_name,
                         const std::string& selected_path,
                         const std::string& description) {
    const std::string recorded = RequireMetadataString(metadata, field_name);
    if (AbsolutePath(recorded) != AbsolutePath(selected_path)) {
        throw std::runtime_error(
            description + " mismatch: selected " + AbsolutePath(selected_path) +
            " but RunMetadata records " + AbsolutePath(recorded));
    }
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
            const DAQHardwareSettings selected_settings =
                LoadDAQHardwareSettings(ConfigParser::FromText(
                    config_contents, AbsolutePath(config_file)));
            ValidateRuntimeMetadataAgainstConfig(runtime_metadata,
                                                 selected_settings);
            expected_record_length = selected_settings.record_length;
            expected_channel_mask = selected_settings.channel_mask;
            const std::string acquisition_status =
                RequireMetadataString(runtime_metadata, "acquisition_status");
            if (acquisition_status != "completed") {
                throw std::runtime_error(
                    "Runtime metadata does not report acquisition_status=completed");
            }
            RequireMetadataPath(runtime_metadata, "raw_output_path",
                                input_file, "Raw input path");
            RequireMetadataPath(runtime_metadata, "config_path",
                                config_file, "Runtime config path");
            RequireMetadataPath(runtime_metadata, "metadata_path",
                                metadata_file, "Runtime metadata path");
            const std::string recorded_config_sha256 =
                RequireMetadataString(runtime_metadata, "config_sha256");
            const std::string actual_config_sha256 = Sha256Hex(config_contents);
            if (recorded_config_sha256 != actual_config_sha256) {
                throw std::runtime_error(
                    "Runtime config SHA-256 mismatch: sidecar records " +
                    recorded_config_sha256 + ", selected snapshot is " +
                    actual_config_sha256);
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
    double charge_ch[8] = {0.0};
    double pulse_height_ch[8] = {0.0};
    double pulse_start_time_ch[8] = {0.0}; 
    double baseline_ch[8] = {0.0}; 

    if (debug_event_id < 0) {
        try {
            temporary_root_output = ReserveTemporaryRootOutput(output_file);
        } catch (const std::exception& error) {
            std::cerr << "[Error] " << error.what() << "\n";
            return 1;
        }
        fOut = new TFile(temporary_root_output.path.c_str(), "RECREATE");
        if (!fOut || fOut->IsZombie() ||
            !PathMatchesIdentity(temporary_root_output.path,
                                 temporary_root_output.identity)) {
            std::cerr << "[Error] Cannot create isolated temporary ROOT file "
                      << "for " << output_file << "\n";
            if (fOut) fOut->Close();
            delete fOut;
            RemoveTemporaryRootOutput(temporary_root_output);
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
            RemoveTemporaryRootOutput(temporary_root_output);
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
            WriteStringObject("ExecutablePath", executable_path) <= 0 ||
            WriteStringObject("ExecutableSha256", executable_sha256) <= 0 ||
            WriteStringObject("ExecutableBuildTime", CPNR_BUILD_TIMESTAMP) <= 0 ||
            WriteStringObject("ExecutableGitCommit", CPNR_GIT_COMMIT) <= 0 ||
            WriteStringObject("CommandLine", CommandLine(argc, argv)) <= 0 ||
            WriteStringObject("RunNumberSource", run_number_source) <= 0 ||
            fOut->TestBit(TFile::kWriteError);

        TParameter<int> p_run_num("RunNumber", run_number);
        if (p_run_num.Write() <= 0 || fOut->TestBit(TFile::kWriteError)) {
            initial_write_failed = true;
        }
        if (initial_write_failed) {
            std::cerr << "[Error] ROOT provenance write failed before conversion\n";
            fOut->Close();
            delete fOut;
            RemoveTemporaryRootOutput(temporary_root_output);
            return 1;
        }

        tOut = new TTree("phys_tree", "DT5730 Physics Data");
        tOut->Branch("EventID", &header.EventID, "EventID/i");
        tOut->Branch("SyncTime_TTT", &header.ExtendedTTT, "SyncTime_TTT/l");
        tOut->Branch("ChannelMask", &header.ChannelMask, "ChannelMask/s");
        tOut->Branch("RecordLength", &record_len_branch, "RecordLength/i"); 

        for (int i = 0; i < 8; ++i) {
            tOut->Branch(Form("Charge_CH%d", i), &charge_ch[i], Form("Charge_CH%d/D", i));
            tOut->Branch(Form("PulseHeight_CH%d", i), &pulse_height_ch[i], Form("PulseHeight_CH%d/D", i));
            tOut->Branch(Form("PulseStart_T0_CH%d", i), &pulse_start_time_ch[i], Form("PulseStart_T0_CH%d/D", i));
            tOut->Branch(Form("Baseline_CH%d", i), &baseline_ch[i], Form("Baseline_CH%d/D", i)); 
            if (save_waveform) tOut->Branch(Form("Waveform_CH%d", i), &wave_ch[i]);
        }
    }

    std::vector<uint16_t> raw_waveform_buffer;
    uint32_t current_event = 0;
    
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
            header.ChannelMask != expected_channel_mask) {
            std::cerr << "\n[Error] Invalid/corrupt raw event header at event "
                      << current_event - 1U << "\n";
            conversion_failed = true;
            g_running = 0;
            break;
        }

        if (is_first_event) {
            first_ttt = header.ExtendedTTT;
            prev_board_counter = header.BoardEventCounter;
            is_first_event = false;
        } else {
            uint32_t diff = (header.BoardEventCounter - prev_board_counter) & 0xFFFFFF;
            if (diff > 1) lost_events += (diff - 1); 
        }
        last_ttt = header.ExtendedTTT;
        prev_board_counter = header.BoardEventCounter;
        total_acquired_samples += header.RecordLength;

        int active_ch = 0;
        for (int i = 0; i < 8; ++i) {
            if ((header.ChannelMask >> i) & 1) active_ch++;
            wave_ch[i].clear();
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

                if (trace_len > 0) {
                    size_t init_window = std::min((size_t)5, trace_len);
                    double init_base = 0.0;
                    for (size_t i = 0; i < init_window; ++i) init_base += trace_ptr[i];
                    init_base /= init_window;

                    size_t baseline_samples = trace_len / 4; 
                    for (size_t i = init_window; i < trace_len; ++i) {
                        if (init_base - trace_ptr[i] > 30.0) { 
                            baseline_samples = (i > 5) ? i - 5 : 1; 
                            break;
                        }
                    }
                    if (baseline_samples > 150) baseline_samples = 150; 
                    
                    double baseline = 0.0;
                    for(size_t i = 0; i < baseline_samples; ++i) {
                        baseline += trace_ptr[i];
                    }
                    baseline /= baseline_samples;
                    baseline_ch[ch] = baseline;

                    // ====================================================================
                    // [핵심 교정] 노이즈 상쇄(Zero-Sum)가 완벽히 적용된 전하량 적분
                    // ====================================================================
                    double charge = 0.0;
                    double min_adc = baseline; 
                    
                    for(size_t i = baseline_samples; i < trace_len; ++i) {
                        // 1. 빗장(if)을 완전히 제거하여 위/아래 노이즈 요동을 모두 더해 0으로 상쇄
                        charge += (baseline - trace_ptr[i]);
                        
                        // 2. 최대 파고(Pulse Height) 탐색은 실제 펄스의 최저점을 찾아야 하므로 유지
                        if (trace_ptr[i] < min_adc) {
                            min_adc = trace_ptr[i];
                        }
                    }
                    
                    // 3. 적분 결과가 순수 노이즈 덩어리(결과값이 0 이하)인 경우에만 0으로 컷오프
                    charge_ch[ch] = (charge > 0) ? charge : 0.0;
                    pulse_height_ch[ch] = (baseline - min_adc > 0) ? (baseline - min_adc) : 0.0; 
                    // ====================================================================

                    double trigger_threshold = baseline - 30.0; 
                    for(size_t i = baseline_samples; i < trace_len; ++i) {
                        if (trace_ptr[i] < trigger_threshold) {
                            pulse_start_time_ch[ch] = i * 2.0; 
                            break;
                        }
                    }
                }

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
                        if (target > (int)current_event) { debug_event_id = target; continue_debug = false; }
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

    if (g_running && debug_event_id < 0) {
        std::cout << "\r\033[K[Progress] 100.0% | Events: " << current_event << " | Done.          \n";

        double real_time_sec = (last_ttt > first_ttt) ? (last_ttt - first_ttt) * 8e-9 : 0.0;
        double dead_time_sec = total_acquired_samples * 2e-9; 
        double live_time_sec = real_time_sec - dead_time_sec;
        if (live_time_sec < 0) live_time_sec = 0.0;
        
        uint64_t total_triggers = current_event + lost_events;
        double lost_events_pct = (total_triggers > 0) ? (static_cast<double>(lost_events) / total_triggers * 100.0) : 0.0;
        double dead_time_pct = (real_time_sec > 0) ? (dead_time_sec / real_time_sec * 100.0) : 0.0;
        
        double avg_rate = (real_time_sec > 0) ? (current_event / real_time_sec) : 0.0;

        std::cout << "\n\033[1;36m========== [ ROOT Conversion Summary ] ==========\033[0m\n"
                  << " - Recorded Events : " << current_event << "\n"
                  << " - Lost Events     : " << lost_events << " (" << std::fixed << std::setprecision(3) << lost_events_pct << " %, Board Buffer Full)\n"
                  << " - HW Real Time    : " << std::fixed << std::setprecision(2) << real_time_sec << " sec\n"
                  << " - HW Live Time    : " << std::fixed << std::setprecision(2) << live_time_sec << " sec\n"
                  << " - True Dead Time  : " << std::fixed << std::setprecision(5) << dead_time_pct << " % (Record Window)\n"
                  << " - Avg Trig Rate   : " << std::fixed << std::setprecision(2) << avg_rate << " Hz\n"
                  << "\033[1;36m=================================================\033[0m\n\n";

        if (fOut) {
            fOut->cd();
            TParameter<double> p_real("RealTime_sec", real_time_sec);
            TParameter<double> p_live("LiveTime_sec", live_time_sec);
            TParameter<double> p_dead("DeadTime_pct", dead_time_pct);
            TParameter<int> p_lost("LostEvents_count", lost_events);
            TParameter<int> p_rec("RecordedEvents_count", current_event);
            TParameter<double> p_rate("TriggerRate_Hz", avg_rate); 
            
            if (p_real.Write() <= 0 || p_live.Write() <= 0 ||
                p_dead.Write() <= 0 || p_lost.Write() <= 0 ||
                p_rec.Write() <= 0 || p_rate.Write() <= 0 ||
                fOut->TestBit(TFile::kWriteError)) {
                std::cerr << "[Error] ROOT summary write failed\n";
                conversion_failed = true;
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
                PublishTemporaryRootOutput(temporary_root_output, output_file);
                std::cout << "\033[1;32m[Production] Conversion complete. Saved to \033[0m"
                          << output_file << "\n";
            } catch (const std::exception& error) {
                std::cerr << "[Error] " << error.what() << "\n";
                conversion_failed = true;
                RemoveTemporaryRootOutput(temporary_root_output);
            }
        } else {
            RemoveTemporaryRootOutput(temporary_root_output);
        }
    }

    if (app) delete app;
    return (!conversion_failed && g_running) ? 0 : 1;
}
