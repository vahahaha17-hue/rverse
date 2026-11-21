/// @file test_riser_shaping.cpp
/// @brief Unit tests for Tension LFO, last-bar gate, post tail, loudness and bar-length helpers.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "Presets.h"
#include "RiserShaping.h"

#include <cmath>
#include <vector>

using Catch::Approx;
using namespace rvrse;

namespace {

constexpr double kSr = 48000.0;
constexpr double kBpm = 120.0;                // 24000 frames per beat at 48 kHz
constexpr int kFramesPerBeat = 24000;

std::vector<float> ones(size_t n) { return std::vector<float>(n, 1.0f); }

double rmsRange(const std::vector<float>& v, size_t from, size_t to)
{
  double sum = 0.0;
  for (size_t i = from; i < to; ++i)
    sum += static_cast<double>(v[i]) * v[i];
  return std::sqrt(sum / static_cast<double>(to - from));
}

} // namespace

TEST_CASE("Bars: time signature to beats per bar", "[shaping][bars]")
{
  REQUIRE(BeatsPerBarFromTimeSig(4, 4) == Approx(4.0));
  REQUIRE(BeatsPerBarFromTimeSig(3, 4) == Approx(3.0));
  REQUIRE(BeatsPerBarFromTimeSig(6, 8) == Approx(3.0));
  REQUIRE(BeatsPerBarFromTimeSig(7, 8) == Approx(3.5));
  REQUIRE(BeatsPerBarFromTimeSig(0, 4) == Approx(4.0));
  REQUIRE(BeatsPerBarFromTimeSig(4, 0) == Approx(4.0));
}

TEST_CASE("Bars: Riser Length resolves beats and bars", "[shaping][bars]")
{
  // Existing beat entries are untouched.
  REQUIRE(ResolveRiserLengthBeats(kRiserLen_4, 3.0) == Approx(4.0));
  REQUIRE(ResolveRiserLengthBeats(kRiserLen_1_4, 3.0) == Approx(0.25));

  // Bar entries follow the host time signature.
  REQUIRE(ResolveRiserLengthBeats(kRiserLengthBarsFirstIdx + 0, 4.0) == Approx(4.0));
  REQUIRE(ResolveRiserLengthBeats(kRiserLengthBarsFirstIdx + 1, 4.0) == Approx(8.0));
  REQUIRE(ResolveRiserLengthBeats(kRiserLengthBarsFirstIdx + 2, 4.0) == Approx(16.0));
  REQUIRE(ResolveRiserLengthBeats(kRiserLengthBarsFirstIdx + 1, 3.0) == Approx(6.0));

  // Out-of-range indices clamp instead of reading past the tables.
  REQUIRE(ResolveRiserLengthBeats(-5, 4.0) == Approx(0.25));
  REQUIRE(ResolveRiserLengthBeats(999, 4.0) == Approx(16.0));
  REQUIRE(kNumRiserLengthEntries == 10);
}

TEST_CASE("Tension: LFO gain is full on the grid and dips between", "[shaping][tension]")
{
  const double period = 24000.0;
  REQUIRE(TensionLfoGain(0.0, period, 0.5) == Approx(1.0));
  REQUIRE(TensionLfoGain(-period, period, 0.5) == Approx(1.0));
  REQUIRE(TensionLfoGain(-period / 2.0, period, 0.5) == Approx(0.5));
  REQUIRE(TensionLfoGain(-period / 2.0, period, 1.0) == Approx(0.0).margin(1e-9));
  REQUIRE(TensionLfoGain(123.0, 0.0, 0.7) == Approx(1.0)); // degenerate period → no effect
}

TEST_CASE("Tension: volume LFO pulses around the drop", "[shaping][tension]")
{
  const int anchor = 4 * kFramesPerBeat;
  auto l = ones(static_cast<size_t>(anchor) + 1000);
  auto r = l;

  ApplyTensionLfo(l, r, anchor, kBpm, kSr, kTensionVolume, 0 /* 1/4 */, 0.5f);

  REQUIRE(l[static_cast<size_t>(anchor)] == Approx(1.0f).margin(1e-4));
  REQUIRE(l[static_cast<size_t>(anchor - kFramesPerBeat)] == Approx(1.0f).margin(1e-4));
  REQUIRE(l[static_cast<size_t>(anchor - kFramesPerBeat / 2)] == Approx(0.5f).margin(1e-3));
  REQUIRE(r == l);
}

