/// @file test_pipeline_shaping.cpp
/// @brief Integration tests: characters, shaping and loudness through the full offline pipeline.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "RvrseProcessor.h"
#include "test_helpers.h"

#include <cmath>
#include <memory>
#include <vector>

using Catch::Approx;
using namespace rvrse;

namespace {

constexpr double kSr = 48000.0;
constexpr double kBpm = 120.0;
constexpr double kBeats = 4.0;
constexpr int kAnchorFrames = 4 * 24000; // 4 beats at 120 BPM, 48 kHz

/// Short decaying tone burst, like a drum or vocal hit.
std::shared_ptr<SampleData> makeHit(float amplitude = 0.8f)
{
  auto sample = std::make_shared<SampleData>();
  const size_t frames = static_cast<size_t>(0.25 * kSr);
  sample->mLeft = test::generateSine(frames, 220.0, kSr, amplitude);
  for (size_t i = 0; i < frames; ++i)
    sample->mLeft[i] *= std::exp(-6.0f * static_cast<float>(i) / static_cast<float>(frames));
  sample->mRight = sample->mLeft;
  sample->mSampleRate = kSr;
  sample->mNumChannels = 2;
  return sample;
}

std::shared_ptr<RiserData> run(RvrseProcessor& p, const ShapeSettings& shape,
                               EReverbCharacter character = kReverbMatrix,
                               float amplitude = 0.8f)
{
  return p.RunFullPipelineForTests(makeHit(amplitude), kSr, 0.6f, character, kBpm, kBeats, shape);
}

float peakOf(const RiserData& r) { return ComputePeak(r.mLeft, r.mRight); }

} // namespace

TEST_CASE("Pipeline: every reverb character produces a riser ending on the drop", "[pipeline][shaping]")
{
  for (const EReverbCharacter c : { kReverbMatrix, kReverbRoom, kReverbHall, kReverbPlate })
  {
    RvrseProcessor processor;
    const auto riser = run(processor, ShapeSettings {}, c);

    REQUIRE(riser);
    REQUIRE(riser->IsReady());
    REQUIRE(riser->mBeatAnchorFrames == kAnchorFrames);
    REQUIRE(riser->NumFrames() >= kAnchorFrames);
    REQUIRE(ComputePeak(riser->mLeft, riser->mRight) > 0.0f);

    for (const float v : riser->mLeft)
      REQUIRE(std::isfinite(v));
  }
}

TEST_CASE("Pipeline: default shape leaves the riser unmodified by shaping", "[pipeline][shaping]")
{
  RvrseProcessor a;
  RvrseProcessor b;
  const auto first = run(a, ShapeSettings {});
  const auto second = run(b, ShapeSettings {});

  REQUIRE(first->mLeft == second->mLeft);   // deterministic
  REQUIRE(first->NumFrames() == second->NumFrames());
}

TEST_CASE("Pipeline: shape-only rebuild reuses the cached stretch", "[pipeline][shaping]")
{
  RvrseProcessor processor;
  ShapeSettings shape;
  const auto original = run(processor, shape);
  const std::vector<float> originalLeft = original->mLeft;

  // Same settings through the shape stage must reproduce the same audio exactly.
  const auto reshaped = processor.ReshapeForTests(shape);
  REQUIRE(reshaped->mLeft == originalLeft);
  REQUIRE(reshaped->mBeatAnchorFrames == original->mBeatAnchorFrames);

  // Different settings change the audio but never the timing.
  shape.mGateRegion = kGateLastBar;
  const auto gated = processor.ReshapeForTests(shape);
  REQUIRE(gated->mLeft != originalLeft);
  REQUIRE(gated->NumFrames() == original->NumFrames());
  REQUIRE(gated->mBeatAnchorFrames == original->mBeatAnchorFrames);
}

TEST_CASE("Pipeline: last-bar gate silences the closed windows", "[pipeline][shaping]")
{
  RvrseProcessor processor;
  ShapeSettings shape;
  shape.mGateRegion = kGateLastBeat;
  shape.mGateRateIdx = 1; // 1/16
  const auto riser = run(processor, shape, kReverbRoom);

  const size_t regionStart = static_cast<size_t>(kAnchorFrames - 24000);
  REQUIRE(std::abs(riser->mLeft[regionStart + 4500]) < 1e-6f);
  REQUIRE(std::abs(riser->mLeft[regionStart + 6000 + 4500]) < 1e-6f);

  // The riser builds up, so the open windows near the drop carry audio.
  float openPeak = 0.0f;
  for (size_t i = regionStart + 12000; i < regionStart + 15000; ++i)
    openPeak = std::max(openPeak, std::abs(riser->mLeft[i]));
  REQUIRE(openPeak > 0.0f);
}

