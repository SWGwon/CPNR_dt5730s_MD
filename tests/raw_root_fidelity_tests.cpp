#include "EventHeader.h"
#include "RawRootFidelity.h"
#include "Sha256.h"
#include "WaveformDsp.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;

class TemporaryDirectory {
 public:
  TemporaryDirectory() {
    std::array<char, 64> pattern{};
    const std::string prefix = "/tmp/cpnr_raw_fidelity_test.XXXXXX";
    std::copy(prefix.begin(), prefix.end(), pattern.begin());
    char* created = ::mkdtemp(pattern.data());
    if (created == nullptr) {
      throw std::runtime_error("mkdtemp failed: " +
                               std::string(std::strerror(errno)));
    }
    path_ = created;
  }
  ~TemporaryDirectory() {
    std::error_code ignored;
    fs::remove_all(path_, ignored);
  }
  const fs::path& path() const { return path_; }

 private:
  fs::path path_;
};

struct Fixture {
  EventHeader header{};
  std::vector<std::uint16_t> waveform;
};

Fixture ValidFixture() {
  Fixture fixture;
  fixture.header.ExtendedTTT = 1000U;
  fixture.header.EventID = 0U;
  fixture.header.RecordLength = 130U;
  fixture.header.ChannelMask = 1U;
  fixture.header.Pattern = 7U;
  fixture.header.BoardEventCounter = 55U;
  fixture.waveform.assign(fixture.header.RecordLength, 1000U);
  for (std::size_t sample = 64U; sample < 72U; ++sample) {
    fixture.waveform[sample] = 960U;
  }
  return fixture;
}

void WriteFixture(const fs::path& path, const Fixture& fixture) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) throw std::runtime_error("Cannot create test RAW");
  output.write(reinterpret_cast<const char*>(&fixture.header),
               sizeof(fixture.header));
  output.write(reinterpret_cast<const char*>(fixture.waveform.data()),
               static_cast<std::streamsize>(fixture.waveform.size() *
                                            sizeof(std::uint16_t)));
  output.close();
  if (!output) throw std::runtime_error("Cannot finish test RAW");
}

cpnr::WaveformDspSettings DspSettings(std::uint32_t schema) {
  cpnr::WaveformDspSettings settings;
  settings.baseline_samples = 32U;
  settings.short_gate_samples = 2U;
  settings.long_gate_samples = 4U;
  settings.pulse_start_threshold_adc = 30.0;
  settings.integrate_from_pulse_start = true;
  settings.legacy_adaptive_baseline = false;
  settings.preserve_signed_charge =
      schema != cpnr::kWaveformDspSchemaClampedCharge;
  settings.integration_mode =
      schema == cpnr::kWaveformDspSchemaPeakCenteredCharge
          ? cpnr::WaveformDspIntegrationMode::kPeakCenteredWindow
          : cpnr::WaveformDspIntegrationMode::kThresholdAnchoredGates;
  return settings;
}

cpnr::RawRootFidelitySettings Settings(
    const fs::path& path,
    std::uint32_t schema = cpnr::kWaveformDspCurrentSchema) {
  cpnr::RawRootFidelitySettings settings;
  settings.resolved_raw_path = fs::absolute(path).string();
  settings.expected_size_bytes = fs::file_size(path);
  settings.expected_sha256 = Sha256FileHex(path.string());
  settings.expected_events = 1U;
  settings.expected_record_length = 130U;
  settings.expected_channel_mask = 1U;
  settings.falling_polarity = true;
  settings.waveform_dsp = DspSettings(schema);
  settings.compare_short_charge = true;
  return settings;
}