TEST_CASE("Tension: off or zero depth leaves audio untouched", "[shaping][tension]")
{
  const int anchor = 2 * kFramesPerBeat;
  const auto original = ones(static_cast<size_t>(anchor));

  auto l = original;
  auto r = original;
  ApplyTensionLfo(l, r, anchor, kBpm, kSr, kTensionOff, 1, 0.8f);
  REQUIRE(l == original);

  ApplyTensionLfo(l, r, anchor, kBpm, kSr, kTensionVolume, 1, 0.0f);
  REQUIRE(l == original);

  ApplyTensionLfo(l, r, anchor, 0.0, kSr, kTensionVolume, 1, 0.8f); // invalid tempo
  REQUIRE(l == original);
}

TEST_CASE("Tension: filter LFO closes the high end between grid points", "[shaping][tension]")
{
  const int anchor = 2 * kFramesPerBeat;
  const size_t n = static_cast<size_t>(anchor) + 2000;

  // Nyquist-rate signal: only the filter's high-frequency response matters.
  std::vector<float> l(n);
  for (size_t i = 0; i < n; ++i)
    l[i] = (i % 2 == 0) ? 1.0f : -1.0f;
  auto r = l;

  ApplyTensionLfo(l, r, anchor, kBpm, kSr, kTensionFilter, 0 /* 1/4 */, 1.0f);

  const double onGrid = rmsRange(l, static_cast<size_t>(anchor) - 100, static_cast<size_t>(anchor) + 100);
  const size_t mid = static_cast<size_t>(anchor - kFramesPerBeat / 2);
  const double between = rmsRange(l, mid - 100, mid + 100);

  REQUIRE(onGrid > between * 10.0);
  REQUIRE(onGrid > 0.5);
}

TEST_CASE("Gate: chops only the region before the drop", "[shaping][gate]")
{
  const int anchor = 4 * kFramesPerBeat;
  auto l = ones(static_cast<size_t>(anchor) + 5000);
  auto r = l;

  ApplyLastBarGate(l, r, anchor, kBpm, kSr, kGateLastBeat, 1 /* 1/16 */, 4.0);

  const size_t regionStart = static_cast<size_t>(anchor - kFramesPerBeat);

  // Before the region and after the drop: untouched.
  REQUIRE(l[regionStart - 1] == Approx(1.0f));
  REQUIRE(l[0] == Approx(1.0f));
  REQUIRE(l[static_cast<size_t>(anchor)] == Approx(1.0f));
  REQUIRE(l.back() == Approx(1.0f));

  // 1/16 step = 6000 frames: open for the first half, closed for the second half.
  REQUIRE(l[regionStart + 1500] == Approx(1.0f));
  REQUIRE(l[regionStart + 4500] == Approx(0.0f).margin(1e-6));
  REQUIRE(l[regionStart + 6000 + 1500] == Approx(1.0f));
  REQUIRE(l[regionStart + 6000 + 4500] == Approx(0.0f).margin(1e-6));
  REQUIRE(r == l);
}

TEST_CASE("Gate: step edges are ramped, not hard clicks", "[shaping][gate]")
{
  const int anchor = 4 * kFramesPerBeat;
  auto l = ones(static_cast<size_t>(anchor));
  auto r = l;
  ApplyLastBarGate(l, r, anchor, kBpm, kSr, kGateLastBeat, 1, 4.0);

  const size_t regionStart = static_cast<size_t>(anchor - kFramesPerBeat);
  float maxStep = 0.0f;
  for (size_t i = regionStart + 1; i < static_cast<size_t>(anchor); ++i)
    maxStep = std::max(maxStep, std::abs(l[i] - l[i - 1]));

  // 2 ms ramp at 48 kHz = 96 frames → at most ~1/96 per sample.
  REQUIRE(maxStep < 0.05f);
}

