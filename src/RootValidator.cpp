#include "RootValidator.h"

#include "DAQConfig.h"
#include "EventHeader.h"
#include "Sha256.h"

#include <TBranch.h>
#include <TClass.h>
#include <TDataType.h>
#include <TError.h>
#include <TFile.h>
#include <TLeaf.h>
#include <TList.h>
#include <TMacro.h>
#include <TObjString.h>
#include <TObject.h>
#include <TParameter.h>
#include <TTree.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <typeinfo>
#include <utility>
#include <vector>
#include <unistd.h>

#ifndef CPNR_GIT_COMMIT
#define CPNR_GIT_COMMIT "unknown"
#endif

#ifndef CPNR_BUILD_TIMESTAMP
#define CPNR_BUILD_TIMESTAMP "unknown"
#endif

namespace cpnr {
namespace {

using Json = nlohmann::json;
namespace fs = std::filesystem;

constexpr int kChannelCount = 8;
constexpr double kAdcMaximum = 16383.0;
constexpr std::uint64_t kQuantileSampleLimit = 500000U;

struct FileIdentity {
  std::uint64_t device = 0U;
  std::uint64_t inode = 0U;
  std::uint64_t mode = 0U;
  std::uint64_t size = 0U;
  std::int64_t mtime_seconds = 0;
  std::int64_t mtime_nanoseconds = 0;
  std::int64_t ctime_seconds = 0;
  std::int64_t ctime_nanoseconds = 0;
};

class ScopedFileDescriptor {
 public:
  explicit ScopedFileDescriptor(int descriptor) : descriptor_(descriptor) {}
  ScopedFileDescriptor(const ScopedFileDescriptor&) = delete;
  ScopedFileDescriptor& operator=(const ScopedFileDescriptor&) = delete;
  ~ScopedFileDescriptor() {
    if (descriptor_ >= 0) ::close(descriptor_);
  }
  int get() const { return descriptor_; }

 private:
  int descriptor_ = -1;
};

FileIdentity IdentityFromStat(const struct stat& status,
                              const std::string& description) {
  if (!S_ISREG(status.st_mode) || status.st_size < 0) {
    throw std::runtime_error(description + " is not a regular file");
  }
  return {static_cast<std::uint64_t>(status.st_dev),
          static_cast<std::uint64_t>(status.st_ino),
          static_cast<std::uint64_t>(status.st_mode),
          static_cast<std::uint64_t>(status.st_size),
          static_cast<std::int64_t>(status.st_mtim.tv_sec),
          static_cast<std::int64_t>(status.st_mtim.tv_nsec),
          static_cast<std::int64_t>(status.st_ctim.tv_sec),
          static_cast<std::int64_t>(status.st_ctim.tv_nsec)};
}

FileIdentity StatRegularPath(const std::string& path,
                             const std::string& description) {
  struct stat status {};
  if (::stat(path.c_str(), &status) != 0) {
    throw std::runtime_error("Cannot stat " + description + " '" + path +
                             "': " + std::strerror(errno));
  }
  return IdentityFromStat(status, description + " '" + path + "'");
}

FileIdentity StatRegularFile(const std::string& path) {
  return StatRegularPath(path, "input ROOT file");
}

FileIdentity DescriptorIdentity(
    int descriptor,
    const std::string& description = "pinned input ROOT descriptor") {
  struct stat status {};
  if (::fstat(descriptor, &status) != 0) {
    throw std::runtime_error("Cannot identify " + description + ": " +
                             std::string(std::strerror(errno)));
  }
  return IdentityFromStat(status, description);
}

bool SameIdentity(const FileIdentity& left, const FileIdentity& right) {
  return left.device == right.device && left.inode == right.inode &&
         left.mode == right.mode && left.size == right.size &&
         left.mtime_seconds == right.mtime_seconds &&
         left.mtime_nanoseconds == right.mtime_nanoseconds &&
         left.ctime_seconds == right.ctime_seconds &&
         left.ctime_nanoseconds == right.ctime_nanoseconds;
}

Json IdentityJson(const FileIdentity& identity) {
  return {{"device", identity.device},
          {"inode", identity.inode},
          {"mode", identity.mode},
          {"size_bytes", identity.size},
          {"mtime_seconds", identity.mtime_seconds},
          {"mtime_nanoseconds", identity.mtime_nanoseconds},
          {"ctime_seconds", identity.ctime_seconds},
          {"ctime_nanoseconds", identity.ctime_nanoseconds}};
}

std::string AbsolutePath(const std::string& path) {
  std::error_code error;
  fs::path absolute = fs::absolute(fs::path(path), error);
  if (error) return path;
  fs::path canonical = fs::weakly_canonical(absolute, error);
  return error ? absolute.string() : canonical.string();
}

std::string ValidatorExecutablePath() {
  std::error_code error;
  const fs::path executable = fs::read_symlink("/proc/self/exe", error);
  if (error || executable.empty()) {
    throw std::runtime_error("Cannot resolve validator executable identity: " +
                             error.message());
  }
  return AbsolutePath(executable.string());
}

void Progress(const RootValidationOptions& options, double percent,
              std::string_view stage) {
  if (options.progress) options.progress(percent, stage);
}

bool IsCancelled(const RootValidationOptions& options) {
  return options.cancelled != nullptr && options.cancelled->load();
}

class ValidationCancelled final : public std::runtime_error {
 public:
  explicit ValidationCancelled(const std::string& stage)
      : std::runtime_error("Validation cancelled while " + stage) {}
};

std::optional<std::string> Sha256DescriptorCancellable(
    int descriptor, std::uint64_t size_bytes,
    const RootValidationOptions& options) {
  if (descriptor < 0) {
    throw std::runtime_error("Invalid file descriptor for SHA-256");
  }
  Sha256Accumulator accumulator;
  std::array<std::uint8_t, 1024U * 1024U> buffer{};
  std::uint64_t offset = 0U;
  while (offset < size_bytes) {
    if (IsCancelled(options)) return std::nullopt;
    const std::size_t requested = static_cast<std::size_t>(
        std::min<std::uint64_t>(buffer.size(), size_bytes - offset));
    const ssize_t count = ::pread(descriptor, buffer.data(), requested,
                                  static_cast<off_t>(offset));
    if (count < 0 && errno == EINTR) continue;
    if (count < 0) {
      throw std::runtime_error(
          "Cannot read file descriptor for SHA-256 (" +
          std::string(std::strerror(errno)) + ")");
    }
    if (count == 0) {
      throw std::runtime_error(
          "File became shorter while computing SHA-256");
    }
    accumulator.Update(buffer.data(), static_cast<std::size_t>(count));
    offset += static_cast<std::uint64_t>(count);
  }
  if (IsCancelled(options)) return std::nullopt;
  return accumulator.FinalHex();
}

struct CheckCollector {
  explicit CheckCollector(Json* checks) : checks_(checks) {}

  void Add(std::string status, std::string category, std::string name,
           Json observed, Json expected, std::string detail) {
    if (status == "PASS") ++passes;
    if (status == "FAIL") ++failures;
    if (status == "WARN") ++warnings;
    if (status == "SKIP") ++skips;
    checks_->push_back({{"status", std::move(status)},
                        {"category", std::move(category)},
                        {"name", std::move(name)},
                        {"observed", std::move(observed)},
                        {"expected", std::move(expected)},
                        {"detail", std::move(detail)}});
  }

  std::uint64_t passes = 0U;
  std::uint64_t failures = 0U;
  std::uint64_t warnings = 0U;
  std::uint64_t skips = 0U;