cpnr::RawRootEventView View(const Fixture& fixture,
                            bool include_waveform = true,
                            std::uint32_t schema =
                                cpnr::kWaveformDspCurrentSchema) {
  cpnr::RawRootEventView view;
  view.entry = 0U;
  view.sync_time_ttt = fixture.header.ExtendedTTT;
  view.event_id = fixture.header.EventID;
  view.record_length = fixture.header.RecordLength;
  view.channel_mask = fixture.header.ChannelMask;
  view.pattern = fixture.header.Pattern;
  view.board_event_counter = fixture.header.BoardEventCounter;
  view.pulse_start_ns.fill(-1.0);
  const cpnr::WaveformDspValues dsp = cpnr::ComputeWaveformDsp(
      fixture.waveform.data(), fixture.waveform.size(), true,
      DspSettings(schema));
  view.baseline[0] = dsp.baseline;
  view.short_charge[0] = dsp.short_charge;
  view.charge[0] = dsp.charge;
  view.pulse_height[0] = dsp.pulse_height;
  view.pulse_start_ns[0] = dsp.pulse_start_ns;
  view.waveforms_saved = include_waveform;
  if (include_waveform) view.waveforms[0] = &fixture.waveform;
  return view;
}

void Expect(bool condition, const std::string& detail);

void TestWaveformDspSchemaDispatch(const fs::path& directory) {
  Fixture fixture = ValidFixture();
  fixture.waveform.assign(fixture.header.RecordLength, 1000U);
  fixture.waveform[64U] = 960U;  // First legacy threshold crossing: +40 ADC.
  fixture.waveform[65U] = 1020U;
  fixture.waveform[66U] = 1020U;
  fixture.waveform[67U] = 1020U;
  fixture.waveform[75U] = 950U;  // Largest directed peak: +50 ADC.
  const fs::path raw = directory / "dsp-schema-dispatch.dat";
  WriteFixture(raw, fixture);

  const cpnr::RawRootEventView schema1 = View(
      fixture, false, cpnr::kWaveformDspSchemaClampedCharge);
  Expect(schema1.short_charge[0] == 20.0,
         "Schema 1 did not use the threshold-anchored ShortGate");
  Expect(schema1.charge[0] == 0.0,
         "Schema 1 did not clamp the negative threshold-gate integral");
  Expect(schema1.pulse_start_ns[0] == 128.0,
         "Schema 1 T0 did not retain threshold-crossing semantics");

  const cpnr::RawRootEventView schema2 = View(
      fixture, false, cpnr::kWaveformDspSchemaSignedCharge);
  Expect(schema2.short_charge[0] == 20.0,
         "Schema 2 did not use the threshold-anchored ShortGate");
  Expect(schema2.charge[0] == -20.0,
         "Schema 2 did not preserve the signed threshold-gate integral");
  Expect(schema2.pulse_start_ns[0] == 128.0,
         "Schema 2 T0 did not retain threshold-crossing semantics");

  const cpnr::RawRootEventView schema3 = View(
      fixture, false, cpnr::kWaveformDspSchemaPeakCenteredCharge);
  Expect(schema3.short_charge[0] == -10.0 &&
             schema3.charge[0] == -10.0,
         "Schema 3 did not use the signed peak-centered window/alias");
  Expect(schema3.pulse_height[0] == 50.0 &&
             schema3.pulse_start_ns[0] == 150.0,
         "Schema 3 did not anchor height and T0 at the largest peak");

  for (const std::uint32_t schema : {
           cpnr::kWaveformDspSchemaClampedCharge,
           cpnr::kWaveformDspSchemaSignedCharge,
           cpnr::kWaveformDspSchemaPeakCenteredCharge}) {
    cpnr::RawRootFidelityVerifier verifier(Settings(raw, schema));
    verifier.CompareEvent(View(fixture, true, schema));
    Expect(verifier.Finish().ExactMatch(),
           "RAW fidelity rejected matching waveform DSP schema " +
               std::to_string(schema));
  }

  cpnr::RawRootFidelityVerifier mismatched_verifier(
      Settings(raw, cpnr::kWaveformDspSchemaPeakCenteredCharge));
  mismatched_verifier.CompareEvent(
      View(fixture, false, cpnr::kWaveformDspSchemaSignedCharge));
  Expect(!mismatched_verifier.Finish().ExactMatch(),
         "RAW fidelity accepted schema-2 scalars under schema-3 settings");
}