TEST_CASE("Pipeline: Tension volume LFO dips between grid points", "[pipeline][shaping]")
{
  RvrseProcessor plain;
  RvrseProcessor modulated;
  ShapeSettings off;
  ShapeSettings tension;
  tension.mTensionTarget = kTensionVolume;
  tension.mTensionRateIdx = 0; // 1/4
  tension.mTensionDepth = 1.0f;

  const auto base = run(plain, off, kReverbHall);
  const auto lfo = run(modulated, tension, kReverbHall);

  // Half a beat before the drop the LFO is fully closed (depth 100%).
  const size_t dip = static_cast<size_t>(kAnchorFrames - 12000);
  REQUIRE(std::abs(lfo->mLeft[dip]) < 1e-3f);
  REQUIRE(std::abs(base->mLeft[dip]) > 0.0f);

  // On the grid the audio passes at full level.
  const size_t grid = static_cast<size_t>(kAnchorFrames - 24000);
  REQUIRE(lfo->mLeft[grid] == Approx(base->mLeft[grid]).margin(1e-3));
}

TEST_CASE("Pipeline: post tail extends the riser past the drop", "[pipeline][shaping]")
{
  RvrseProcessor processor;
  ShapeSettings shape;
  const auto without = run(processor, shape, kReverbHall);
  const int framesWithout = without->NumFrames();
  const std::vector<float> beforeLeft = without->mLeft;

  shape.mPostTail = true;
  const auto with = processor.ReshapeForTests(shape);

  REQUIRE(with->NumFrames() > framesWithout);
  REQUIRE(with->mBeatAnchorFrames == without->mBeatAnchorFrames);

  // Pre-drop audio is untouched; the tail only adds from the hit start onwards.
  const size_t hitStart = static_cast<size_t>(with->HitStartFrame());
  for (size_t i = 0; i < hitStart; i += 997)
    REQUIRE(with->mLeft[i] == Approx(beforeLeft[i]).margin(1e-6));

  float tailEnergy = 0.0f;
  for (size_t i = static_cast<size_t>(framesWithout); i < with->mLeft.size(); ++i)
    tailEnergy += std::abs(with->mLeft[i]);
  REQUIRE(tailEnergy > 0.0f);
}

TEST_CASE("Pipeline: Auto loudness hits the target regardless of input level", "[pipeline][shaping]")
{
  const float target = std::pow(10.0f, kLoudnessTargetPeakDb / 20.0f);

  ShapeSettings shape;
  shape.mLoudness = kLoudnessAuto;

  RvrseProcessor quiet;
  RvrseProcessor loud;
  const auto quietRiser = run(quiet, shape, kReverbPlate, 0.05f);
  const auto loudRiser = run(loud, shape, kReverbPlate, 0.9f);

  REQUIRE(peakOf(*quietRiser) == Approx(target).epsilon(0.02));
  REQUIRE(peakOf(*loudRiser) == Approx(target).epsilon(0.02));
}

TEST_CASE("Pipeline: Locked loudness keeps the gain while settings change", "[pipeline][shaping]")
{
  const float target = std::pow(10.0f, kLoudnessTargetPeakDb / 20.0f);

  RvrseProcessor processor;
  ShapeSettings shape;
  shape.mLoudness = kLoudnessAuto;
  const auto autoRiser = run(processor, shape, kReverbRoom);
  REQUIRE(peakOf(*autoRiser) == Approx(target).epsilon(0.02));

  // Lock, then add a gate that removes part of the audio: the gain must NOT be recomputed,
  // so the peak can only stay the same or fall.
  shape.mLoudness = kLoudnessLocked;
  shape.mTensionTarget = kTensionVolume;
  shape.mTensionRateIdx = 0;
  shape.mTensionDepth = 1.0f;
  const auto locked = processor.ReshapeForTests(shape);
  REQUIRE(peakOf(*locked) <= peakOf(*autoRiser) * 1.0001f);

  // Auto would instead renormalize the quieter result back up to the target.
  shape.mLoudness = kLoudnessAuto;
  const auto renormalized = processor.ReshapeForTests(shape);
  REQUIRE(peakOf(*renormalized) == Approx(target).epsilon(0.02));
}

TEST_CASE("Pipeline: loudness Off keeps the original level", "[pipeline][shaping]")
{
  RvrseProcessor processor;
  ShapeSettings shape;
  shape.mLoudness = kLoudnessOff;
  const auto off = run(processor, shape, kReverbMatrix, 0.8f);

  const float target = std::pow(10.0f, kLoudnessTargetPeakDb / 20.0f);
  REQUIRE(peakOf(*off) != Approx(target).epsilon(0.001));
}

TEST_CASE("Pipeline: bar-based length follows the time signature", "[pipeline][shaping]")
{
  // 1 bar in 3/4 is 3 beats → 72000 frames at 120 BPM / 48 kHz.
  RvrseProcessor processor;
  const double beats = ResolveRiserLengthBeats(kRiserLengthBarsFirstIdx, 3.0);
  const auto riser = processor.RunFullPipelineForTests(
    makeHit(), kSr, 0.6f, kReverbRoom, kBpm, beats, ShapeSettings {});

  REQUIRE(riser->mBeatAnchorFrames == 3 * 24000);
}