TEST_CASE("Gate: region length follows beats per bar", "[shaping][gate]")
{
  const int anchor = 8 * kFramesPerBeat;

  auto checkRegionStart = [&](EGateRegion region, double beatsPerBar, double expectedBeats) {
    auto l = ones(static_cast<size_t>(anchor));
    auto r = l;
    ApplyLastBarGate(l, r, anchor, kBpm, kSr, region, 0 /* 1/8 */, beatsPerBar);

    const int expectedStart = anchor - static_cast<int>(expectedBeats * kFramesPerBeat);
    // 1/8 step = 12000 frames, closed half starts 6000 frames in.
    REQUIRE(l[static_cast<size_t>(expectedStart - 1)] == Approx(1.0f));
    REQUIRE(l[static_cast<size_t>(expectedStart + 9000)] == Approx(0.0f).margin(1e-6));
  };

  checkRegionStart(kGateLastBeat, 4.0, 1.0);
  checkRegionStart(kGateLastTwoBeats, 4.0, 2.0);
  checkRegionStart(kGateLastBar, 4.0, 4.0);
  checkRegionStart(kGateLastBar, 3.0, 3.0);   // 3/4 bar
}

TEST_CASE("Gate: off and degenerate input are no-ops", "[shaping][gate]")
{
  const int anchor = 2 * kFramesPerBeat;
  const auto original = ones(static_cast<size_t>(anchor));

  auto l = original;
  auto r = original;
  ApplyLastBarGate(l, r, anchor, kBpm, kSr, kGateOff, 1, 4.0);
  REQUIRE(l == original);

  ApplyLastBarGate(l, r, 0, kBpm, kSr, kGateLastBar, 1, 4.0);
  REQUIRE(l == original);

  ApplyLastBarGate(l, r, anchor, 0.0, kSr, kGateLastBar, 1, 4.0);
  REQUIRE(l == original);
}

TEST_CASE("Gate: region longer than the riser is clamped safely", "[shaping][gate]")
{
  const int anchor = kFramesPerBeat / 2; // shorter than the one-beat region
  auto l = ones(static_cast<size_t>(anchor));
  auto r = l;
  ApplyLastBarGate(l, r, anchor, kBpm, kSr, kGateLastBeat, 1, 4.0);
  REQUIRE(l.size() == static_cast<size_t>(anchor));
}

TEST_CASE("Post tail: mixes at the hit start and grows the buffer", "[shaping][tail]")
{
  // Tail is longer than the 150 ms fade-out (7200 frames at 48 kHz), so it has a steady part.
  std::vector<float> l(1000, 0.0f);
  std::vector<float> r(1000, 0.0f);
  const std::vector<float> tailL(20000, 0.5f);
  const std::vector<float> tailR(20000, -0.5f);

  MixPostTail(l, r, tailL, tailR, 800, kSr);

  REQUIRE(l.size() == 20800);
  REQUIRE(r.size() == 20800);
  REQUIRE(l[799] == Approx(0.0f));
  REQUIRE(l[800] == Approx(0.5f));
  REQUIRE(r[800] == Approx(-0.5f));
  REQUIRE(l[5800] == Approx(0.5f));          // steady part of the tail
  REQUIRE(std::abs(l.back()) < 0.01f);       // faded out at the very end
}

TEST_CASE("Post tail: adds onto existing riser audio", "[shaping][tail]")
{
  std::vector<float> l(2000, 0.25f);
  std::vector<float> r(2000, 0.25f);
  const std::vector<float> tail(500, 0.5f);

  MixPostTail(l, r, tail, tail, 100, kSr);

  REQUIRE(l.size() == 2000);                 // fits, so no growth
  REQUIRE(l[50] == Approx(0.25f));
  REQUIRE(l[150] > 0.25f);
}

TEST_CASE("Post tail: empty or mismatched tails do nothing", "[shaping][tail]")
{
  std::vector<float> l(100, 0.1f);
  std::vector<float> r(100, 0.1f);
  MixPostTail(l, r, {}, {}, 10, kSr);
  REQUIRE(l.size() == 100);

  MixPostTail(l, r, std::vector<float>(10, 1.0f), std::vector<float>(5, 1.0f), 10, kSr);
  REQUIRE(l.size() == 100);
  REQUIRE(l[10] == Approx(0.1f));
}