void Expect(bool condition, const std::string& detail) {
  if (!condition) throw std::runtime_error(detail);
}

template <typename Function>
void ExpectFailure(Function function, const std::string& detail) {
  try {
    function();
  } catch (const std::exception&) {
    return;
  }
  throw std::runtime_error("Expected failure: " + detail);
}

void TestExactWaveformAndScalarModes(const fs::path& directory) {
  const Fixture fixture = ValidFixture();
  const fs::path raw = directory / "valid.dat";
  WriteFixture(raw, fixture);

  cpnr::RawRootFidelityVerifier waveform_verifier(Settings(raw));
  waveform_verifier.CompareEvent(View(fixture, true));
  const cpnr::RawRootFidelityResult waveform_result =
      waveform_verifier.Finish();
  Expect(waveform_result.ExactMatch(), "Exact waveform fixture mismatched");
  Expect(waveform_result.compared_bytes_sha256 ==
             waveform_result.authenticated_sha256,
         "Comparison pass bytes were not bound to the authenticated digest");
  Expect(waveform_result.waveform_samples_compared == 130U,
         "Waveform comparison did not cover every sample");

  cpnr::RawRootFidelityVerifier scalar_verifier(Settings(raw));
  scalar_verifier.CompareEvent(View(fixture, false));
  const cpnr::RawRootFidelityResult scalar_result = scalar_verifier.Finish();
  Expect(scalar_result.ExactMatch(), "Exact scalar fixture mismatched");
  Expect(scalar_result.compared_bytes_sha256 ==
             scalar_result.authenticated_sha256,
         "Scalar comparison pass bytes were not digest-bound");
  Expect(!scalar_result.waveforms_compared,
         "Scalar-only fixture unexpectedly claimed waveform comparison");
}

void TestLegacyEightSampleGranularity(const fs::path& directory) {
  Fixture fixture = ValidFixture();
  fixture.header.RecordLength = 128U;
  fixture.waveform.resize(fixture.header.RecordLength);
  const fs::path raw = directory / "legacy-granularity-8.dat";
  WriteFixture(raw, fixture);

  cpnr::RawRootFidelitySettings settings = Settings(raw);
  settings.expected_record_length = fixture.header.RecordLength;
  settings.expected_record_length_granularity = 8U;
  cpnr::RawRootFidelityVerifier verifier(settings);
  verifier.CompareEvent(View(fixture, true));
  const cpnr::RawRootFidelityResult result = verifier.Finish();
  Expect(result.ExactMatch(),
         "Authenticated legacy 8-sample fixture mismatched");
  Expect(result.waveform_samples_compared == fixture.header.RecordLength,
         "Legacy comparison did not cover every waveform sample");
}

void TestContentMismatchAccounting(const fs::path& directory) {
  const Fixture fixture = ValidFixture();
  const fs::path raw = directory / "mismatch.dat";
  WriteFixture(raw, fixture);

  Fixture altered_waveform = fixture;
  altered_waveform.waveform[70] = 959U;
  cpnr::RawRootEventView waveform_view = View(fixture, true);
  waveform_view.waveforms[0] = &altered_waveform.waveform;
  cpnr::RawRootFidelityVerifier waveform_verifier(Settings(raw));
  waveform_verifier.CompareEvent(waveform_view);
  const cpnr::RawRootFidelityResult waveform_result =
      waveform_verifier.Finish();
  Expect(!waveform_result.ExactMatch(),
         "Changed ROOT waveform was accepted as exact");
  Expect(waveform_result.TotalWaveformSampleMismatches() == 1U,
         "Changed ROOT waveform sample was not counted exactly");

  cpnr::RawRootEventView scalar_view = View(fixture, false);
  scalar_view.charge[0] += 1.0;
  cpnr::RawRootFidelityVerifier scalar_verifier(Settings(raw));
  scalar_verifier.CompareEvent(scalar_view);
  const cpnr::RawRootFidelityResult scalar_result = scalar_verifier.Finish();
  Expect(!scalar_result.ExactMatch(),
         "Changed ROOT scalar was accepted as exact");
  Expect(scalar_result.TotalScalarFieldMismatches() == 1U,
         "Changed ROOT scalar field was not counted exactly");
}