 private:
  Json* checks_;
};

double Quantile(std::vector<double> values, double probability) {
  if (values.empty()) return std::numeric_limits<double>::quiet_NaN();
  std::sort(values.begin(), values.end());
  const double position =
      probability * static_cast<double>(values.size() - 1U);
  const std::size_t lower = static_cast<std::size_t>(std::floor(position));
  const std::size_t upper = static_cast<std::size_t>(std::ceil(position));
  const double fraction = position - static_cast<double>(lower);
  return values[lower] * (1.0 - fraction) + values[upper] * fraction;
}

double Median(const std::vector<double>& values) {
  return Quantile(values, 0.5);
}

double Mad(const std::vector<double>& values, double center) {
  std::vector<double> deviations;
  deviations.reserve(values.size());
  for (const double value : values) {
    deviations.push_back(std::abs(value - center));
  }
  return Median(deviations);
}

const Json& RequireField(const Json& object, const std::string& key,
                         const std::string& path) {
  if (!object.is_object() || !object.contains(key)) {
    throw std::runtime_error("missing " + path + "." + key);
  }
  return object.at(key);
}

std::string RequireString(const Json& object, const std::string& key,
                          const std::string& path) {
  const Json& field = RequireField(object, key, path);
  if (!field.is_string()) {
    throw std::runtime_error(path + "." + key + " must be a string");
  }
  return field.get<std::string>();
}

bool RequireBool(const Json& object, const std::string& key,
                 const std::string& path) {
  const Json& field = RequireField(object, key, path);
  if (!field.is_boolean()) {
    throw std::runtime_error(path + "." + key + " must be boolean");
  }
  return field.get<bool>();
}

std::uint64_t RequireUnsigned(const Json& object, const std::string& key,
                              const std::string& path,
                              std::uint64_t minimum,
                              std::uint64_t maximum) {
  const Json& field = RequireField(object, key, path);
  std::uint64_t value = 0U;
  if (field.is_number_unsigned()) {
    value = field.get<std::uint64_t>();
  } else if (field.is_number_integer()) {
    const std::int64_t signed_value = field.get<std::int64_t>();
    if (signed_value < 0) {
      throw std::runtime_error(path + "." + key + " must be non-negative");
    }
    value = static_cast<std::uint64_t>(signed_value);
  } else {
    throw std::runtime_error(path + "." + key + " must be an integer");
  }
  if (value < minimum || value > maximum) {
    throw std::runtime_error(path + "." + key + " is out of range");
  }
  return value;
}

double RequireNumber(const Json& object, const std::string& key,
                     const std::string& path, double minimum,
                     double maximum) {
  const Json& field = RequireField(object, key, path);
  if (!field.is_number()) {
    throw std::runtime_error(path + "." + key + " must be numeric");
  }
  const double value = field.get<double>();
  if (!std::isfinite(value) || value < minimum || value > maximum) {
    throw std::runtime_error(path + "." + key + " is out of range");
  }
  return value;
}

Json ParseStrictJson(const std::string& contents) {
  std::vector<std::set<std::string>> object_keys;
  std::string duplicate_key;
  const Json::parser_callback_t callback =
      [&](int, Json::parse_event_t event, Json& parsed) {
        if (event == Json::parse_event_t::object_start) {
          object_keys.emplace_back();
        } else if (event == Json::parse_event_t::key) {
          if (object_keys.empty()) {
            throw std::runtime_error("malformed JSON object structure");
          }
          const std::string key = parsed.get<std::string>();
          if (!object_keys.back().insert(key).second &&
              duplicate_key.empty()) {
            duplicate_key = key;
          }
        } else if (event == Json::parse_event_t::object_end) {
          if (object_keys.empty()) {
            throw std::runtime_error("malformed JSON object structure");
          }
          object_keys.pop_back();
        }
        return true;
      };
  Json parsed = Json::parse(contents, callback, true, false);
  if (!duplicate_key.empty()) {
    throw std::runtime_error("duplicate JSON key: " + duplicate_key);
  }
  if (!object_keys.empty() || !parsed.is_object()) {
    throw std::runtime_error("metadata root must be one JSON object");
  }
  return parsed;
}

std::optional<std::string> ReadStringObject(TFile& file,
                                            const char* name) {
  auto* object = dynamic_cast<TObjString*>(file.Get(name));
  if (object == nullptr) return std::nullopt;
  return std::string(object->GetString().Data());
}

std::optional<std::vector<std::string>> ReadMacroLines(TFile& file,
                                                       const char* name) {
  auto* macro = dynamic_cast<TMacro*>(file.Get(name));
  if (macro == nullptr || macro->GetListOfLines() == nullptr) {
    return std::nullopt;
  }
  std::vector<std::string> lines;
  TIter next(macro->GetListOfLines());
  while (TObject* object = next()) {
    auto* line = dynamic_cast<TObjString*>(object);
    if (line == nullptr) return std::nullopt;
    lines.emplace_back(line->GetString().Data());
  }
  return lines;
}

std::vector<std::string> TextLines(const std::string& contents) {
  std::vector<std::string> lines;
  std::istringstream input(contents);
  std::string line;
  while (std::getline(input, line)) lines.push_back(line);
  return lines;
}

bool IsLowercaseSha256(const std::string& digest) {
  return digest.size() == 64U &&
         std::all_of(digest.begin(), digest.end(), [](char character) {
           return (character >= '0' && character <= '9') ||
                  (character >= 'a' && character <= 'f');
         });
}

template <typename T>
std::optional<T> ReadParameter(TFile& file, const char* name) {
  auto* parameter = dynamic_cast<TParameter<T>*>(file.Get(name));
  if (parameter == nullptr) return std::nullopt;
  return parameter->GetVal();
}

bool IsExpectedObject(TFile& file, const char* name, const char* class_name) {
  TObject* object = file.Get(name);
  return object != nullptr && object->InheritsFrom(class_name);
}

struct ChannelMetadata {
  bool present = false;
  bool trigger_enabled = false;
  std::uint32_t requested_dc_offset = 0U;
  std::string mode;
  double measured_baseline = 0.0;
  bool baseline_available = false;
  double requested_mv = 0.0;
  bool requested_available = false;
  std::uint32_t delta_adc = 0U;
  bool delta_available = false;
  std::uint32_t written_adc = 0U;
  bool written_available = false;
  std::uint32_t readback_adc = 0U;
  bool readback_available = false;
  double effective_mv = 0.0;
  bool effective_available = false;
};

struct HardwareMetadata {
  bool valid = false;
  std::uint32_t input_range_mvpp = 0U;
  std::uint32_t adc_bits = 0U;
  std::uint32_t clock_source = 0U;
  std::uint32_t run_sync_mode = 0U;
  std::string polarity;
  std::uint32_t record_mask = 0U;
  std::uint32_t record_length = 0U;
  std::uint32_t post_trigger = 0U;
  std::uint32_t external_mode = 0U;
  std::uint32_t self_mode = 0U;
  std::uint32_t self_mask = 0U;
  std::string pair_logic;
  bool explicit_routing = false;
  std::uint32_t global_readback = 0U;
  std::array<std::uint32_t, 4> pair_readback{};
};

struct MetadataState {
  bool present = false;
  bool parsed = false;
  Json document;
  HardwareMetadata hardware;
  std::array<ChannelMetadata, kChannelCount> channels{};
  int run_number = 0;
};

void ValidateConfigAgainstMetadata(const std::string& config_contents,
                                   const MetadataState& metadata,
                                   CheckCollector* checks) {
  if (!metadata.hardware.valid) return;
  const HardwareMetadata& hardware = metadata.hardware;
  DAQHardwareSettings settings;
  try {
    settings = LoadDAQHardwareSettings(ConfigParser::FromText(
        config_contents, "embedded RunConfigExact"));
  } catch (const std::exception& error) {
    checks->Add("FAIL", "provenance", "config_metadata_consistency",
                error.what(),
                "config accepted by the production DAQConfig parser",
                "Embedded runtime config is malformed or violates the exact "
                "DAQ hardware-setting constraints.");
    return;
  }

  Json mismatches = Json::array();
  const auto compare = [&](const std::string& key, Json configured,
                           Json recorded) {
    if (configured != recorded) {
      mismatches.push_back({{"key", key},
                            {"config", std::move(configured)},
                            {"metadata", std::move(recorded)}});
    }
  };
  compare("Digitizer.RecordLength", settings.record_length,
          hardware.record_length);
  compare("Digitizer.ChannelMask", settings.channel_mask,
          hardware.record_mask);
  compare("Digitizer.PostTrigger", settings.post_trigger,
          hardware.post_trigger);
  compare("Digitizer.InputRangeMv", settings.input_range_mv,
          hardware.input_range_mvpp);
  compare("Digitizer.ADCBits", settings.adc_bits, hardware.adc_bits);
  compare("Digitizer.TriggerPolarity",
          settings.trigger_polarity == 1 ? "falling" : "rising",
          hardware.polarity);
  compare("Digitizer.ExtTriggerMode", settings.ext_trigger_mode,
          hardware.external_mode);
  compare("Digitizer.SelfTriggerMode", settings.self_trigger_mode,
          hardware.self_mode);
  compare("Digitizer.SelfTriggerMask", settings.self_trigger_mask,
          hardware.self_mask);
  compare("HardwareCoincidence.PairLogic",
          settings.pair_logic == DAQPairLogic::kAnd ? "AND" : "OR",
          hardware.pair_logic);
  compare("HardwareCoincidence.explicit", settings.explicit_trigger_routing,
          hardware.explicit_routing);
  compare("Synchronization.ClockSource", settings.clock_source,
          hardware.clock_source);
  compare("Synchronization.RunSyncMode", settings.run_sync_mode,
          hardware.run_sync_mode);
  for (int channel = 0; channel < kChannelCount; ++channel) {
    const ChannelMetadata& channel_metadata = metadata.channels[channel];
    if (!channel_metadata.present) continue;
    const std::string prefix = "Channel_" + std::to_string(channel) + ".";
    const DAQChannelSettings& configured = settings.channels[channel];
    compare(prefix + "DCOffset", configured.dc_offset,
            channel_metadata.requested_dc_offset);
    if (channel_metadata.mode == "baseline_relative_mv") {
      if (!configured.has_trigger_threshold ||
          !configured.threshold_is_relative_mv ||
          std::abs(configured.trigger_threshold_mv -
                   channel_metadata.requested_mv) > 1e-9) {
        mismatches.push_back(
            {{"key", prefix + "TriggerThresholdMv"},
             {"config",
              configured.has_trigger_threshold &&
                      configured.threshold_is_relative_mv
                  ? Json(configured.trigger_threshold_mv)
                  : Json(nullptr)},
             {"metadata", channel_metadata.requested_mv}});
      }
    } else if (channel_metadata.mode == "legacy_absolute_adc") {
      if (!configured.has_trigger_threshold ||
          configured.threshold_is_relative_mv ||
          configured.trigger_threshold != channel_metadata.written_adc) {
        mismatches.push_back(
            {{"key", prefix + "TriggerThreshold"},
             {"config",
              configured.has_trigger_threshold &&
                      !configured.threshold_is_relative_mv
                  ? Json(configured.trigger_threshold)
                  : Json(nullptr)},
             {"metadata", channel_metadata.written_adc}});
      }
    }
  }
  checks->Add(mismatches.empty() ? "PASS" : "FAIL", "provenance",
              "config_metadata_consistency", mismatches, Json::array(),
              mismatches.empty()
                  ? "The production DAQConfig parser accepts the embedded "
                    "runtime config and every applied hardware setting agrees "
                    "with metadata."
                  : "Embedded config and hardware metadata disagree.");
}

MetadataState ValidateMetadata(TFile& file, CheckCollector* checks,
                               Json* metadata_report,
                               std::optional<std::string>* config_contents) {
  MetadataState state;
  const std::optional<std::string> metadata_contents =
      ReadStringObject(file, "RunMetadata");
  state.present = metadata_contents.has_value();
  (*metadata_report)["present"] = state.present;
  if (!state.present) {
    checks->Add("FAIL", "provenance", "runtime_metadata", nullptr,
                "RunMetadata and current provenance objects",
                "Legacy ROOT file: runtime metadata is absent, so hardware "
                "settings and source identity cannot be authenticated.");
    return state;
  }

  const std::array<std::pair<const char*, const char*>, 21> objects = {{
      {"RunConfig", "TMacro"},
      {"RunConfigExact", "TObjString"},
      {"RunConfigSha256", "TObjString"},
      {"RunMetadata", "TObjString"},
      {"RunMetadataSha256", "TObjString"},
      {"InputFile", "TObjString"},
      {"ConfigFile", "TObjString"},
      {"MetadataFile", "TObjString"},
      {"RecordedRawOutputPath", "TObjString"},
      {"ResolvedRawInputPath", "TObjString"},
      {"RecordedConfigPath", "TObjString"},
      {"ResolvedConfigPath", "TObjString"},
      {"RecordedMetadataPath", "TObjString"},
      {"ResolvedMetadataPath", "TObjString"},
      {"ExecutablePath", "TObjString"},
      {"ExecutableSha256", "TObjString"},
      {"ExecutableBuildTime", "TObjString"},
      {"ExecutableGitCommit", "TObjString"},
      {"CommandLine", "TObjString"},
      {"RunNumberSource", "TObjString"},
      {"RunNumber", "TParameter<int>"}}};
  Json missing_or_wrong = Json::array();
  for (const auto& [name, type] : objects) {
    if (!IsExpectedObject(file, name, type)) {
      TObject* object = file.Get(name);
      missing_or_wrong.push_back(
          {{"name", name},
           {"observed", object == nullptr ? Json(nullptr)
                                           : Json(object->ClassName())},
           {"expected", type}});
    }
  }
  checks->Add(missing_or_wrong.empty() ? "PASS" : "FAIL", "provenance",
              "root_provenance_objects", missing_or_wrong, Json::array(),
              missing_or_wrong.empty()
                  ? "All current-format provenance objects have expected types."
                  : "Current-format provenance objects are missing or have "
                    "unexpected ROOT types.");

  const std::optional<std::string> metadata_digest =
      ReadStringObject(file, "RunMetadataSha256");
  const std::string computed_metadata_digest = Sha256Hex(*metadata_contents);
  const bool metadata_hash_matches =
      metadata_digest && *metadata_digest == computed_metadata_digest;
  checks->Add(metadata_hash_matches ? "PASS" : "FAIL", "provenance",
              "metadata_sha256", computed_metadata_digest,
              metadata_digest ? Json(*metadata_digest) : Json(nullptr),
              metadata_hash_matches
                  ? "Embedded RunMetadata bytes match RunMetadataSha256."
                  : "Embedded RunMetadata hash mismatch or digest missing.");
  (*metadata_report)["sha256_valid"] = metadata_hash_matches;

  *config_contents = ReadStringObject(file, "RunConfigExact");
  const std::optional<std::string> config_digest =
      ReadStringObject(file, "RunConfigSha256");
  const std::optional<std::string> computed_config_digest =
      *config_contents
          ? std::optional<std::string>(Sha256Hex(**config_contents))
          : std::nullopt;
  const bool config_hash_matches =
      computed_config_digest && config_digest &&
      *computed_config_digest == *config_digest;
  checks->Add(config_hash_matches ? "PASS" : "FAIL", "provenance",
              "config_sha256",
              computed_config_digest ? Json(*computed_config_digest)
                                     : Json(nullptr),
              config_digest ? Json(*config_digest) : Json(nullptr),
              config_hash_matches
                  ? "Embedded RunConfigExact bytes match RunConfigSha256."
                  : "Embedded runtime config hash mismatch or object missing.");
  (*metadata_report)["config_sha256_valid"] = config_hash_matches;

  const std::optional<std::vector<std::string>> macro_lines =
      ReadMacroLines(file, "RunConfig");
  const bool macro_matches =
      macro_lines && *config_contents &&
      *macro_lines == TextLines(**config_contents);
  checks->Add(macro_matches ? "PASS" : "FAIL", "provenance",
              "config_macro_consistency",
              macro_lines ? Json(*macro_lines) : Json(nullptr),
              *config_contents ? Json(TextLines(**config_contents))
                               : Json(nullptr),
              macro_matches
                  ? "RunConfig TMacro lines match RunConfigExact bytes."
                  : "RunConfig and RunConfigExact contain different settings.");

  Json production_identity_errors = Json::array();
  const auto require_nonempty_object = [&](const char* name) {
    const std::optional<std::string> value = ReadStringObject(file, name);
    if (!value || value->empty()) {
      production_identity_errors.push_back(name);
    }
    return value;
  };
  const std::optional<std::string> production_path =
      require_nonempty_object("ExecutablePath");
  const std::optional<std::string> production_sha =
      require_nonempty_object("ExecutableSha256");
  require_nonempty_object("ExecutableBuildTime");
  require_nonempty_object("ExecutableGitCommit");
  require_nonempty_object("CommandLine");
  const std::optional<std::string> run_number_source =
      require_nonempty_object("RunNumberSource");
  if (production_path && !fs::path(*production_path).is_absolute()) {
    production_identity_errors.push_back("ExecutablePath:not_absolute");
  }
  if (production_sha && !IsLowercaseSha256(*production_sha)) {
    production_identity_errors.push_back("ExecutableSha256:not_sha256");
  }
  if (run_number_source && *run_number_source != "-r" &&
      *run_number_source != "RunMetadata" &&
      *run_number_source != "input filename") {
    production_identity_errors.push_back("RunNumberSource:unknown");
  }
  checks->Add(production_identity_errors.empty() ? "PASS" : "FAIL",
              "provenance", "production_identity_fields",
              production_identity_errors, Json::array(),
              production_identity_errors.empty()
                  ? "Production executable/build/command identity fields are well formed."
                  : "Production identity fields are missing or malformed.");

  try {
    state.document = ParseStrictJson(*metadata_contents);
    state.parsed = true;
    const Json& document = state.document;
    const std::uint64_t schema =
        RequireUnsigned(document, "schema_version", "metadata", 1U, 1U);
    state.run_number = static_cast<int>(RequireUnsigned(
        document, "run_number", "metadata", 1U,
        static_cast<std::uint64_t>(std::numeric_limits<int>::max())));
    const std::string acquisition_status =
        RequireString(document, "acquisition_status", "metadata");
    if (acquisition_status != "completed") {
      throw std::runtime_error(
          "metadata.acquisition_status must be completed");
    }
    const Json failure_reason =
        RequireField(document, "failure_reason", "metadata");
    if (!failure_reason.is_null()) {
      throw std::runtime_error(
          "metadata.failure_reason must be null for a completed run");
    }
    RequireUnsigned(document, "created_unix_time", "metadata", 0U,
                    std::numeric_limits<std::uint64_t>::max());
    for (const char* key : {"raw_output_path", "metadata_path", "config_path",
                            "source_config_path", "binary_path",
                            "git_commit", "build_timestamp"}) {
      RequireString(document, key, "metadata");
    }
    const std::uint64_t raw_size = RequireUnsigned(
        document, "raw_output_size_bytes", "metadata", 0U,
        std::numeric_limits<std::uint64_t>::max());
    (void)raw_size;
    for (const char* key : {"raw_output_sha256", "config_sha256",
                            "binary_sha256"}) {
      const std::string digest = RequireString(document, key, "metadata");
      if (!IsLowercaseSha256(digest)) {
        throw std::runtime_error(std::string("metadata.") + key +
                                 " is not lowercase SHA-256");
      }
    }
    if (config_digest &&
        RequireString(document, "config_sha256", "metadata") !=
            *config_digest) {
      throw std::runtime_error(
          "metadata.config_sha256 differs from RunConfigSha256");
    }

    const Json hardware = RequireField(document, "hardware", "metadata");
    for (const char* key : {"model", "roc_firmware", "amc_firmware"}) {
      if (RequireString(hardware, key, "metadata.hardware").empty()) {
        throw std::runtime_error(std::string("metadata.hardware.") + key +
                                 " is empty");
      }
    }
    RequireUnsigned(hardware, "serial_number", "metadata.hardware", 0U,
                    std::numeric_limits<std::uint32_t>::max());
    HardwareMetadata parsed_hardware;
    parsed_hardware.input_range_mvpp = static_cast<std::uint32_t>(
        RequireUnsigned(hardware, "input_range_mvpp", "metadata.hardware",
                        500U, 2000U));
    if (parsed_hardware.input_range_mvpp != 500U &&
        parsed_hardware.input_range_mvpp != 2000U) {
      throw std::runtime_error(
          "metadata.hardware.input_range_mvpp must be exactly 500 or 2000");
    }
    parsed_hardware.adc_bits = static_cast<std::uint32_t>(RequireUnsigned(
        hardware, "adc_bits", "metadata.hardware", 14U, 14U));
    RequireUnsigned(hardware, "dc_offset_dac_bits", "metadata.hardware", 16U,
                    16U);
    const std::uint64_t clock_source = RequireUnsigned(
        hardware, "clock_source", "metadata.hardware", 0U, 1U);
    const std::uint64_t clock_readback = RequireUnsigned(
        hardware, "clock_source_readback", "metadata.hardware", 0U, 1U);
    const std::uint64_t sync_mode = RequireUnsigned(
        hardware, "run_sync_mode", "metadata.hardware", 0U, 4U);
    const std::uint64_t sync_readback = RequireUnsigned(
        hardware, "run_sync_mode_readback", "metadata.hardware", 0U, 4U);
    if (clock_source != clock_readback || sync_mode != sync_readback) {
      throw std::runtime_error(
          "clock/run-sync configuration differs from readback");
    }
    parsed_hardware.clock_source =
        static_cast<std::uint32_t>(clock_source);
    parsed_hardware.run_sync_mode =
        static_cast<std::uint32_t>(sync_mode);
    parsed_hardware.polarity =
        RequireString(hardware, "trigger_polarity", "metadata.hardware");
    if (parsed_hardware.polarity != "falling" &&
        parsed_hardware.polarity != "rising") {
      throw std::runtime_error("unsupported trigger polarity");
    }
    parsed_hardware.record_mask = static_cast<std::uint32_t>(RequireUnsigned(
        hardware, "record_mask", "metadata.hardware", 1U, 0xFFU));
    const std::uint32_t record_readback =
        static_cast<std::uint32_t>(RequireUnsigned(
            hardware, "record_mask_readback", "metadata.hardware", 1U,
            0xFFU));
    if (record_readback != parsed_hardware.record_mask) {
      throw std::runtime_error("record mask differs from readback");
    }
    parsed_hardware.record_length =
        static_cast<std::uint32_t>(RequireUnsigned(
            hardware, "record_length", "metadata.hardware", 128U, 102400U));
    if (parsed_hardware.record_length % 8U != 0U) {
      throw std::runtime_error("record length is not a multiple of 8");
    }
    parsed_hardware.post_trigger = static_cast<std::uint32_t>(RequireUnsigned(
        hardware, "post_trigger_percent", "metadata.hardware", 0U, 100U));
    parsed_hardware.external_mode =
        static_cast<std::uint32_t>(RequireUnsigned(
            hardware, "external_trigger_mode", "metadata.hardware", 0U, 1U));
    parsed_hardware.self_mode = static_cast<std::uint32_t>(RequireUnsigned(
        hardware, "self_trigger_mode", "metadata.hardware", 0U, 1U));
    parsed_hardware.self_mask = static_cast<std::uint32_t>(RequireUnsigned(
        hardware, "self_trigger_mask", "metadata.hardware", 0U, 0xFFU));
    parsed_hardware.pair_logic =
        RequireString(hardware, "pair_logic", "metadata.hardware");
    if (parsed_hardware.pair_logic != "AND" &&
        parsed_hardware.pair_logic != "OR") {
      throw std::runtime_error("pair_logic must be AND or OR");
    }
    parsed_hardware.explicit_routing = RequireBool(
        hardware, "explicit_trigger_routing", "metadata.hardware");
    if (static_cast<std::uint64_t>(parsed_hardware.record_length) *
            (100U - parsed_hardware.post_trigger) <
        8000U) {
      throw std::runtime_error(
          "record length/post-trigger leave less than 160 ns pre-trigger");
    }
    if (parsed_hardware.external_mode == 0U &&
        parsed_hardware.self_mode == 0U) {
      throw std::runtime_error(
          "external and self trigger modes cannot both be disabled");
    }
    if (parsed_hardware.self_mode != 0U &&
        parsed_hardware.pair_logic == "AND") {
      for (int pair = 0; pair < 4; ++pair) {
        const std::uint32_t pair_bits =
            (parsed_hardware.self_mask >> (2 * pair)) & 0x3U;
        if (pair_bits == 1U || pair_bits == 2U) {
          throw std::runtime_error(
              "AND routing requires complete adjacent channel pairs");
        }
      }
    }
    if (!parsed_hardware.explicit_routing &&
        (parsed_hardware.pair_logic != "OR" ||
         parsed_hardware.self_mask !=
             (parsed_hardware.self_mode != 0U
                  ? parsed_hardware.record_mask
                  : 0U))) {
      throw std::runtime_error(
          "legacy routing metadata differs from DAQConfig defaults");
    }
    parsed_hardware.global_readback =
        static_cast<std::uint32_t>(RequireUnsigned(
            hardware, "global_trigger_mask_readback", "metadata.hardware", 0U,
            std::numeric_limits<std::uint32_t>::max()));
    const Json pair_readbacks =
        RequireField(hardware, "pair_logic_readback", "metadata.hardware");
    if (!pair_readbacks.is_array() || pair_readbacks.size() != 4U) {
      throw std::runtime_error(
          "metadata.hardware.pair_logic_readback must have four fields");
    }
    std::uint32_t expected_pair_sources = 0U;
    for (int pair = 0; pair < 4; ++pair) {
      const Json wrapper = {{"value", pair_readbacks.at(pair)}};
      parsed_hardware.pair_readback[static_cast<std::size_t>(pair)] =
          static_cast<std::uint32_t>(RequireUnsigned(
              wrapper, "value", "metadata.hardware.pair_logic_readback", 0U,
              7U));
      const std::uint32_t bits =
          (parsed_hardware.self_mask >> (2 * pair)) & 0x3U;
      if (bits != 0U) expected_pair_sources |= 1U << pair;
      if (parsed_hardware.explicit_routing && bits != 0U) {
        const std::uint32_t expected_field =
            bits == 1U ? 5U
            : bits == 2U
                ? 6U
                : parsed_hardware.pair_logic == "AND" ? 4U : 7U;
        if (parsed_hardware.pair_readback[static_cast<std::size_t>(pair)] !=
            expected_field) {
          throw std::runtime_error(
              "pair trigger logic readback differs from requested routing");
        }
      }
    }
    if ((parsed_hardware.self_mask & ~parsed_hardware.record_mask) != 0U ||
        (parsed_hardware.self_mode == 0U &&
         parsed_hardware.self_mask != 0U) ||
        (parsed_hardware.self_mode != 0U &&
         parsed_hardware.self_mask == 0U)) {
      throw std::runtime_error("self-trigger mask/mode is inconsistent");
    }
    const std::uint32_t expected_global =
        expected_pair_sources |
        (parsed_hardware.external_mode != 0U ? (1U << 30U) : 0U) |
        (!parsed_hardware.explicit_routing ? (1U << 31U) : 0U);
    if ((parsed_hardware.global_readback & 0xC000000FU) != expected_global ||
        (parsed_hardware.global_readback & (0x7U << 24U)) != 0U) {
      throw std::runtime_error(
          "global trigger source/veto readback is inconsistent");
    }

    const Json channels = RequireField(document, "channels", "metadata");
    if (!channels.is_array()) {
      throw std::runtime_error("metadata.channels must be an array");
    }
    std::set<int> seen_channels;
    for (const Json& channel : channels) {
      const int number = static_cast<int>(RequireUnsigned(
          channel, "channel", "metadata.channels[]", 0U,
          static_cast<std::uint64_t>(kChannelCount - 1)));
      if (!seen_channels.insert(number).second ||
          ((parsed_hardware.record_mask >> number) & 1U) == 0U) {
        throw std::runtime_error("duplicate or disabled metadata channel");
      }
      ChannelMetadata parsed_channel;
      parsed_channel.present = true;
      parsed_channel.trigger_enabled = RequireBool(
          channel, "trigger_enabled", "metadata.channels[]");
      if (parsed_channel.trigger_enabled !=
          (((parsed_hardware.self_mask >> number) & 1U) != 0U)) {
        throw std::runtime_error(
            "channel trigger_enabled differs from self-trigger mask");
      }
      const std::uint64_t range_register = RequireUnsigned(
          channel, "input_range_register", "metadata.channels[]", 0U,
          std::numeric_limits<std::uint32_t>::max());
      const std::uint64_t range_readback = RequireUnsigned(
          channel, "input_range_readback", "metadata.channels[]", 0U, 1U);
      if (range_register !=
              0x1028U + 0x100U * static_cast<std::uint32_t>(number) ||
          range_readback !=
              (parsed_hardware.input_range_mvpp == 500U ? 1U : 0U)) {
        throw std::runtime_error("channel input-range readback is inconsistent");
      }
      const std::uint64_t requested_offset = RequireUnsigned(
          channel, "requested_dc_offset", "metadata.channels[]", 0U, 65535U);
      const std::uint64_t readback_offset = RequireUnsigned(
          channel, "readback_dc_offset", "metadata.channels[]", 0U, 65535U);
      if (requested_offset != readback_offset ||
          RequireString(channel, "polarity_readback", "metadata.channels[]") !=
              parsed_hardware.polarity) {
        throw std::runtime_error("channel analog readback is inconsistent");
      }
      parsed_channel.requested_dc_offset =
          static_cast<std::uint32_t>(requested_offset);
      parsed_channel.mode = RequireString(
          channel, "threshold_mode", "metadata.channels[]");
      const Json baseline = RequireField(
          channel, "measured_baseline_adc", "metadata.channels[]");
      if (!baseline.is_null()) {
        parsed_channel.measured_baseline = RequireNumber(
            channel, "measured_baseline_adc", "metadata.channels[]", 0.0,
            kAdcMaximum);
        parsed_channel.baseline_available = true;
      }
      const Json requested = RequireField(
          channel, "requested_threshold_mv", "metadata.channels[]");
      const Json delta =
          RequireField(channel, "delta_adc", "metadata.channels[]");
      const Json written = RequireField(
          channel, "written_threshold_adc", "metadata.channels[]");
      const Json readback = RequireField(
          channel, "readback_threshold_adc", "metadata.channels[]");
      const Json effective = RequireField(
          channel, "effective_threshold_mv", "metadata.channels[]");

      if (!parsed_channel.trigger_enabled) {
        if (parsed_channel.mode != "not_used_record_only" ||
            !requested.is_null() || !delta.is_null() || !written.is_null() ||
            !readback.is_null() || !effective.is_null()) {
          throw std::runtime_error(
              "record-only channel has discriminator threshold metadata");
        }
      } else {
        parsed_channel.written_adc = static_cast<std::uint32_t>(
            RequireUnsigned(channel, "written_threshold_adc",
                            "metadata.channels[]", 0U, 16383U));
        parsed_channel.readback_adc = static_cast<std::uint32_t>(
            RequireUnsigned(channel, "readback_threshold_adc",
                            "metadata.channels[]", 0U, 16383U));
        parsed_channel.written_available = true;
        parsed_channel.readback_available = true;
        if (parsed_channel.written_adc != parsed_channel.readback_adc) {
          throw std::runtime_error("threshold write/readback mismatch");
        }
        if (parsed_channel.mode == "baseline_relative_mv") {
          if (!parsed_channel.baseline_available) {
            throw std::runtime_error(
                "baseline-relative threshold lacks measured baseline");
          }
          parsed_channel.requested_mv = RequireNumber(
              channel, "requested_threshold_mv", "metadata.channels[]", 0.0,
              static_cast<double>(parsed_hardware.input_range_mvpp));
          parsed_channel.delta_adc = static_cast<std::uint32_t>(
              RequireUnsigned(channel, "delta_adc", "metadata.channels[]", 1U,
                              16383U));
          parsed_channel.effective_mv = RequireNumber(
              channel, "effective_threshold_mv", "metadata.channels[]", 0.0,
              static_cast<double>(parsed_hardware.input_range_mvpp));
          parsed_channel.requested_available = true;
          parsed_channel.delta_available = true;
          parsed_channel.effective_available = true;
          const std::uint32_t expected_delta =
              static_cast<std::uint32_t>(std::llround(
                  parsed_channel.requested_mv *
                  static_cast<double>(1U << parsed_hardware.adc_bits) /
                  static_cast<double>(parsed_hardware.input_range_mvpp)));
          const long long rounded_baseline =
              std::llround(parsed_channel.measured_baseline);
          const long long expected_written =
              parsed_hardware.polarity == "falling"
                  ? rounded_baseline -
                        static_cast<long long>(parsed_channel.delta_adc)
                  : rounded_baseline +
                        static_cast<long long>(parsed_channel.delta_adc);
          const double expected_effective =
              std::abs(parsed_channel.measured_baseline -
                       static_cast<double>(parsed_channel.written_adc)) *
              static_cast<double>(parsed_hardware.input_range_mvpp) /
              static_cast<double>(1U << parsed_hardware.adc_bits);
          if (parsed_channel.delta_adc != expected_delta ||
              expected_written !=
                  static_cast<long long>(parsed_channel.written_adc) ||
              std::abs(parsed_channel.effective_mv - expected_effective) >
                  1e-6) {
            throw std::runtime_error(
                "baseline-relative threshold equation is inconsistent");
          }
        } else if (parsed_channel.mode == "legacy_absolute_adc") {
          if (!requested.is_null() || !delta.is_null() ||
              !effective.is_null()) {
            throw std::runtime_error(
                "legacy absolute threshold has relative-threshold fields");
          }
        } else {
          throw std::runtime_error("unknown channel threshold mode");
        }
      }
      state.channels[static_cast<std::size_t>(number)] = parsed_channel;
    }
    const unsigned int active_channels =
        static_cast<unsigned int>(__builtin_popcount(parsed_hardware.record_mask));
    if (channels.size() != active_channels) {
      throw std::runtime_error(
          "metadata channel count differs from record mask");
    }
    parsed_hardware.valid = true;
    state.hardware = parsed_hardware;
    (*metadata_report)["schema_version"] = schema;
    (*metadata_report)["run_number"] = state.run_number;
    (*metadata_report)["acquisition_status"] = acquisition_status;
    (*metadata_report)["hardware"] = hardware;
    checks->Add("PASS", "provenance", "metadata_schema", "valid",
                "schema_version=1 completed runtime metadata",
                "Embedded runtime metadata, hardware readbacks, thresholds, "
                "and trigger routing are internally consistent.");
    checks->Add(
        "PASS", "trigger", "trigger_metadata_authentication",
        {{"self_trigger_mask", state.hardware.self_mask},
         {"pair_logic", state.hardware.pair_logic},
         {"global_trigger_mask_readback", state.hardware.global_readback},
         {"pair_logic_readback", state.hardware.pair_readback}},
        "internally consistent requested settings and hardware readbacks",
        "Authenticated metadata proves the saved threshold and trigger-routing readbacks are internally consistent.");
  } catch (const std::exception& error) {
    // Never expose partially parsed channel/hardware state to downstream
    // threshold checks after a late schema failure.
    state.parsed = false;
    state.hardware = HardwareMetadata{};
    state.channels = {};
    state.run_number = 0;
    checks->Add("FAIL", "provenance", "metadata_schema", error.what(),
                "valid schema_version=1 completed runtime metadata",
                "Embedded runtime metadata failed strict validation.");
    checks->Add(
        "FAIL", "trigger", "trigger_metadata_authentication", error.what(),
        "authenticated threshold and routing metadata",
        "Threshold and trigger-routing settings cannot be trusted because strict runtime metadata validation failed.");
  }

  if (*config_contents && state.parsed && state.hardware.valid) {
    ValidateConfigAgainstMetadata(**config_contents, state, checks);
  }
  return state;
}

struct ArtifactDigestObservation {
  FileIdentity identity;
  std::string sha256;
};

using ArtifactDigestCache =
    std::map<std::string, ArtifactDigestObservation>;

void ValidateExternalArtifact(
    const std::string& name, const std::string& path,
    std::optional<std::uint64_t> expected_size,
    const std::string& expected_sha256, CheckCollector* checks,
    const RootValidationOptions& options, bool hash_contents,
    ArtifactDigestCache* cache) {
  if (path.empty()) {
    checks->Add("FAIL", "provenance", name, nullptr,
                "non-empty recorded artifact path",
                "The provenance record contains an empty artifact path.");
    return;
  }
  std::error_code error;
  fs::path absolute = fs::absolute(fs::path(path), error);
  if (error) {
    checks->Add("FAIL", "provenance", name, error.message(),
                "resolvable artifact path",
                "The recorded artifact path could not be resolved.");
    return;
  }
  const std::string absolute_path = absolute.lexically_normal().string();
  const int descriptor = ::open(absolute_path.c_str(),
                                O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (descriptor < 0 && (errno == ENOENT || errno == ENOTDIR)) {
    checks->Add("SKIP", "provenance", name,
                {{"path", absolute_path}, {"available", false}},
                "verify when artifact is available",
                "The external artifact is not present at its recorded path; "
                "this does not invalidate the self-contained ROOT report.");
    return;
  }
  if (descriptor < 0) {
    checks->Add("FAIL", "provenance", name,
                {{"path", absolute_path},
                 {"error", std::string(std::strerror(errno))}},
                "readable non-symlink regular artifact",
                "The external artifact could not be opened safely.");
    return;
  }
  const ScopedFileDescriptor pinned_artifact(descriptor);
  try {
    const FileIdentity initial_identity = DescriptorIdentity(
        pinned_artifact.get(), "pinned external artifact descriptor");
    const FileIdentity initial_path_identity =
        StatRegularPath(absolute_path, "external artifact");
    if (!SameIdentity(initial_identity, initial_path_identity)) {
      throw std::runtime_error(
          "artifact path changed while its descriptor was opened");
    }
    const std::uint64_t observed_size = initial_identity.size;
    const bool size_matches =
        !expected_size || observed_size == *expected_size;
    if (!hash_contents) {
      checks->Add(size_matches ? "SKIP" : "FAIL", "provenance", name,
                  {{"path", absolute_path},
                   {"available", true},
                   {"size_bytes", observed_size},
                   {"sha256", nullptr}},
                  {{"size_bytes", expected_size ? Json(*expected_size)
                                                 : Json(nullptr)},
                   {"sha256", expected_sha256}},
                  size_matches
                      ? "Prefix validation checks artifact identity and size but deliberately skips reading all artifact bytes for SHA-256."
                      : "The available artifact size differs from its recorded identity even without a content hash.");
      return;
    }

    std::string observed_sha256;
    const auto cached = cache->find(absolute_path);
    if (cached != cache->end()) {
      if (!SameIdentity(cached->second.identity, initial_identity)) {
        throw std::runtime_error(
            "artifact identity differs from an earlier observation");
      }
      observed_sha256 = cached->second.sha256;
    } else {
      const std::optional<std::string> digest =
          Sha256DescriptorCancellable(pinned_artifact.get(), observed_size,
                                      options);
      if (!digest) {
        throw ValidationCancelled("hashing external provenance artifacts");
      }
      const FileIdentity final_identity = DescriptorIdentity(
          pinned_artifact.get(), "pinned external artifact descriptor");
      const FileIdentity final_path_identity =
          StatRegularPath(absolute_path, "external artifact");
      if (!SameIdentity(initial_identity, final_identity) ||
          !SameIdentity(initial_identity, final_path_identity)) {
        throw std::runtime_error(
            "artifact identity changed while computing SHA-256");
      }
      observed_sha256 = *digest;
      cache->emplace(absolute_path,
                     ArtifactDigestObservation{initial_identity,
                                               observed_sha256});
    }
    const bool hash_matches = observed_sha256 == expected_sha256;
    checks->Add(size_matches && hash_matches ? "PASS" : "FAIL",
                "provenance", name,
                {{"path", absolute_path},
                 {"size_bytes", observed_size},
                 {"sha256", observed_sha256}},
                {{"size_bytes", expected_size ? Json(*expected_size)
                                               : Json(nullptr)},
                 {"sha256", expected_sha256}},
                size_matches && hash_matches
                    ? "Available external artifact matches its recorded identity."
                    : "Available external artifact differs from its recorded identity.");
  } catch (const ValidationCancelled&) {
    throw;
  } catch (const std::exception& error_detail) {
    checks->Add("FAIL", "provenance", name, error_detail.what(),
                "readable artifact matching recorded size/SHA-256",
                "The artifact exists but could not be authenticated.");
  }
}

void ValidateExternalProvenance(TFile& file, const MetadataState& metadata,
                                const std::optional<std::string>& config_contents,
                                CheckCollector* checks,
                                const RootValidationOptions& options,
                                bool hash_contents) {
  if (!metadata.parsed || !metadata.hardware.valid) return;
  ArtifactDigestCache cache;
  try {
    const Json& document = metadata.document;
    const std::string raw_path =
        RequireString(document, "raw_output_path", "metadata");
    const std::uint64_t raw_size = RequireUnsigned(
        document, "raw_output_size_bytes", "metadata", 0U,
        std::numeric_limits<std::uint64_t>::max());
    const std::string raw_sha =
        RequireString(document, "raw_output_sha256", "metadata");
    ValidateExternalArtifact("external_raw_output", raw_path, raw_size,
                             raw_sha, checks, options, hash_contents, &cache);

    const std::string config_path =
        RequireString(document, "config_path", "metadata");
    const std::string config_sha =
        RequireString(document, "config_sha256", "metadata");
    ValidateExternalArtifact(
        "external_runtime_config", config_path,
        config_contents
            ? std::optional<std::uint64_t>(config_contents->size())
            : std::nullopt,
        config_sha, checks, options, hash_contents, &cache);

    const std::string frontend_path =
        RequireString(document, "binary_path", "metadata");
    const std::string frontend_sha =
        RequireString(document, "binary_sha256", "metadata");
    ValidateExternalArtifact("external_frontend_binary", frontend_path,
                             std::nullopt, frontend_sha, checks, options,
                             hash_contents, &cache);

    const std::optional<std::string> production_path =
        ReadStringObject(file, "ExecutablePath");
    const std::optional<std::string> production_sha =
        ReadStringObject(file, "ExecutableSha256");
    if (production_path && production_sha) {
      ValidateExternalArtifact("external_production_binary", *production_path,
                               std::nullopt, *production_sha, checks, options,
                               hash_contents, &cache);
    } else {
      checks->Add("FAIL", "provenance", "external_production_binary",
                  nullptr, "ExecutablePath and ExecutableSha256",
                  "Production executable identity objects are missing.");
    }

    const std::optional<std::string> root_input =
        ReadStringObject(file, "InputFile");
    const std::optional<std::string> root_config =
        ReadStringObject(file, "ConfigFile");
    const std::optional<std::string> root_metadata =
        ReadStringObject(file, "MetadataFile");
    const std::optional<std::string> recorded_raw_object =
        ReadStringObject(file, "RecordedRawOutputPath");
    const std::optional<std::string> resolved_raw_object =
        ReadStringObject(file, "ResolvedRawInputPath");
    const std::optional<std::string> recorded_config_object =
        ReadStringObject(file, "RecordedConfigPath");
    const std::optional<std::string> resolved_config_object =
        ReadStringObject(file, "ResolvedConfigPath");
    const std::optional<std::string> recorded_metadata_object =
        ReadStringObject(file, "RecordedMetadataPath");
    const std::optional<std::string> resolved_metadata_object =
        ReadStringObject(file, "ResolvedMetadataPath");
    const std::string metadata_path =
        RequireString(document, "metadata_path", "metadata");
    const auto is_absolute = [](const std::optional<std::string>& path) {
      return path && fs::path(*path).is_absolute();
    };
    const bool paths_authenticated =
        root_input && root_config && root_metadata && recorded_raw_object &&
        resolved_raw_object && recorded_config_object &&
        resolved_config_object && recorded_metadata_object &&
        resolved_metadata_object &&
        *recorded_raw_object == raw_path &&
        *recorded_config_object == config_path &&
        *recorded_metadata_object == metadata_path &&
        AbsolutePath(*root_input) == AbsolutePath(*resolved_raw_object) &&
        AbsolutePath(*root_config) == AbsolutePath(*resolved_config_object) &&
        AbsolutePath(*root_metadata) ==
            AbsolutePath(*resolved_metadata_object) &&
        is_absolute(recorded_raw_object) && is_absolute(resolved_raw_object) &&
        is_absolute(recorded_config_object) &&
        is_absolute(resolved_config_object) &&
        is_absolute(recorded_metadata_object) &&
        is_absolute(resolved_metadata_object);
    const bool raw_relocated =
        recorded_raw_object && resolved_raw_object &&
        AbsolutePath(*recorded_raw_object) !=
            AbsolutePath(*resolved_raw_object);
    const bool config_relocated =
        recorded_config_object && resolved_config_object &&
        AbsolutePath(*recorded_config_object) !=
            AbsolutePath(*resolved_config_object);
    const bool metadata_relocated =
        recorded_metadata_object && resolved_metadata_object &&
        AbsolutePath(*recorded_metadata_object) !=
            AbsolutePath(*resolved_metadata_object);
    checks->Add(paths_authenticated ? "PASS" : "FAIL", "provenance",
                "embedded_artifact_paths",
                {{"recorded",
                  {{"raw", recorded_raw_object
                               ? Json(*recorded_raw_object) : Json(nullptr)},
                   {"config", recorded_config_object
                                  ? Json(*recorded_config_object)
                                  : Json(nullptr)},
                   {"metadata", recorded_metadata_object
                                    ? Json(*recorded_metadata_object)
                                    : Json(nullptr)}}},
                 {"resolved",
                  {{"raw", resolved_raw_object
                               ? Json(*resolved_raw_object) : Json(nullptr)},
                   {"config", resolved_config_object
                                  ? Json(*resolved_config_object)
                                  : Json(nullptr)},
                   {"metadata", resolved_metadata_object
                                    ? Json(*resolved_metadata_object)
                                    : Json(nullptr)}}},
                 {"relocated",
                  {{"raw", raw_relocated},
                   {"config", config_relocated},
                   {"metadata", metadata_relocated}}}},
                "recorded locators match RunMetadata; resolved locators are absolute and match compatibility aliases",
                paths_authenticated
                    ? "Original RunMetadata locators and authenticated current artifact locations are preserved independently."
                    : "Recorded/resolved artifact provenance objects are missing, relative, or internally inconsistent.");

    const std::optional<std::string> embedded_metadata =
        ReadStringObject(file, "RunMetadata");
    if (embedded_metadata) {
      ValidateExternalArtifact(
          "external_runtime_metadata", metadata_path,
          std::optional<std::uint64_t>(embedded_metadata->size()),
          Sha256Hex(*embedded_metadata), checks, options, hash_contents,
          &cache);
    }
    if (resolved_raw_object) {
      ValidateExternalArtifact("external_resolved_raw_output",
                               *resolved_raw_object, raw_size, raw_sha,
                               checks, options, hash_contents, &cache);
    }
    if (resolved_config_object) {
      ValidateExternalArtifact(
          "external_resolved_runtime_config", *resolved_config_object,
          config_contents
              ? std::optional<std::uint64_t>(config_contents->size())
              : std::nullopt,
          config_sha, checks, options, hash_contents, &cache);
    }
    if (resolved_metadata_object && embedded_metadata) {
      ValidateExternalArtifact(
          "external_resolved_runtime_metadata", *resolved_metadata_object,
          std::optional<std::uint64_t>(embedded_metadata->size()),
          Sha256Hex(*embedded_metadata), checks, options, hash_contents,
          &cache);
    }
  } catch (const ValidationCancelled&) {
    throw;
  } catch (const std::exception& error) {
    checks->Add("FAIL", "provenance", "external_artifact_validation",
                error.what(), "well-formed artifact identity fields",
                "External provenance validation could not be initialized.");
  }
}

struct ChannelAccumulator {
  std::uint64_t active_events = 0U;
  std::uint64_t finite_violations = 0U;
  std::uint64_t range_violations = 0U;
  std::uint64_t inactive_sentinel_violations = 0U;
  std::uint64_t waveform_size_violations = 0U;
  std::uint64_t waveform_range_violations = 0U;
  std::uint64_t waveform_dsp_mismatches = 0U;
  std::uint64_t t0_violations = 0U;
  std::uint64_t t0_found = 0U;
  std::uint64_t saturation_events = 0U;
  std::uint64_t trigger_evaluable = 0U;
  std::uint64_t trigger_crossings = 0U;
  double pulse_min = std::numeric_limits<double>::infinity();
  double pulse_max = -std::numeric_limits<double>::infinity();
  double charge_min = std::numeric_limits<double>::infinity();
  double charge_max = -std::numeric_limits<double>::infinity();
  std::vector<double> baseline_samples;
  std::vector<double> tail_samples;
  std::vector<double> bin_samples;
  std::vector<double> bin_medians;
};

struct LegacyPairExtremum {
  std::uint64_t entry = 0U;
  double low = 0.0;
  double high = 0.0;
};

void FlushBaselineBins(
    std::array<ChannelAccumulator, kChannelCount>* accumulators) {
  for (ChannelAccumulator& accumulator : *accumulators) {
    if (!accumulator.bin_samples.empty()) {
      accumulator.bin_medians.push_back(Median(accumulator.bin_samples));
      accumulator.bin_samples.clear();
    }
  }
}

std::optional<std::uint64_t> SettlingEvent(
    const std::vector<double>& bin_medians, double tail_median,
    std::uint64_t bin_size, double tolerance) {
  constexpr std::size_t kStableBins = 5U;
  if (bin_medians.size() < kStableBins) return std::nullopt;
  for (std::size_t first = 0U;
       first + kStableBins <= bin_medians.size(); ++first) {
    bool stable = true;
    for (std::size_t offset = 0U; offset < kStableBins; ++offset) {
      if (std::abs(bin_medians[first + offset] - tail_median) > tolerance) {
        stable = false;
        break;
      }
    }
    if (stable) return static_cast<std::uint64_t>(first) * bin_size;
  }
  return std::nullopt;
}

bool ApproxEqual(double observed, double expected) {
  return std::abs(observed - expected) <=
         std::max(1e-9, 1e-9 * std::max(std::abs(observed),
                                       std::abs(expected)));
}

struct DerivedWaveformValues {
  double baseline = 0.0;
  double charge = 0.0;
  double pulse_height = 0.0;
  double pulse_start_ns = -1.0;
};

DerivedWaveformValues RecomputeWaveformValues(
    const std::vector<UShort_t>& trace, bool falling_polarity) {
  DerivedWaveformValues values;
  if (trace.empty()) return values;

  const std::size_t initial_window = std::min<std::size_t>(5U, trace.size());
  double initial_baseline = 0.0;
  for (std::size_t sample = 0U; sample < initial_window; ++sample) {
    initial_baseline += trace[sample];
  }
  initial_baseline /= static_cast<double>(initial_window);

  std::size_t baseline_samples = trace.size() / 4U;
  for (std::size_t sample = initial_window; sample < trace.size(); ++sample) {
    const double baseline_excursion =
        falling_polarity
            ? initial_baseline - static_cast<double>(trace[sample])
            : static_cast<double>(trace[sample]) - initial_baseline;
    if (baseline_excursion > 30.0) {
      baseline_samples = sample > 5U ? sample - 5U : 1U;
      break;
    }
  }
  baseline_samples = std::min<std::size_t>(baseline_samples, 150U);
  if (baseline_samples == 0U) baseline_samples = 1U;

  for (std::size_t sample = 0U; sample < baseline_samples; ++sample) {
    values.baseline += trace[sample];
  }
  values.baseline /= static_cast<double>(baseline_samples);

  double pulse_extremum = values.baseline;
  double integrated_charge = 0.0;
  if (falling_polarity) {
    for (std::size_t sample = baseline_samples; sample < trace.size(); ++sample) {
      integrated_charge +=
          values.baseline - static_cast<double>(trace[sample]);
      pulse_extremum =
          std::min(pulse_extremum, static_cast<double>(trace[sample]));
    }
    values.pulse_height =
        std::max(0.0, values.baseline - pulse_extremum);
  } else {
    for (std::size_t sample = baseline_samples; sample < trace.size(); ++sample) {
      integrated_charge +=
          static_cast<double>(trace[sample]) - values.baseline;
      pulse_extremum =
          std::max(pulse_extremum, static_cast<double>(trace[sample]));
    }
    values.pulse_height =
        std::max(0.0, pulse_extremum - values.baseline);
  }
  values.charge = std::max(0.0, integrated_charge);

  const double start_threshold = falling_polarity
                                     ? values.baseline - 30.0
                                     : values.baseline + 30.0;
  for (std::size_t sample = baseline_samples; sample < trace.size(); ++sample) {
    const bool crossed =
        falling_polarity
            ? static_cast<double>(trace[sample]) < start_threshold
            : static_cast<double>(trace[sample]) > start_threshold;
    if (crossed) {
      values.pulse_start_ns = 2.0 * static_cast<double>(sample);
      break;
    }
  }
  return values;
}

Json NullOrDouble(bool available, double value) {
  return available && std::isfinite(value) ? Json(value) : Json(nullptr);
}

Json NullOrUnsigned(bool available, std::uint64_t value) {
  return available ? Json(value) : Json(nullptr);
}

std::string DomainStatus(
    const Json& checks,
    std::initializer_list<std::string_view> categories) {
  int worst = -1;
  for (const Json& check : checks) {
    if (!check.is_object() || !check.contains("category") ||
        !check.at("category").is_string() || !check.contains("status") ||
        !check.at("status").is_string()) {
      continue;
    }
    const std::string category = check.at("category").get<std::string>();
    if (std::find(categories.begin(), categories.end(), category) ==
        categories.end()) {
      continue;
    }
    const std::string status = check.at("status").get<std::string>();
    if (status == "FAIL") worst = std::max(worst, 2);
    else if (status == "WARN") worst = std::max(worst, 1);
    else if (status == "PASS") worst = std::max(worst, 0);
  }
  return worst == 2 ? "FAIL" : worst == 1 ? "WARN" : worst == 0 ? "PASS"
                                                                     : "SKIP";
}

}  // namespace

Json ValidateRootFile(const std::string& input_path,
                      const RootValidationOptions& options) {
  if (input_path.empty()) {
    throw std::invalid_argument("Input ROOT path is empty");
  }
  Progress(options, 0.0, "opening input");
  const std::string absolute_path = AbsolutePath(input_path);
  const int input_descriptor =
      ::open(absolute_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (input_descriptor < 0) {
    throw std::runtime_error("Cannot open input ROOT file read-only '" +
                             absolute_path + "': " + std::strerror(errno));
  }
  const ScopedFileDescriptor pinned_input(input_descriptor);
  const FileIdentity initial_identity = DescriptorIdentity(pinned_input.get());
  if (!SameIdentity(initial_identity, StatRegularFile(absolute_path))) {
    throw std::runtime_error(
        "Input ROOT path changed while its read-only descriptor was opened");
  }
  const std::string pinned_input_path =
      "/proc/self/fd/" + std::to_string(pinned_input.get());
  const bool hash_full_contents = options.max_events == 0U;
  std::optional<std::string> initial_sha256;
  if (hash_full_contents) {
    Progress(options, 2.0, "hashing input");
    initial_sha256 = Sha256DescriptorCancellable(
        pinned_input.get(), initial_identity.size, options);
    if (!initial_sha256) {
      throw ValidationCancelled("hashing the input ROOT file");
    }
    const FileIdentity post_hash_identity =
        DescriptorIdentity(pinned_input.get());
    if (!SameIdentity(initial_identity, post_hash_identity) ||
        !SameIdentity(initial_identity, StatRegularFile(absolute_path))) {
      throw std::runtime_error(
          "Input ROOT identity changed while computing its initial SHA-256");
    }
  } else {
    Progress(options, 2.0, "opening prefix without full-file hash");
  }
  const std::string validator_path = ValidatorExecutablePath();
  const std::string validator_sha256 = Sha256FileHex("/proc/self/exe");

  Json report = {{"schema_version", 1},
                 {"validator", {{"name", "root_validate_dt5730"},
                                {"version", 1},
                                {"executable_path", validator_path},
                                {"executable_sha256", validator_sha256},
                                {"git_commit", CPNR_GIT_COMMIT},
                                {"build_timestamp", CPNR_BUILD_TIMESTAMP}}},
                 {"input", {{"path", absolute_path},
                            {"size_bytes", initial_identity.size},
                            {"sha256", initial_sha256
                                           ? Json(*initial_sha256)
                                           : Json(nullptr)},
                            {"max_events", options.max_events == 0U
                                               ? Json(nullptr)
                                               : Json(options.max_events)},
                            {"validation_mode",
                             hash_full_contents ? "full" : "prefix"},
                            {"identity_start", IdentityJson(initial_identity)}}},
                 {"analysis", Json::object()},
                 {"overall_status", "PASS"},
                 {"legacy", false},
                 {"checks", Json::array()},
                 {"summary", Json::object()},
                 {"channels", Json::array()},
                 {"metadata", Json::object()}};
  CheckCollector checks(&report["checks"]);
  if (!hash_full_contents) {
    checks.Add(
        "WARN", "operation", "prefix_validation_mode",
        {{"max_events", options.max_events},
         {"full_root_sha256", false},
         {"external_artifact_sha256", false}},
        "full validation for definitive whole-file conclusions",
        "Prefix mode scans only the leading event limit and deliberately avoids full-file content hashes; all positive conclusions are limited to that prefix.");
  }

  const int previous_error_level = gErrorIgnoreLevel;
  gErrorIgnoreLevel = kError;
  // ROOT opens a /proc/self/fd path that resolves to the already-authenticated
  // inode.  A concurrent rename of the public path therefore cannot make the
  // scan consume different bytes than the hashes in this report.
  std::unique_ptr<TFile> file(TFile::Open(pinned_input_path.c_str(), "READ"));
  gErrorIgnoreLevel = previous_error_level;
  if (!file || file->IsZombie()) {
    throw std::runtime_error("Input is not a readable ROOT file: " +
                             absolute_path);
  }

  checks.Add("PASS", "integrity", "root_file_open", "readable",
             "readable non-zombie ROOT file", "ROOT opened the file in READ mode.");
  const Long64_t root_end = file->GetEND();
  const bool root_length_matches =
      root_end >= 0 && static_cast<std::uint64_t>(root_end) == initial_identity.size;
  checks.Add(root_length_matches ? "PASS" : "FAIL", "integrity",
             "root_logical_size", root_end, initial_identity.size,
             root_length_matches
                 ? "ROOT logical EOF matches the physical file size."
                 : "Bytes exist outside ROOT's logical EOF or the file is truncated.");
  const bool recovered = file->TestBit(TFile::kRecovered);
  checks.Add(recovered ? "FAIL" : "PASS", "integrity", "root_recovered",
             recovered, false,
             recovered ? "ROOT marked this file as recovered after an unclean write."
                       : "ROOT file is not marked as recovered.");

  Progress(options, 8.0, "validating provenance");
  std::optional<std::string> config_contents;
  MetadataState metadata = ValidateMetadata(
      *file, &checks, &report["metadata"], &config_contents);
  report["legacy"] = !metadata.present;
  const bool falling_polarity =
      !metadata.hardware.valid || metadata.hardware.polarity == "falling";
  ValidateExternalProvenance(*file, metadata, config_contents, &checks,
                             options, hash_full_contents);

  const std::optional<int> run_number =
      ReadParameter<int>(*file, "RunNumber");
  if (!run_number) {
    checks.Add("FAIL", "provenance", "run_number", nullptr,
               "positive TParameter<int>", "RunNumber object is missing or has the wrong type.");
  } else {
    const bool positive = *run_number > 0;
    const bool metadata_match =
        !metadata.parsed || metadata.run_number == *run_number;
    checks.Add(positive && metadata_match ? "PASS" : "FAIL", "provenance",
               "run_number", *run_number,
               metadata.parsed ? Json(metadata.run_number)
                               : Json("positive integer"),
               !positive
                   ? "RunNumber must be positive; zero is legacy/untraceable."
                   : metadata_match
                         ? "ROOT and runtime metadata run numbers agree."
                         : "ROOT and runtime metadata run numbers disagree.");
  }
  report["summary"]["run_number"] =
      run_number ? Json(*run_number) : Json(nullptr);

  Progress(options, 17.0, "validating ROOT schema");
  auto* tree = dynamic_cast<TTree*>(file->Get("phys_tree"));
  if (tree == nullptr) {
    checks.Add("FAIL", "schema", "physics_tree", nullptr,
               "TTree phys_tree", "Required physics tree is absent or has the wrong type.");
  } else {
    checks.Add("PASS", "schema", "physics_tree", tree->ClassName(),
               "TTree phys_tree", "Physics tree is present.");
  }

  const char* const count_parameter_type =
      metadata.present ? "TParameter<Long64_t>" : "TParameter<int>";
  const std::array<std::pair<const char*, const char*>, 6> summary_types = {{
      {"RealTime_sec", "TParameter<double>"},
      {"LiveTime_sec", "TParameter<double>"},
      {"DeadTime_pct", "TParameter<double>"},
      {"LostEvents_count", count_parameter_type},
      {"RecordedEvents_count", count_parameter_type},
      {"TriggerRate_Hz", "TParameter<double>"}}};
  Json summary_schema_errors = Json::array();
  for (const auto& [name, type] : summary_types) {
    if (!IsExpectedObject(*file, name, type)) {
      TObject* object = file->Get(name);
      summary_schema_errors.push_back(
          {{"name", name},
           {"observed", object ? Json(object->ClassName()) : Json(nullptr)},
           {"expected", type}});
    }
  }
  checks.Add(summary_schema_errors.empty() ? "PASS" : "FAIL", "schema",
             "summary_objects", summary_schema_errors, Json::array(),
             summary_schema_errors.empty()
                 ? "All conversion-summary objects have expected ROOT types."
                 : "Conversion-summary objects are missing or mistyped.");

  std::uint64_t total_entries = 0U;
  bool scalar_schema_valid = tree != nullptr;
  bool has_waveforms = false;
  bool has_audit_branches = false;
  if (tree != nullptr) {
    const Long64_t signed_entries = tree->GetEntries();
    if (signed_entries < 0) {
      scalar_schema_valid = false;
      checks.Add("FAIL", "integrity", "tree_entries", signed_entries,
                 "non-negative", "TTree returned an invalid entry count.");
    } else {
      total_entries = static_cast<std::uint64_t>(signed_entries);
    }
    struct ScalarBranchSpec {
      std::string name;
      std::string type;
      EDataType data_type;
    };
    std::vector<ScalarBranchSpec> specifications = {
        {"EventID", "UInt_t", kUInt_t},
        {"SyncTime_TTT", "ULong64_t", kULong64_t},
        {"ChannelMask", "UShort_t", kUShort_t},
        {"RecordLength", "UInt_t", kUInt_t}};
    for (int channel = 0; channel < kChannelCount; ++channel) {
      for (const char* prefix : {"Charge_CH", "PulseHeight_CH",
                                 "PulseStart_T0_CH", "Baseline_CH"}) {
        specifications.push_back(
            {std::string(prefix) + std::to_string(channel), "Double_t",
             kDouble_t});
      }
    }
    const bool pattern_present = tree->GetBranch("Pattern") != nullptr;
    const bool board_counter_present =
        tree->GetBranch("BoardEventCounter") != nullptr;
    const bool inspect_audit_branches =
        metadata.present || pattern_present || board_counter_present;
    if (inspect_audit_branches) {
      specifications.push_back({"Pattern", "UShort_t", kUShort_t});
      specifications.push_back(
          {"BoardEventCounter", "UInt_t", kUInt_t});
    }
    Json branch_errors = Json::array();
    Json audit_branch_errors = Json::array();
    for (const ScalarBranchSpec& specification : specifications) {
      TBranch* branch = tree->GetBranch(specification.name.c_str());
      TLeaf* leaf =
          branch == nullptr ? nullptr : branch->GetLeaf(specification.name.c_str());
      const std::string observed_type =
          leaf == nullptr ? "" : std::string(leaf->GetTypeName());
      TClass* expected_class = nullptr;
      EDataType expected_data_type = kOther_t;
      const bool branch_type_matches =
          branch != nullptr &&
          branch->GetExpectedType(expected_class, expected_data_type) == 0 &&
          expected_class == nullptr &&
          expected_data_type == specification.data_type;
      const bool scalar_shape =
          branch != nullptr && leaf != nullptr && branch->GetNleaves() == 1 &&
          branch->GetListOfLeaves() != nullptr &&
          branch->GetListOfLeaves()->GetEntriesFast() == 1 &&
          branch->GetListOfBranches() != nullptr &&
          branch->GetListOfBranches()->GetEntriesFast() == 0 &&
          leaf->GetBranch() == branch && leaf->GetLeafCount() == nullptr &&
          leaf->GetLenStatic() == 1 && leaf->GetLen() == 1;
      const bool entries_match =
          branch != nullptr && branch->GetEntries() ==
                                   static_cast<Long64_t>(total_entries);
      if (branch == nullptr || leaf == nullptr ||
          observed_type != specification.type || !branch_type_matches ||
          !scalar_shape || !entries_match) {
        Json& errors =
            specification.name == "Pattern" ||
                    specification.name == "BoardEventCounter"
                ? audit_branch_errors
                : branch_errors;
        errors.push_back(
            {{"name", specification.name},
             {"type", observed_type.empty() ? Json(nullptr)
                                             : Json(observed_type)},
             {"branch_expected_type_matches", branch_type_matches},
             {"leaf_count",
              branch ? Json(branch->GetNleaves()) : Json(nullptr)},
             {"leaf_length_static",
              leaf ? Json(leaf->GetLenStatic()) : Json(nullptr)},
             {"leaf_length",
              leaf ? Json(leaf->GetLen()) : Json(nullptr)},
             {"has_variable_leaf_count",
              leaf ? Json(leaf->GetLeafCount() != nullptr) : Json(nullptr)},
             {"entries", branch ? Json(branch->GetEntries()) : Json(nullptr)},
             {"expected_type", specification.type},
             {"expected_scalar_shape", true},
             {"expected_entries", total_entries}});
      }
    }
    const bool audit_branch_set_valid =
        inspect_audit_branches && pattern_present && board_counter_present &&
        audit_branch_errors.empty();
    has_audit_branches = audit_branch_set_valid;
    const bool audit_schema_valid =
        metadata.present ? audit_branch_set_valid
                         : (!inspect_audit_branches || audit_branch_set_valid);
    scalar_schema_valid = scalar_schema_valid && branch_errors.empty() &&
                          audit_schema_valid;
    checks.Add(branch_errors.empty() ? "PASS" : "FAIL", "schema",
               "scalar_branches", branch_errors, Json::array(),
               branch_errors.empty()
                   ? "All 36 required scalar branches have expected leaf types and entry counts."
                   : "Required scalar branches are missing, mistyped, or have inconsistent entries.");
    checks.Add(metadata.present
                   ? (audit_schema_valid ? "PASS" : "FAIL")
                   : (inspect_audit_branches
                          ? (audit_schema_valid ? "PASS" : "FAIL")
                          : "SKIP"),
               "schema", "audit_branches",
               {{"pattern_present", pattern_present},
                {"board_event_counter_present", board_counter_present},
                {"errors", audit_branch_errors}},
               metadata.present
                   ? Json("Pattern/UShort_t and BoardEventCounter/UInt_t")
                   : Json("both audit branches together when present"),
               !metadata.present && !inspect_audit_branches
                   ? "Legacy format predates raw-header audit branches."
                   : audit_schema_valid
                         ? "Raw Pattern and BoardEventCounter audit fields are preserved with exact scalar types."
                         : "Audit branches are missing, partial, mistyped, or have inconsistent entries.");

    int waveform_count = 0;
    Json waveform_errors = Json::array();
    TClass* const waveform_class =
        TClass::GetClass(typeid(std::vector<UShort_t>));
    for (int channel = 0; channel < kChannelCount; ++channel) {
      const std::string name = "Waveform_CH" + std::to_string(channel);
      TBranch* branch = tree->GetBranch(name.c_str());
      if (branch == nullptr) continue;
      ++waveform_count;
      const std::string class_name = branch->GetClassName();
      TClass* actual_class = nullptr;
      EDataType actual_data_type = kOther_t;
      const bool class_valid =
          waveform_class != nullptr &&
          branch->GetExpectedType(actual_class, actual_data_type) == 0 &&
          actual_class == waveform_class && actual_data_type == kOther_t;
      if (!class_valid ||
          branch->GetEntries() != static_cast<Long64_t>(total_entries)) {
        waveform_errors.push_back(
            {{"name", name},
             {"class", class_name},
             {"expected_type_api_matches", class_valid},
             {"entries", branch->GetEntries()},
             {"expected_class", "vector<unsigned short>"},
             {"expected_entries", total_entries}});
      }
    }
    has_waveforms = waveform_count == kChannelCount;
    const bool waveform_schema_valid =
        (waveform_count == 0 || has_waveforms) && waveform_errors.empty();
    scalar_schema_valid = scalar_schema_valid && waveform_schema_valid;
    checks.Add(waveform_schema_valid ? "PASS" : "FAIL", "schema",
               "waveform_branches",
               {{"count", waveform_count}, {"errors", waveform_errors}},
               "either zero or eight vector<unsigned short> branches",
               waveform_count == 0
                   ? "Waveforms were intentionally dropped during conversion."
                   : waveform_schema_valid
                         ? "All eight optional waveform branches are valid."
                         : "Waveform branch set is partial or malformed.");
  }

  report["summary"]["entries"] = total_entries;
  report["summary"]["waveforms_saved"] = has_waveforms;
  report["summary"]["audit_branches_saved"] = has_audit_branches;
  if (metadata.hardware.valid && metadata.parsed) {
    const std::uint64_t active_channels = static_cast<std::uint64_t>(
        __builtin_popcount(metadata.hardware.record_mask));
    const std::uint64_t bytes_per_event =
        sizeof(EventHeader) +
        static_cast<std::uint64_t>(metadata.hardware.record_length) *
            active_channels * sizeof(std::uint16_t);
    const bool size_computable =
        bytes_per_event == 0U ||
        total_entries <=
            std::numeric_limits<std::uint64_t>::max() / bytes_per_event;
    const std::uint64_t expected_raw_size =
        size_computable ? total_entries * bytes_per_event : 0U;
    const std::uint64_t recorded_raw_size = RequireUnsigned(
        metadata.document, "raw_output_size_bytes", "metadata", 0U,
        std::numeric_limits<std::uint64_t>::max());
    checks.Add(size_computable && recorded_raw_size == expected_raw_size
                   ? "PASS"
                   : "FAIL",
               "integrity", "raw_tree_size_consistency",
               recorded_raw_size,
               size_computable ? Json(expected_raw_size) : Json("no overflow"),
               "Raw size must equal entry_count * (24-byte header + active "
               "waveform payload) for the authenticated event shape.");
  }
  const std::uint64_t scan_limit =
      options.max_events == 0U
          ? total_entries
          : std::min(options.max_events, total_entries);
  const bool sampled = scan_limit < total_entries;
  const std::uint64_t sample_stride =
      std::max<std::uint64_t>(1U,
                              (scan_limit + kQuantileSampleLimit - 1U) /
                                  kQuantileSampleLimit);
  const std::uint64_t baseline_bin_size =
      std::max<std::uint64_t>(100U, (scan_limit + 999U) / 1000U);
  const std::uint64_t tail_start = scan_limit * 4U / 5U;

  std::array<ChannelAccumulator, kChannelCount> accumulators{};
  std::vector<LegacyPairExtremum> legacy_pair_extrema;
  legacy_pair_extrema.reserve(static_cast<std::size_t>(
      std::min<std::uint64_t>(scan_limit, kQuantileSampleLimit)));
  std::uint64_t scanned = 0U;
  std::uint64_t event_id_violations = 0U;
  std::uint64_t ttt_nonincreasing = 0U;
  std::uint64_t shape_violations = 0U;
  std::uint64_t read_failures = 0U;
  std::uint64_t routing_evaluable = 0U;
  std::uint64_t routing_passed = 0U;
  std::uint64_t board_counter_range_violations = 0U;
  std::uint64_t board_counter_duplicate_violations = 0U;
  std::uint64_t board_counter_backward_violations = 0U;
  std::uint64_t board_counter_lost_events = 0U;
  bool board_counter_loss_overflow = false;
  std::set<std::uint32_t> observed_masks;
  std::set<std::uint32_t> observed_lengths;
  std::set<std::uint32_t> observed_patterns;
  std::optional<std::uint64_t> first_ttt;
  std::optional<std::uint64_t> last_ttt;
  std::optional<std::uint32_t> previous_board_counter;
  bool scan_cancelled = false;

  UInt_t event_id = 0U;
  ULong64_t sync_time = 0U;
  UShort_t channel_mask = 0U;
  UInt_t record_length = 0U;
  UShort_t pattern = 0U;
  UInt_t board_event_counter = 0U;
  std::array<double, kChannelCount> charge{};
  std::array<double, kChannelCount> pulse{};
  std::array<double, kChannelCount> start_time{};
  std::array<double, kChannelCount> baseline{};
  std::array<std::vector<UShort_t>*, kChannelCount> waveforms{};

  if (tree != nullptr && scalar_schema_valid && scan_limit != 0U) {
    tree->SetBranchStatus("*", 0);
    int address_errors = 0;
    const auto bind = [&](const std::string& name, void* address) {
      tree->SetBranchStatus(name.c_str(), 1);
      if (tree->SetBranchAddress(name.c_str(), address) < 0) ++address_errors;
    };
    bind("EventID", &event_id);
    bind("SyncTime_TTT", &sync_time);
    bind("ChannelMask", &channel_mask);
    bind("RecordLength", &record_length);
    if (has_audit_branches) {
      bind("Pattern", &pattern);
      bind("BoardEventCounter", &board_event_counter);
    }
    for (int channel = 0; channel < kChannelCount; ++channel) {
      bind("Charge_CH" + std::to_string(channel), &charge[channel]);
      bind("PulseHeight_CH" + std::to_string(channel), &pulse[channel]);
      bind("PulseStart_T0_CH" + std::to_string(channel), &start_time[channel]);
      bind("Baseline_CH" + std::to_string(channel), &baseline[channel]);
      if (has_waveforms) {
        bind("Waveform_CH" + std::to_string(channel), &waveforms[channel]);
      }
    }
    if (address_errors != 0) {
      checks.Add("FAIL", "schema", "branch_binding", address_errors, 0,
                 "ROOT rejected one or more validated branch addresses.");
      scalar_schema_valid = false;
    } else {
      checks.Add("PASS", "schema", "branch_binding", 0, 0,
                 "All scan branches were bound successfully.");
    }
  }

  if (tree != nullptr && scalar_schema_valid && scan_limit != 0U) {
    Progress(options, 25.0, "scanning events");
    const std::uint64_t progress_interval =
        std::max<std::uint64_t>(1U, scan_limit / 100U);
    for (std::uint64_t entry = 0U; entry < scan_limit; ++entry) {
      if (IsCancelled(options)) {
        scan_cancelled = true;
        break;
      }
      if (tree->GetEntry(static_cast<Long64_t>(entry)) <= 0) {
        ++read_failures;
        break;
      }
      if (event_id != entry) ++event_id_violations;
      if (!first_ttt) first_ttt = sync_time;
      if (last_ttt && sync_time <= *last_ttt) ++ttt_nonincreasing;
      last_ttt = sync_time;
      observed_masks.insert(channel_mask);
      observed_lengths.insert(record_length);
      if (has_audit_branches) {
        observed_patterns.insert(pattern);
        if (board_event_counter > 0xFFFFFFU) {
          ++board_counter_range_violations;
          previous_board_counter.reset();
        } else {
          if (previous_board_counter) {
            const std::uint32_t difference =
                (board_event_counter - *previous_board_counter) & 0xFFFFFFU;
            if (difference == 0U) {
              ++board_counter_duplicate_violations;
            } else if (difference > 0x800000U) {
              ++board_counter_backward_violations;
            } else if (difference > 1U) {
              const std::uint64_t newly_lost =
                  static_cast<std::uint64_t>(difference - 1U);
              if (board_counter_lost_events >
                  std::numeric_limits<std::uint64_t>::max() - newly_lost) {
                board_counter_loss_overflow = true;
              } else {
                board_counter_lost_events += newly_lost;
              }
            }
          }
          previous_board_counter = board_event_counter;
        }
      }
      const std::uint32_t required_granularity = metadata.present ? 8U : 4U;
      const bool shape_is_valid =
          channel_mask != 0U && (channel_mask & ~0xFFU) == 0U &&
          record_length >= 128U && record_length <= 102400U &&
          record_length % required_granularity == 0U;
      if (!shape_is_valid ||
          (metadata.hardware.valid &&
           (channel_mask != metadata.hardware.record_mask ||
            record_length != metadata.hardware.record_length))) {
        ++shape_violations;
      }

      std::array<bool, kChannelCount> crossings{};
      std::array<bool, kChannelCount> crossing_available{};
      for (int channel = 0; channel < kChannelCount; ++channel) {
        ChannelAccumulator& accumulator = accumulators[channel];
        const bool active = ((channel_mask >> channel) & 1U) != 0U;
        const bool finite = std::isfinite(charge[channel]) &&
                            std::isfinite(pulse[channel]) &&
                            std::isfinite(start_time[channel]) &&
                            std::isfinite(baseline[channel]);
        if (!active) {
          if (!finite || charge[channel] != 0.0 || pulse[channel] != 0.0 ||
              start_time[channel] != -1.0 || baseline[channel] != 0.0) {
            ++accumulator.inactive_sentinel_violations;
          }
          if (has_waveforms && waveforms[channel] != nullptr &&
              !waveforms[channel]->empty()) {
            ++accumulator.waveform_size_violations;
          }
          continue;
        }
        ++accumulator.active_events;
        if (!finite) {
          ++accumulator.finite_violations;
          continue;
        }
        const double theoretical_charge_max =
            static_cast<double>(record_length) * kAdcMaximum;
        const double pulse_height_limit =
            falling_polarity ? baseline[channel]
                             : kAdcMaximum - baseline[channel];
        if (baseline[channel] < 0.0 || baseline[channel] > kAdcMaximum ||
            pulse[channel] < 0.0 ||
            pulse[channel] > pulse_height_limit + 1e-9 ||
            charge[channel] < 0.0 ||
            charge[channel] > theoretical_charge_max + 1e-9) {
          ++accumulator.range_violations;
        }
        const double maximum_t0 =
            record_length == 0U
                ? -1.0
                : 2.0 * (static_cast<double>(record_length) - 1.0);
        const bool t0_in_range =
            start_time[channel] == -1.0 ||
            (start_time[channel] >= 0.0 &&
             start_time[channel] <= maximum_t0 &&
             std::fmod(start_time[channel], 2.0) == 0.0);
        const bool t0_consistent =
            (start_time[channel] >= 0.0) == (pulse[channel] > 30.0);
        if (!t0_in_range || !t0_consistent) ++accumulator.t0_violations;
        if (start_time[channel] >= 0.0) ++accumulator.t0_found;
        accumulator.pulse_min =
            std::min(accumulator.pulse_min, pulse[channel]);
        accumulator.pulse_max =
            std::max(accumulator.pulse_max, pulse[channel]);
        accumulator.charge_min =
            std::min(accumulator.charge_min, charge[channel]);
        accumulator.charge_max =
            std::max(accumulator.charge_max, charge[channel]);
        const double pulse_extremum =
            falling_polarity ? baseline[channel] - pulse[channel]
                             : baseline[channel] + pulse[channel];
        bool saturated = falling_polarity
                             ? pulse_extremum <= 0.5
                             : pulse_extremum >= kAdcMaximum - 0.5;

        if ((entry % sample_stride) == 0U) {
          accumulator.baseline_samples.push_back(baseline[channel]);
          accumulator.bin_samples.push_back(baseline[channel]);
          if (entry >= tail_start) {
            accumulator.tail_samples.push_back(baseline[channel]);
          }
        }

        if (has_waveforms) {
          if (waveforms[channel] == nullptr ||
              waveforms[channel]->size() != record_length) {
            ++accumulator.waveform_size_violations;
          } else {
            bool waveform_range_error = false;
            for (const UShort_t sample : *waveforms[channel]) {
              if (sample > static_cast<UShort_t>(kAdcMaximum)) {
                waveform_range_error = true;
                break;
              }
              if (sample == 0U ||
                  sample == static_cast<UShort_t>(kAdcMaximum)) {
                saturated = true;
              }
            }
            if (waveform_range_error) ++accumulator.waveform_range_violations;
            if (!waveform_range_error) {
              const DerivedWaveformValues derived =
                  RecomputeWaveformValues(*waveforms[channel],
                                          falling_polarity);
              if (!ApproxEqual(derived.baseline, baseline[channel]) ||
                  !ApproxEqual(derived.charge, charge[channel]) ||
                  !ApproxEqual(derived.pulse_height, pulse[channel]) ||
                  derived.pulse_start_ns != start_time[channel]) {
                ++accumulator.waveform_dsp_mismatches;
              }
            }
          }
        }
        if (saturated) ++accumulator.saturation_events;

        const ChannelMetadata& threshold = metadata.channels[channel];
        if (threshold.present && threshold.trigger_enabled &&
            threshold.readback_available) {
          if (has_waveforms && waveforms[channel] != nullptr &&
              waveforms[channel]->size() == record_length) {
            crossing_available[channel] = true;
            if (metadata.hardware.polarity == "falling") {
              crossings[channel] = std::any_of(
                  waveforms[channel]->begin(), waveforms[channel]->end(),
                  [&](UShort_t sample) {
                    return sample <= threshold.readback_adc;
                  });
            } else {
              crossings[channel] = std::any_of(
                  waveforms[channel]->begin(), waveforms[channel]->end(),
                  [&](UShort_t sample) {
                    return sample >= threshold.readback_adc;
                  });
            }
          } else {
            crossing_available[channel] = true;
            crossings[channel] = falling_polarity
                                     ? pulse_extremum <= static_cast<double>(
                                           threshold.readback_adc)
                                     : pulse_extremum >= static_cast<double>(
                                           threshold.readback_adc);
          }
          if (crossing_available[channel]) {
            ++accumulator.trigger_evaluable;
            if (crossings[channel]) ++accumulator.trigger_crossings;
          }
        }
      }

      if ((entry % sample_stride) == 0U && (channel_mask & 0x3U) == 0x3U) {
        legacy_pair_extrema.push_back(
            {entry, baseline[0] - pulse[0], baseline[1] - pulse[1]});
      }

      if (metadata.hardware.valid && metadata.hardware.explicit_routing &&
          metadata.hardware.external_mode == 0U &&
          metadata.hardware.self_mode != 0U) {
        bool evaluable = true;
        bool global_condition = false;
        for (int pair = 0; pair < 4; ++pair) {
          const std::uint32_t bits =
              (metadata.hardware.self_mask >> (2 * pair)) & 0x3U;
          if (bits == 0U) continue;
          const int low = pair * 2;
          const int high = low + 1;
          bool pair_condition = false;
          if (bits == 1U) {
            evaluable = evaluable && crossing_available[low];
            pair_condition = crossings[low];
          } else if (bits == 2U) {
            evaluable = evaluable && crossing_available[high];
            pair_condition = crossings[high];
          } else {
            evaluable = evaluable && crossing_available[low] &&
                        crossing_available[high];
            if (metadata.hardware.pair_logic == "AND") {
              if (has_waveforms && crossing_available[low] &&
                  crossing_available[high] && waveforms[low] != nullptr &&
                  waveforms[high] != nullptr &&
                  waveforms[low]->size() == waveforms[high]->size()) {
                const ChannelMetadata& low_threshold = metadata.channels[low];
                const ChannelMetadata& high_threshold = metadata.channels[high];
                for (std::size_t sample = 0U;
                     sample < waveforms[low]->size(); ++sample) {
                  const bool low_active =
                      metadata.hardware.polarity == "falling"
                          ? (*waveforms[low])[sample] <= low_threshold.readback_adc
                          : (*waveforms[low])[sample] >= low_threshold.readback_adc;
                  const bool high_active =
                      metadata.hardware.polarity == "falling"
                          ? (*waveforms[high])[sample] <= high_threshold.readback_adc
                          : (*waveforms[high])[sample] >= high_threshold.readback_adc;
                  if (low_active && high_active) {
                    pair_condition = true;
                    break;
                  }
                }
              } else {
                // Scalar PulseHeight proves only that both channels crossed at
                // some point in the record. It cannot prove that the two
                // comparator levels overlapped in time as hardware AND needs.
                evaluable = false;
              }
            } else {
              pair_condition = crossings[low] || crossings[high];
            }
          }
          global_condition = global_condition || pair_condition;
        }
        if (evaluable) {
          ++routing_evaluable;
          if (global_condition) ++routing_passed;
        }
      }

      ++scanned;
      if (scanned % baseline_bin_size == 0U) {
        FlushBaselineBins(&accumulators);
      }
      if ((scanned % progress_interval) == 0U || scanned == scan_limit) {
        Progress(options,
                 25.0 + 55.0 * static_cast<double>(scanned) /
                            static_cast<double>(scan_limit),
                 "scanning events");
      }
    }
    FlushBaselineBins(&accumulators);
  } else if (scan_limit == 0U && tree != nullptr && scalar_schema_valid) {
    checks.Add("WARN", "data_quality", "empty_tree", 0, ">0 events",
               "The ROOT schema is valid but the physics tree is empty.");
  } else if (!scalar_schema_valid) {
    checks.Add("SKIP", "integrity", "event_scan", "not run",
               "valid branch schema",
               "Event scan was skipped because the tree schema is unsafe to bind.");
  }

  Progress(options, 83.0, "evaluating event integrity");
  const bool partial_event_scan = scanned < total_entries;
  report["analysis"] = {
      {"completed", !scan_cancelled},
      {"event_scan_completed",
       scalar_schema_valid && !scan_cancelled && read_failures == 0U &&
           scanned == scan_limit},
      {"cancelled", scan_cancelled},
      {"events_total", total_entries},
      {"events_scanned", scanned},
      {"sampled", sampled || partial_event_scan},
      {"quantile_sample_stride", sample_stride}};
  if (!scan_cancelled && scalar_schema_valid) {
    checks.Add(partial_event_scan ? "WARN" : "PASS", "operation",
               "analysis_coverage",
               {{"events_scanned", scanned}, {"events_total", total_entries}},
               "all events",
               partial_event_scan
                   ? "This is a prefix sample; observations apply only to scanned events and the overall report remains WARN/PARTIAL."
                   : "Every event in phys_tree was scanned.");
  }
  if (scan_cancelled) {
    checks.Add("WARN", "operation", "cancelled", scanned, scan_limit,
               "Validation was cancelled by SIGINT/SIGTERM; partial metrics are reported.");
  }
  if (tree != nullptr && scalar_schema_valid && scan_limit != 0U) {
    checks.Add(read_failures != 0U
                   ? "FAIL"
                   : partial_event_scan ? "SKIP" : "PASS",
               "integrity",
               "tree_read", read_failures, 0,
               read_failures != 0U
                   ? "ROOT failed while reading an event entry."
               : partial_event_scan
                   ? "Every prefix entry was readable, but the remainder was not scanned."
                   : "Every TTree entry was readable.");
    checks.Add(event_id_violations != 0U
                   ? "FAIL"
                   : partial_event_scan ? "SKIP" : "PASS",
               "integrity",
               "event_id_contiguous", event_id_violations, 0,
               partial_event_scan && event_id_violations == 0U
                   ? "EventID is contiguous in the scanned prefix; the full file was not established."
                   : "EventID must equal its zero-based tree entry index.");
    checks.Add(ttt_nonincreasing != 0U
                   ? "FAIL"
                   : partial_event_scan ? "SKIP" : "PASS",
               "integrity",
               "trigger_time_monotonic", ttt_nonincreasing, 0,
               partial_event_scan && ttt_nonincreasing == 0U
                   ? "Timestamps increase in the scanned prefix; the full file was not established."
                   : "Extended trigger timestamps must be strictly increasing.");
    if (has_audit_branches) {
      checks.Add(board_counter_range_violations != 0U
                     ? "FAIL"
                     : partial_event_scan ? "SKIP" : "PASS",
                 "integrity", "board_event_counter_range",
                 board_counter_range_violations, 0,
                 partial_event_scan && board_counter_range_violations == 0U
                     ? "BoardEventCounter stayed within its hardware 24-bit range in the scanned prefix."
                     : "BoardEventCounter must stay within its hardware 24-bit range.");
      checks.Add(board_counter_loss_overflow
                     ? "FAIL"
                     : partial_event_scan ? "SKIP" : "PASS",
                 "integrity", "board_counter_loss_arithmetic",
                 {{"overflow", board_counter_loss_overflow},
                  {"reconstructed_lost_events", board_counter_lost_events}},
                 "overflow-free unsigned 64-bit reconstruction",
                 partial_event_scan && !board_counter_loss_overflow
                     ? "Lost-event reconstruction is overflow-free for the scanned prefix."
                     : "Modulo-24-bit board-counter deltas must accumulate without overflow.");
      const bool counter_sequence_invalid =
          board_counter_duplicate_violations != 0U ||
          board_counter_backward_violations != 0U;
      checks.Add(counter_sequence_invalid
                     ? "FAIL"
                     : partial_event_scan ? "SKIP" : "PASS",
                 "integrity", "board_event_counter_sequence",
                 {{"duplicate_or_stale", board_counter_duplicate_violations},
                  {"backward_or_reset", board_counter_backward_violations}},
                 {{"duplicate_or_stale", 0}, {"backward_or_reset", 0}},
                 counter_sequence_invalid
                     ? "BoardEventCounter contains a duplicate/stale value or a backward/reset transition; neither can be interpreted as ordinary event loss."
                 : partial_event_scan
                     ? "BoardEventCounter advances unambiguously in the scanned prefix; the remainder was not scanned."
                     : "BoardEventCounter advances monotonically modulo 24 bits, including valid rollover.");
    }
    const bool shape_constant = observed_masks.size() <= 1U &&
                                observed_lengths.size() <= 1U &&
                                shape_violations == 0U;
    checks.Add(!shape_constant
                   ? "FAIL"
                   : partial_event_scan ? "SKIP" : "PASS",
               "integrity",
               "event_shape",
               {{"channel_masks", observed_masks},
                {"record_lengths", observed_lengths},
                {"metadata_mismatches", shape_violations}},
               metadata.hardware.valid
                   ? Json({{"channel_mask", metadata.hardware.record_mask},
                           {"record_length", metadata.hardware.record_length}})
                   : Json("one constant valid mask and record length"),
               shape_constant && partial_event_scan
                   ? "Channel mask and record length agree throughout the prefix; the remainder was not scanned."
                   : "Channel mask and record length must be constant and agree with metadata.");
    if (!metadata.present &&
        std::any_of(observed_lengths.begin(), observed_lengths.end(),
                    [](std::uint32_t length) { return length % 8U != 0U; })) {
      checks.Add("WARN", "compatibility", "legacy_record_length_granularity",
                 observed_lengths, "current files use a multiple of 8",
                 "This legacy file uses the older valid x730 multiple-of-4 "
                 "record-length convention; it is not classified as data "
                 "corruption.");
    }
  }

  report["summary"]["patterns"] = observed_patterns;
  report["summary"]["board_counter_reconstructed_lost_events"] =
      has_audit_branches ? Json(board_counter_lost_events) : Json(nullptr);
  report["summary"]["board_counter_duplicate_or_stale_events"] =
      has_audit_branches ? Json(board_counter_duplicate_violations)
                         : Json(nullptr);
  report["summary"]["board_counter_backward_or_reset_events"] =
      has_audit_branches ? Json(board_counter_backward_violations)
                         : Json(nullptr);

  const std::optional<double> real_time =
      ReadParameter<double>(*file, "RealTime_sec");
  const std::optional<double> live_time =
      ReadParameter<double>(*file, "LiveTime_sec");
  const std::optional<double> dead_time_percent =
      ReadParameter<double>(*file, "DeadTime_pct");
  std::optional<Long64_t> lost_events;
  std::optional<Long64_t> recorded_events;
  if (metadata.present) {
    lost_events = ReadParameter<Long64_t>(*file, "LostEvents_count");
    recorded_events =
        ReadParameter<Long64_t>(*file, "RecordedEvents_count");
  } else {
    if (const std::optional<int> legacy_lost =
            ReadParameter<int>(*file, "LostEvents_count")) {
      lost_events = static_cast<Long64_t>(*legacy_lost);
    }
    if (const std::optional<int> legacy_recorded =
            ReadParameter<int>(*file, "RecordedEvents_count")) {
      recorded_events = static_cast<Long64_t>(*legacy_recorded);
    }
  }
  const std::optional<double> trigger_rate =
      ReadParameter<double>(*file, "TriggerRate_Hz");
  report["summary"]["recorded_events"] =
      recorded_events ? Json(*recorded_events) : Json(nullptr);
  report["summary"]["lost_events"] =
      lost_events ? Json(*lost_events) : Json(nullptr);
  report["summary"]["real_time_sec"] =
      real_time ? Json(*real_time) : Json(nullptr);
  report["summary"]["live_time_sec"] =
      live_time ? Json(*live_time) : Json(nullptr);
  report["summary"]["dead_time_pct"] =
      dead_time_percent ? Json(*dead_time_percent) : Json(nullptr);
  report["summary"]["trigger_rate_hz"] =
      trigger_rate ? Json(*trigger_rate) : Json(nullptr);
  report["summary"]["channel_masks"] = observed_masks;
  report["summary"]["record_lengths"] = observed_lengths;

  const bool summary_numbers_valid =
      real_time && live_time && dead_time_percent && trigger_rate &&
      std::isfinite(*real_time) && *real_time >= 0.0 &&
      std::isfinite(*live_time) && *live_time >= 0.0 &&
      std::isfinite(*dead_time_percent) && *dead_time_percent >= 0.0 &&
      std::isfinite(*trigger_rate) && *trigger_rate >= 0.0 &&
      *live_time <= *real_time + 1e-9;
  const bool summary_counts_valid =
      recorded_events && lost_events && *recorded_events >= 0 &&
      *lost_events >= 0;
  checks.Add(summary_numbers_valid && summary_counts_valid ? "PASS" : "FAIL",
             "summary", "summary_value_ranges",
             {{"real_time_sec", real_time ? Json(*real_time) : Json(nullptr)},
              {"live_time_sec", live_time ? Json(*live_time) : Json(nullptr)},
              {"dead_time_pct",
               dead_time_percent ? Json(*dead_time_percent) : Json(nullptr)},
              {"trigger_rate_hz",
               trigger_rate ? Json(*trigger_rate) : Json(nullptr)},
              {"recorded_events",
               recorded_events ? Json(*recorded_events) : Json(nullptr)},
              {"lost_events", lost_events ? Json(*lost_events) : Json(nullptr)}},
             "finite non-negative summary values and LiveTime <= RealTime",
             "Conversion summary values must be finite, non-negative, and internally ordered.");

  if (recorded_events) {
    checks.Add(*recorded_events >= 0 &&
                       static_cast<std::uint64_t>(*recorded_events) ==
                           total_entries
                   ? "PASS"
                   : "FAIL",
               "summary", "recorded_event_count", *recorded_events,
               total_entries,
               "RecordedEvents_count must equal the full TTree entry count.");
  }
  if (lost_events && has_audit_branches) {
    const bool complete_counter_scan =
        !partial_event_scan && !scan_cancelled && read_failures == 0U &&
        !board_counter_loss_overflow &&
        board_counter_range_violations == 0U &&
        board_counter_duplicate_violations == 0U &&
        board_counter_backward_violations == 0U;
    const bool counter_matches =
        complete_counter_scan && *lost_events >= 0 &&
        static_cast<std::uint64_t>(*lost_events) ==
            board_counter_lost_events;
    checks.Add(complete_counter_scan
                   ? (counter_matches ? "PASS" : "FAIL")
                   : "SKIP",
               "integrity", "board_counter_lost_event_count",
               complete_counter_scan
                   ? Json(board_counter_lost_events)
                   : Json({{"scanned_events", scanned},
                           {"reconstructed_prefix", board_counter_lost_events}}),
               *lost_events,
               complete_counter_scan
                   ? "LostEvents_count must exactly equal the modulo-24-bit BoardEventCounter reconstruction."
                   : "The full BoardEventCounter sequence was not available for comparison with LostEvents_count.");
  }
  if (lost_events) {
    const long double total_triggers =
        static_cast<long double>(total_entries) +
        (*lost_events >= 0 ? static_cast<long double>(*lost_events) : 0.0L);
    const double loss_fraction =
        total_triggers == 0.0L || *lost_events < 0
            ? 0.0
            : static_cast<double>(static_cast<long double>(*lost_events) /
                                  total_triggers);
    report["summary"]["lost_event_fraction"] = loss_fraction;
    const std::string status =
        *lost_events < 0 ? "FAIL"
        : loss_fraction > 0.01
            ? "FAIL"
            : *lost_events > 0 ? "WARN" : "PASS";
    checks.Add(status, "data_quality", "lost_events",
               {{"count", *lost_events}, {"fraction", loss_fraction}},
               {{"count", 0}, {"red_fraction", 0.01}},
               has_audit_branches
                   ? "LostEvents_count is independently reconstructed from preserved BoardEventCounter values."
                   : "Legacy phys_tree does not preserve BoardEventCounter, so LostEvents_count cannot be independently reconstructed.");
  }

  if (!sampled && !scan_cancelled && read_failures == 0U && total_entries > 0U &&
      first_ttt && last_ttt && observed_lengths.size() == 1U && real_time &&
      live_time && dead_time_percent && trigger_rate && summary_numbers_valid) {
    const double expected_real =
        *last_ttt > *first_ttt
            ? static_cast<double>(*last_ttt - *first_ttt) * 8e-9
            : 0.0;
    const double expected_dead =
        static_cast<double>(total_entries) *
        static_cast<double>(*observed_lengths.begin()) * 2e-9;
    const double expected_live = std::max(0.0, expected_real - expected_dead);
    const double expected_dead_percent =
        expected_real > 0.0 ? 100.0 * expected_dead / expected_real : 0.0;
    const double expected_rate =
        expected_real > 0.0
            ? static_cast<double>(total_entries) / expected_real
            : 0.0;
    const bool consistent =
        ApproxEqual(*real_time, expected_real) &&
        ApproxEqual(*live_time, expected_live) &&
        ApproxEqual(*dead_time_percent, expected_dead_percent) &&
        ApproxEqual(*trigger_rate, expected_rate);
    checks.Add(consistent ? "PASS" : "FAIL", "summary",
               "timing_summary",
               {{"real_time_sec", *real_time},
                {"live_time_sec", *live_time},
                {"dead_time_pct", *dead_time_percent},
                {"trigger_rate_hz", *trigger_rate}},
               {{"real_time_sec", expected_real},
                {"live_time_sec", expected_live},
                {"dead_time_pct", expected_dead_percent},
                {"trigger_rate_hz", expected_rate}},
               "Summary timing values must reproduce the converter equations exactly within numerical tolerance.");
  } else if (sampled) {
    checks.Add("SKIP", "summary", "timing_summary", "prefix scan",
               "complete event scan",
               "Timing summary recomputation requires the last tree entry.");
  }

  Progress(options, 90.0, "evaluating channels and thresholds");
  std::array<std::optional<std::uint64_t>, kChannelCount>
      channel_settling_events{};
  for (int channel = 0; channel < kChannelCount; ++channel) {
    ChannelAccumulator& accumulator = accumulators[channel];
    const ChannelMetadata& threshold = metadata.channels[channel];
    const bool active =
        !observed_masks.empty() &&
        (((*observed_masks.begin() >> channel) & 1U) != 0U);
    const bool baseline_available = !accumulator.baseline_samples.empty();
    const bool tail_available = !accumulator.tail_samples.empty();
    const double baseline_median =
        baseline_available ? Median(accumulator.baseline_samples) : 0.0;
    const double baseline_mad =
        baseline_available ? Mad(accumulator.baseline_samples, baseline_median)
                           : 0.0;
    const double tail_median =
        tail_available ? Median(accumulator.tail_samples) : baseline_median;
    const double tail_mad =
        tail_available ? Mad(accumulator.tail_samples, tail_median)
                       : baseline_mad;
    double bin_span = 0.0;
    if (!accumulator.bin_medians.empty()) {
      const auto endpoints = std::minmax_element(
          accumulator.bin_medians.begin(), accumulator.bin_medians.end());
      bin_span = *endpoints.second - *endpoints.first;
    }
    const double lsb_mv = metadata.hardware.valid
                              ? static_cast<double>(
                                    metadata.hardware.input_range_mvpp) /
                                    static_cast<double>(
                                        1U << metadata.hardware.adc_bits)
                              : 2000.0 / 16384.0;
    const double settling_tolerance = std::max(3.0, 0.25 / lsb_mv);
    const std::optional<std::uint64_t> settling_event =
        tail_available
            ? SettlingEvent(accumulator.bin_medians, tail_median,
                            baseline_bin_size, settling_tolerance)
            : std::nullopt;
    channel_settling_events[static_cast<std::size_t>(channel)] =
        settling_event;
    const double saturation_fraction =
        accumulator.active_events == 0U
            ? 0.0
            : static_cast<double>(accumulator.saturation_events) /
                  static_cast<double>(accumulator.active_events);
    const double t0_fraction =
        accumulator.active_events == 0U
            ? 0.0
            : static_cast<double>(accumulator.t0_found) /
                  static_cast<double>(accumulator.active_events);
    const double crossing_fraction =
        accumulator.trigger_evaluable == 0U
            ? 0.0
            : static_cast<double>(accumulator.trigger_crossings) /
                  static_cast<double>(accumulator.trigger_evaluable);

    Json threshold_json =
        {{"mode", threshold.present ? Json(threshold.mode) : Json(nullptr)},
         {"requested_mv", NullOrDouble(threshold.requested_available,
                                        threshold.requested_mv)},
         {"delta_adc", NullOrUnsigned(threshold.delta_available,
                                       threshold.delta_adc)},
         {"written_adc", NullOrUnsigned(threshold.written_available,
                                         threshold.written_adc)},
         {"readback_adc", NullOrUnsigned(threshold.readback_available,
                                          threshold.readback_adc)},
         {"effective_mv", NullOrDouble(threshold.effective_available,
                                        threshold.effective_mv)}};
    Json metrics =
        {{"events_scanned", accumulator.active_events},
         {"finite_violations", accumulator.finite_violations},
         {"range_violations", accumulator.range_violations},
         {"inactive_sentinel_violations",
          accumulator.inactive_sentinel_violations},
         {"waveform_size_violations", accumulator.waveform_size_violations},
         {"waveform_range_violations", accumulator.waveform_range_violations},
         {"waveform_dsp_mismatches", accumulator.waveform_dsp_mismatches},
         {"baseline_median_adc",
          NullOrDouble(baseline_available, baseline_median)},
         {"baseline_mad_adc", NullOrDouble(baseline_available, baseline_mad)},
         {"baseline_tail_median_adc",
          NullOrDouble(tail_available, tail_median)},
         {"baseline_tail_mad_adc", NullOrDouble(tail_available, tail_mad)},
         {"baseline_bin_span_adc",
          NullOrDouble(!accumulator.bin_medians.empty(), bin_span)},
         {"settling_event",
          settling_event ? Json(*settling_event) : Json(nullptr)},
         {"calibration_baseline_drift_adc", nullptr},
         {"actual_effective_threshold_mv", nullptr},
         {"pulse_height_min_adc",
          NullOrDouble(accumulator.active_events != 0U,
                       accumulator.pulse_min)},
         {"pulse_height_max_adc",
          NullOrDouble(accumulator.active_events != 0U,
                       accumulator.pulse_max)},
         {"charge_min_adc_samples",
          NullOrDouble(accumulator.active_events != 0U,
                       accumulator.charge_min)},
         {"charge_max_adc_samples",
          NullOrDouble(accumulator.active_events != 0U,
                       accumulator.charge_max)},
         {"t0_found_fraction", active ? Json(t0_fraction) : Json(nullptr)},
         {"t0_violations", accumulator.t0_violations},
         {"saturation_fraction",
          active ? Json(saturation_fraction) : Json(nullptr)},
         {"trigger_crossing_fraction",
          accumulator.trigger_evaluable != 0U ? Json(crossing_fraction)
                                              : Json(nullptr)}};

    const std::uint64_t exact_violations =
        accumulator.finite_violations + accumulator.range_violations +
        accumulator.inactive_sentinel_violations +
        accumulator.waveform_size_violations +
        accumulator.waveform_range_violations +
        accumulator.waveform_dsp_mismatches + accumulator.t0_violations;
    const bool channel_scan_available = scalar_schema_valid && scanned != 0U;
    checks.Add(exact_violations != 0U
                   ? "FAIL"
               : !channel_scan_available || partial_event_scan
                   ? "SKIP"
                   : "PASS",
               "physics_sanity",
               "channel_" + std::to_string(channel) + "_value_integrity",
               exact_violations, 0,
               !channel_scan_available
                   ? "Channel values were not scanned because the event schema was unavailable."
               : exact_violations == 0U && partial_event_scan
                   ? "No value violation was found in the prefix; the full file was not established."
                   : "Finite values, physical ranges, T0 semantics, inactive sentinels, waveform shape, and archived-waveform DSP recomputation are checked.");

    if (active && tail_available) {
      const double mad_warning = std::max(2.0, 0.25 / lsb_mv);
      const double span_warning = std::max(5.0, 0.5 / lsb_mv);
      const bool stable = tail_mad <= mad_warning && bin_span <= span_warning;
      checks.Add(stable && partial_event_scan ? "SKIP"
                                             : stable ? "PASS" : "WARN",
                 "data_quality",
                 "channel_" + std::to_string(channel) + "_baseline_stability",
                 {{"tail_mad_adc", tail_mad}, {"bin_span_adc", bin_span}},
                 {{"max_tail_mad_adc", mad_warning},
                  {"max_bin_span_adc", span_warning}},
                 stable && partial_event_scan
                     ? "The scanned prefix is stable, but its tail is not the file tail; a full scan is required for a positive stability verdict."
                 : stable ? "Baseline is stable under robust ADC/mV thresholds."
                        : "Baseline noise or drift exceeds the conservative warning threshold.");
      const std::uint64_t late_limit =
          std::max<std::uint64_t>(1000U, scan_limit / 100U);
      if (accumulator.bin_medians.size() < 5U) {
        checks.Add("SKIP", "data_quality",
                   "channel_" + std::to_string(channel) + "_settling",
                   {{"bins", accumulator.bin_medians.size()},
                    {"events", accumulator.active_events}},
                   {{"minimum_bins", 5},
                    {"tolerance_adc", settling_tolerance}},
                   partial_event_scan
                       ? "A sampled prefix cannot establish settling relative to the final file baseline."
                       : "The run is too short to provide five independent baseline bins, so settling is not rated.");
      } else {
        const bool settled_promptly =
            settling_event && *settling_event <= late_limit;
        checks.Add(settled_promptly && partial_event_scan
                       ? "SKIP"
                       : settled_promptly ? "PASS" : "WARN",
                   "data_quality",
                   "channel_" + std::to_string(channel) + "_settling",
                   settling_event ? Json(*settling_event) : Json(nullptr),
                   {{"maximum_event", late_limit},
                    {"tolerance_adc", settling_tolerance}},
                   settled_promptly && partial_event_scan
                       ? "The prefix appears settled only relative to its own tail; a full scan is required for a positive settling verdict."
                   : settled_promptly
                       ? "Baseline reaches the tail reference promptly."
                       : "Baseline settling is late or was not established in the scanned events.");
      }
      const std::string saturation_status =
          saturation_fraction > 0.01
              ? "FAIL"
              : saturation_fraction >= 0.001 ? "WARN" : "PASS";
      checks.Add(saturation_status == "PASS" && partial_event_scan
                     ? "SKIP"
                     : saturation_status,
                 "data_quality",
                 "channel_" + std::to_string(channel) + "_saturation",
                 saturation_fraction,
                 {{"warning_fraction", 0.001}, {"red_fraction", 0.01}},
                 saturation_status == "PASS" && partial_event_scan
                     ? "No saturation was found in the prefix, but absence across the full file was not established."
                 : has_waveforms
                     ? "Saturation is detected directly at ADC codes 0 or 16383."
                     : falling_polarity
                           ? "Without waveforms this is inferred from Baseline-PulseHeight <= 0.5 ADC."
                           : "Without waveforms this is inferred from Baseline+PulseHeight >= 16382.5 ADC.");
    }

    if (metadata.hardware.valid && active && tail_available && threshold.present &&
        threshold.trigger_enabled && threshold.readback_available &&
        threshold.requested_available && threshold.delta_available) {
      const double drift =
          std::abs(tail_median - threshold.measured_baseline);
      const double actual_delta =
          std::abs(tail_median - static_cast<double>(threshold.readback_adc));
      const double actual_mv = actual_delta * lsb_mv;
      metrics["calibration_baseline_drift_adc"] = drift;
      metrics["actual_effective_threshold_mv"] = actual_mv;
      const double error_adc =
          std::abs(actual_delta - static_cast<double>(threshold.delta_adc));
      const double green_limit =
          std::max(2.0, 0.10 * static_cast<double>(threshold.delta_adc));
      const double amber_limit =
          std::max(4.0, 0.25 * static_cast<double>(threshold.delta_adc));
      const bool direction_valid =
          metadata.hardware.polarity == "falling"
              ? tail_median > static_cast<double>(threshold.readback_adc)
              : tail_median < static_cast<double>(threshold.readback_adc);
      const std::string status =
          !direction_valid || error_adc > amber_limit
              ? "FAIL"
              : error_adc > green_limit ? "WARN" : "PASS";
      checks.Add(status == "PASS" && partial_event_scan ? "SKIP" : status,
                 "trigger",
                 "channel_" + std::to_string(channel) +
                     "_effective_threshold",
                 {{"stable_baseline_adc", tail_median},
                  {"readback_adc", threshold.readback_adc},
                  {"actual_delta_adc", actual_delta},
                  {"actual_mv", actual_mv},
                  {"calibration_drift_adc", drift}},
                 {{"requested_delta_adc", threshold.delta_adc},
                  {"requested_mv", threshold.requested_mv},
                  {"green_error_adc", green_limit},
                  {"amber_error_adc", amber_limit}},
                 status == "PASS" && partial_event_scan
                     ? "The effective threshold matches within the scanned prefix, but a full scan is required to rule out later baseline drift."
                     : "Stable event baselines are compared with the absolute discriminator readback to detect post-calibration drift.");
    }

    report["channels"].push_back(
        {{"channel", channel},
         {"active", active},
         {"trigger_enabled",
          threshold.present ? Json(threshold.trigger_enabled) : Json(nullptr)},
         {"threshold", std::move(threshold_json)},
         {"metrics", std::move(metrics)}});
  }

  if (routing_evaluable != 0U) {
    const double fraction = static_cast<double>(routing_passed) /
                            static_cast<double>(routing_evaluable);
    const std::string status =
        fraction < 0.95 ? "FAIL" : fraction < 0.99 ? "WARN" : "PASS";
    checks.Add(status == "PASS" && partial_event_scan ? "SKIP" : status,
               "trigger", "routing_event_evidence",
               {{"passed", routing_passed},
                {"evaluated", routing_evaluable},
                {"fraction", fraction}},
               {{"green_fraction", 0.99}, {"red_fraction", 0.95}},
               status == "PASS" && partial_event_scan
                   ? "The routing condition holds in the scanned prefix, but the remainder of the file was not evaluated."
               : has_waveforms
                   ? "Event-level discriminator crossings were evaluated from archived waveforms."
                   : "Event-level falling-edge crossings were derived statistically from Baseline-PulseHeight.");
    report["summary"]["routing_pass_fraction"] = fraction;
    report["summary"]["routing_evidence_mode"] =
        has_waveforms && metadata.hardware.pair_logic == "AND"
            ? "simultaneous_waveform_comparator_overlap"
            : has_waveforms ? "waveform_comparator_crossing"
                            : "derived_from_scalar_pulse_extrema";
  } else if (metadata.hardware.valid &&
             metadata.hardware.self_mode != 0U) {
    checks.Add("WARN", "trigger", "routing_event_evidence", "unavailable",
               "falling polarity or archived waveforms, no external trigger",
               metadata.hardware.external_mode != 0U
                   ? "External triggers make self-trigger evidence non-exclusive."
                   : metadata.hardware.pair_logic == "AND" && !has_waveforms
                         ? "Scalar PulseHeight cannot prove simultaneous comparator overlap; archive waveforms to validate hardware AND from event data."
                         : "Rising-edge trigger evidence requires archived waveforms because PulseHeight stores only falling minima.");
  }

  if (!metadata.present && !legacy_pair_extrema.empty()) {
    if (partial_event_scan) {
      checks.Add("SKIP", "trigger", "legacy_trigger_inference",
                 {{"events_scanned", scanned},
                  {"events_total", total_entries}},
                 "complete event scan",
                 "A prefix tail can differ substantially from the stable file tail, so no file-level legacy cutoff or relative threshold is inferred from a partial scan.");
    } else {
      const std::uint64_t analysis_start = std::max(
          channel_settling_events[0].value_or(0U),
          channel_settling_events[1].value_or(0U));
      double inferred_cutoff = -std::numeric_limits<double>::infinity();
      std::uint64_t analyzed_extrema = 0U;
      for (const LegacyPairExtremum& extrema : legacy_pair_extrema) {
        if (extrema.entry < analysis_start) continue;
        inferred_cutoff =
            std::max(inferred_cutoff, std::min(extrema.low, extrema.high));
        ++analyzed_extrema;
      }
      if (analyzed_extrema == 0U) {
        checks.Add("SKIP", "trigger", "legacy_trigger_inference",
                   "no post-settling events", ">0 post-settling events",
                   "No event remains after the inferred baseline settling point.");
      } else {
        std::uint64_t inferred_and = 0U;
        for (const LegacyPairExtremum& extrema : legacy_pair_extrema) {
          if (extrema.entry < analysis_start) continue;
          if (std::max(extrema.low, extrema.high) <= inferred_cutoff) {
            ++inferred_and;
          }
        }
        const double and_fraction =
            static_cast<double>(inferred_and) /
            static_cast<double>(analyzed_extrema);
        Json inferred_channels = Json::array();
        constexpr double kAssumedLegacyLsbMv = 2000.0 / 16384.0;
        for (int channel = 0; channel < 2; ++channel) {
          Json& metrics = report["channels"].at(channel).at("metrics");
          const Json& tail = metrics.at("baseline_tail_median_adc");
          if (!tail.is_number()) continue;
          const double stable_baseline = tail.get<double>();
          const double relative_adc = stable_baseline - inferred_cutoff;
          const double relative_mv_2v = relative_adc * kAssumedLegacyLsbMv;
          metrics["legacy_inferred_absolute_cutoff_adc"] = inferred_cutoff;
          metrics["legacy_relative_cutoff_adc"] = relative_adc;
          metrics["legacy_relative_cutoff_mv_assuming_2vpp"] = relative_mv_2v;
          inferred_channels.push_back(
              {{"channel", channel},
               {"stable_baseline_adc", stable_baseline},
               {"relative_cutoff_adc", relative_adc},
               {"relative_cutoff_mv_assuming_2vpp", relative_mv_2v}});
        }
        report["summary"]["legacy_trigger_inference"] =
            {{"pair", "CH0/CH1"},
             {"analysis_start_event", analysis_start},
             {"inferred_or_cutoff_adc", inferred_cutoff},
             {"or_pass_fraction", 1.0},
             {"and_pass_fraction_at_same_cutoff", and_fraction},
             {"sampled_events", analyzed_extrema},
             {"record_mask",
              observed_masks.size() == 1U ? Json(*observed_masks.begin())
                                          : Json(nullptr)},
             {"other_recorded_channels_assumed_record_only",
              observed_masks.size() == 1U &&
                  ((*observed_masks.begin() & ~0x3U) != 0U)},
             {"relative_threshold_channels", inferred_channels},
             {"mv_conversion_assumption",
              {{"input_range_mvpp", 2000}, {"adc_bits", 14}}}};
        checks.Add("WARN", "trigger", "legacy_trigger_inference",
                   report["summary"]["legacy_trigger_inference"],
                   "authenticated RunMetadata threshold and routing",
                   "This is a data-derived falling-edge inference. ADC deltas "
                   "are observed; mV values explicitly assume 2 Vpp/14-bit "
                   "and any additional recorded channels are assumed record-only "
                   "because legacy provenance is absent.");
      }
    }
  }

  Progress(options, 97.0, "checking final file identity");
  const FileIdentity pre_final_hash_identity =
      DescriptorIdentity(pinned_input.get());
  std::optional<std::string> final_sha256;
  if (hash_full_contents && !scan_cancelled) {
    final_sha256 = Sha256DescriptorCancellable(
        pinned_input.get(), initial_identity.size, options);
    if (!final_sha256) {
      scan_cancelled = true;
      report["analysis"]["completed"] = false;
      report["analysis"]["cancelled"] = true;
      checks.Add("WARN", "operation", "cancelled_during_final_hash",
                 "cancelled", "completed final SHA-256",
                 "Validation was cancelled while checking the final input digest; partial metrics are retained.");
    }
  }
  const FileIdentity final_identity = DescriptorIdentity(pinned_input.get());
  const FileIdentity final_path_identity = StatRegularFile(absolute_path);
  const bool identity_unchanged =
      SameIdentity(initial_identity, final_identity) &&
      SameIdentity(initial_identity, final_path_identity);
  const bool stable_while_hashing =
      SameIdentity(pre_final_hash_identity, final_identity);
  checks.Add(identity_unchanged && stable_while_hashing ? "PASS" : "FAIL",
             "integrity",
             "file_identity_stable", IdentityJson(final_identity),
             IdentityJson(initial_identity),
             identity_unchanged && stable_while_hashing
                 ? "Input device/inode/size/mtime remained unchanged during validation."
                 : "Input identity changed while it was being validated.");
  if (initial_sha256 && final_sha256) {
    const bool sha256_unchanged = *final_sha256 == *initial_sha256;
    checks.Add(sha256_unchanged ? "PASS" : "FAIL", "integrity",
               "file_sha256_stable", *final_sha256, *initial_sha256,
               sha256_unchanged
                   ? "Input bytes match the SHA-256 captured before validation."
                   : "Input bytes changed while validation was in progress.");
  } else {
    checks.Add("SKIP", "integrity", "file_sha256_stable", nullptr,
               "full validation start/end SHA-256",
               hash_full_contents
                   ? "Final SHA-256 comparison was skipped because validation was cancelled."
                   : "Prefix validation deliberately skips both full-file SHA-256 passes so its runtime scales with the requested event prefix.");
  }
  report["input"]["identity_end"] = IdentityJson(final_identity);
  report["input"]["sha256_end"] =
      final_sha256 ? Json(*final_sha256) : Json(nullptr);

  file->Close();
  report["domain_status"] = {
      {"data_integrity",
       DomainStatus(report["checks"],
                    {"integrity", "schema", "physics_sanity", "summary",
                     "operation"})},
      {"provenance", DomainStatus(report["checks"], {"provenance"})},
      {"trigger_and_quality",
       DomainStatus(report["checks"],
                    {"trigger", "data_quality", "operation"})}};
  report["counts"] = {{"pass", checks.passes},
                      {"warn", checks.warnings},
                      {"fail", checks.failures},
                      {"skip", checks.skips}};
  report["summary"]["check_counts"] = report["counts"];
  if (scan_cancelled) {
    report["overall_status"] = "CANCELLED";
  } else if (checks.failures != 0U) {
    report["overall_status"] = "FAIL";
  } else if (checks.warnings != 0U) {
    report["overall_status"] = "WARN";
  } else {
    report["overall_status"] = "PASS";
  }
  Progress(options, 100.0, "complete");
  return report;
}

}  // namespace cpnr