TEST_CASE("Loudness: gain brings the peak to the target", "[shaping][loudness]")
{
  const float target = std::pow(10.0f, kLoudnessTargetPeakDb / 20.0f);

  REQUIRE(ComputeLoudnessGain(0.5f) == Approx(target / 0.5f));
  REQUIRE(ComputeLoudnessGain(2.0f) == Approx(target / 2.0f));

  std::vector<float> l = { 0.1f, -0.4f, 0.2f };
  std::vector<float> r = { 0.0f, 0.3f, -0.1f };
  const float peak = ComputePeak(l, r);
  REQUIRE(peak == Approx(0.4f));

  ApplyGain(l, r, ComputeLoudnessGain(peak));
  REQUIRE(ComputePeak(l, r) == Approx(target));
}

TEST_CASE("Loudness: silence is never amplified and gain is bounded", "[shaping][loudness]")
{
  REQUIRE(ComputeLoudnessGain(0.0f) == 1.0f);
  REQUIRE(ComputeLoudnessGain(1e-9f) == 1.0f);

  const float maxGain = std::pow(10.0f, kLoudnessMaxGainDb / 20.0f);
  const float minGain = std::pow(10.0f, kLoudnessMinGainDb / 20.0f);
  REQUIRE(ComputeLoudnessGain(1e-5f) == Approx(maxGain));   // very quiet → capped boost
  REQUIRE(ComputeLoudnessGain(1000.0f) == Approx(minGain)); // very hot → capped cut
}

TEST_CASE("Presets: table is complete and consistent", "[presets]")
{
  REQUIRE(kNumPresetEntries == 16);
  REQUIRE(std::string(kPresets[0].mName) == "Init");
  REQUIRE(GetPresetDisplayName(0) == "Init");
  REQUIRE(GetPresetDisplayName(1) == "Clean - Room");
  REQUIRE(GetPresetDisplayName(99) == "Init");

  int perCategory[5] = {};
  for (int i = 0; i < kNumPresetEntries; ++i)
  {
    const Preset& p = kPresets[i];
    REQUIRE(p.mLushPct >= 0.0);
    REQUIRE(p.mLushPct <= 100.0);
    REQUIRE(p.mFadeInPct >= 0.0);
    REQUIRE(p.mFadeInPct <= 100.0);
    REQUIRE(p.mTensionDepthPct >= 0.0);
    REQUIRE(p.mTensionDepthPct <= 100.0);
    REQUIRE(p.mLengthIdx >= 0);
    REQUIRE(p.mLengthIdx < kNumRiserLengthEntries);
    REQUIRE(p.mTensionRateIdx >= 0);
    REQUIRE(p.mTensionRateIdx < kNumTensionRates);
    REQUIRE(p.mGateRateIdx >= 0);
    REQUIRE(p.mGateRateIdx < kNumGateRates);
    REQUIRE(p.mCharacter >= 0);
    REQUIRE(p.mCharacter < kNumReverbCharacters);
    ++perCategory[static_cast<int>(p.mCategory)];
  }

  REQUIRE(perCategory[static_cast<int>(EPresetCategory::Init)] == 1);
  REQUIRE(perCategory[static_cast<int>(EPresetCategory::Clean)] == 4);
  REQUIRE(perCategory[static_cast<int>(EPresetCategory::Rhythmic)] == 4);
  REQUIRE(perCategory[static_cast<int>(EPresetCategory::Cinematic)] == 4);
  REQUIRE(perCategory[static_cast<int>(EPresetCategory::Character)] == 3);
}

TEST_CASE("Presets: Init matches the plugin defaults", "[presets]")
{
  const Preset& init = kPresets[0];
  REQUIRE(init.mLushPct == Approx(kLushDefault));
  REQUIRE(init.mLengthIdx == kRiserLengthDefault);
  REQUIRE(init.mFadeInPct == Approx(kFadeInDefault));
  REQUIRE(init.mCharacter == kReverbCharacterDefault);
  REQUIRE(init.mTensionTarget == kTensionTargetDefault);
  REQUIRE(init.mGateRegion == kGateRegionDefault);
  REQUIRE_FALSE(init.mPostTail);
}

TEST_CASE("Presets: display names are unique", "[presets]")
{
  for (int a = 0; a < kNumPresetEntries; ++a)
    for (int b = a + 1; b < kNumPresetEntries; ++b)
      REQUIRE(GetPresetDisplayName(a) != GetPresetDisplayName(b));
}