void TestMalformedHeaderFailsClosed(const fs::path& directory) {
  Fixture fixture = ValidFixture();
  fixture.header.RecordLength = 120U;
  // Preserve the expected 284-byte event size so authentication succeeds and
  // the malformed EventHeader is rejected by the streaming parser itself.
  const fs::path raw = directory / "malformed.dat";
  WriteFixture(raw, fixture);
  cpnr::RawRootFidelityVerifier verifier(Settings(raw));
  ExpectFailure([&]() { verifier.CompareEvent(View(ValidFixture(), true)); },
                "metadata-inconsistent EventHeader");
}

void TestInPlaceChangeFailsIdentityCheck(const fs::path& directory) {
  const Fixture fixture = ValidFixture();
  const fs::path raw = directory / "changed.dat";
  WriteFixture(raw, fixture);
  cpnr::RawRootFidelityVerifier verifier(Settings(raw));

  const int descriptor = ::open(raw.c_str(), O_WRONLY | O_CLOEXEC);
  if (descriptor < 0) throw std::runtime_error("Cannot reopen test RAW");
  const std::uint16_t changed_sample = 959U;
  const off_t offset = static_cast<off_t>(sizeof(EventHeader) + 70U * 2U);
  const ssize_t written =
      ::pwrite(descriptor, &changed_sample, sizeof(changed_sample), offset);
  if (written != static_cast<ssize_t>(sizeof(changed_sample)) ||
      ::fsync(descriptor) != 0) {
    ::close(descriptor);
    throw std::runtime_error("Cannot mutate isolated test RAW");
  }
  ::close(descriptor);

  verifier.CompareEvent(View(fixture, true));
  ExpectFailure([&]() { static_cast<void>(verifier.Finish()); },
                "in-place RAW mutation");
}

void TestPathReplacementFailsIdentityCheck(const fs::path& directory) {
  const Fixture fixture = ValidFixture();
  const fs::path raw = directory / "replaced.dat";
  const fs::path moved = directory / "original.dat";
  WriteFixture(raw, fixture);
  cpnr::RawRootFidelityVerifier verifier(Settings(raw));

  fs::rename(raw, moved);
  WriteFixture(raw, fixture);
  verifier.CompareEvent(View(fixture, true));
  ExpectFailure([&]() { static_cast<void>(verifier.Finish()); },
                "resolved RAW path replacement");
}

void TestCancellationDuringAuthentication(const fs::path& directory) {
  const Fixture fixture = ValidFixture();
  const fs::path raw = directory / "cancel.dat";
  WriteFixture(raw, fixture);
  cpnr::RawRootFidelitySettings settings = Settings(raw);
  settings.cancelled = []() { return true; };
  bool observed_cancel = false;
  try {
    cpnr::RawRootFidelityVerifier verifier(settings);
  } catch (const cpnr::RawRootFidelityCancelled&) {
    observed_cancel = true;
  }
  Expect(observed_cancel, "Cancellation did not interrupt RAW authentication");
}

}  // namespace

int main() {
  try {
    TemporaryDirectory temporary;
    TestExactWaveformAndScalarModes(temporary.path());
    TestWaveformDspSchemaDispatch(temporary.path());
    TestLegacyEightSampleGranularity(temporary.path());
    TestContentMismatchAccounting(temporary.path());
    TestMalformedHeaderFailsClosed(temporary.path());
    TestInPlaceChangeFailsIdentityCheck(temporary.path());
    TestPathReplacementFailsIdentityCheck(temporary.path());
    TestCancellationDuringAuthentication(temporary.path());
    std::cout << "RAW-to-ROOT fidelity regression tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "RAW-to-ROOT fidelity regression failure: " << error.what()
              << '\n';
    return 1;
  }
}
