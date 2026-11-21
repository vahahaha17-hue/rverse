#pragma once

/// @file RiserShaping.h
/// @brief Offline, stateless riser shaping: Tension LFO, last-bar gate, post tail, loudness.
///
/// All functions work on a stereo riser whose musical anchor (the drop) sits at
/// `anchorFrames`. They are tempo-aware but pure: explicit inputs, no hidden state.
/// Offline only — they allocate and loop over whole buffers.

#include "Constants.h"
#include "TransitionTiming.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace rvrse {

/// User-facing shaping settings applied after the riser has been stretched.
struct ShapeSettings
{
  ETensionTarget mTensionTarget = kTensionOff;
  int mTensionRateIdx = kTensionRateDefault;
  float mTensionDepth = static_cast<float>(kTensionDepthDefault / 100.0); ///< 0–1
  EGateRegion mGateRegion = kGateOff;
  int mGateRateIdx = kGateRateDefault;
  bool mPostTail = false;
  ELoudnessMode mLoudness = kLoudnessOff;
  double mBeatsPerBar = 4.0; ///< From the host time signature (numerator × 4 / denominator)
};

/// True when two shape settings would produce the same audio.
inline bool SameShape(const ShapeSettings& a, const ShapeSettings& b)
{
  return a.mTensionTarget == b.mTensionTarget &&
         a.mTensionRateIdx == b.mTensionRateIdx &&
         std::abs(a.mTensionDepth - b.mTensionDepth) < 1e-6f &&
         a.mGateRegion == b.mGateRegion &&
         a.mGateRateIdx == b.mGateRateIdx &&
         a.mPostTail == b.mPostTail &&
         a.mLoudness == b.mLoudness &&
         std::abs(a.mBeatsPerBar - b.mBeatsPerBar) < 1e-9;
}

inline double GetTensionRateBeats(int rateIdx)
{
  return kTensionRateBeats[std::clamp(rateIdx, 0, kNumTensionRates - 1)];
}

inline double GetGateRateBeats(int rateIdx)
{
  return kGateRateBeats[std::clamp(rateIdx, 0, kNumGateRates - 1)];
}

/// Number of beats covered by a gate region (0 for Off).
inline double GetGateRegionBeats(EGateRegion region, double beatsPerBar)
{
  switch (region)
  {
    case kGateLastBeat:     return 1.0;
    case kGateLastTwoBeats: return 2.0;
    case kGateLastBar:      return std::max(1.0, beatsPerBar);
    case kGateOff:
    default:                return 0.0;
  }
}

/// Beats per bar from a time signature, e.g. 6/8 → 3 beats, 3/4 → 3, 4/4 → 4.
inline double BeatsPerBarFromTimeSig(int numerator, int denominator)
{
  if (numerator <= 0 || denominator <= 0)
    return 4.0;

  return static_cast<double>(numerator) * 4.0 / static_cast<double>(denominator);
}

/// Tempo-synced LFO "open amount" in [0,1]-dip form: 1 on the grid, dipping by `depth` between.
/// Phase is anchored so the LFO is at full level exactly on the drop and every step before it.
inline double TensionLfoGain(double framesFromAnchor, double periodFrames, double depth)
{
  if (periodFrames <= 0.0)
    return 1.0;

  const double phase = kTwoPi * framesFromAnchor / periodFrames;
  const double dip = 0.5 - 0.5 * std::cos(phase); // 0 on grid, 1 half-way between
  return 1.0 - depth * dip;
}

/// Apply the Tension LFO (volume or low-pass filter) across the whole riser buffer.
inline void ApplyTensionLfo(std::vector<float>& left, std::vector<float>& right,
                            int anchorFrames, double bpm, double sampleRate,
                            ETensionTarget target, int rateIdx, float depth)
{
  if (target == kTensionOff || bpm <= 0.0 || sampleRate <= 0.0 || left.empty() ||
      left.size() != right.size())
  {
    return;
  }

  const double clampedDepth = std::clamp(static_cast<double>(depth), 0.0, 1.0);
  if (clampedDepth <= 0.0)
    return;

  const double periodFrames = GetTensionRateBeats(rateIdx) * sampleRate * 60.0 / bpm;
  if (periodFrames < 2.0)
    return;

  const size_t n = left.size();

  if (target == kTensionVolume)
  {
    for (size_t i = 0; i < n; ++i)
    {
      const float g = static_cast<float>(TensionLfoGain(
        static_cast<double>(i) - static_cast<double>(anchorFrames), periodFrames, clampedDepth));
      left[i] *= g;
      right[i] *= g;
    }
    return;
  }

  // Filter: one-pole low-pass whose cutoff dips from fully open (on the grid) towards
  // kTensionFilterMinHz as depth rises. Coefficients refresh every few frames.
  const double minHz = kTensionFilterMinHz;
  const double maxHz = std::min(kTensionFilterMaxHz, sampleRate * 0.45);
  const double ratio = maxHz / minHz;
  float stateL = 0.0f;
  float stateR = 0.0f;
  float alpha = 1.0f;

  for (size_t i = 0; i < n; ++i)
  {
    if (i % static_cast<size_t>(kTensionFilterUpdateFrames) == 0)
    {
      const double open = TensionLfoGain(
        static_cast<double>(i) - static_cast<double>(anchorFrames), periodFrames, clampedDepth);
      const double cutoffHz = minHz * std::pow(ratio, open);
      alpha = static_cast<float>(1.0 - std::exp(-kTwoPi * cutoffHz / sampleRate));
    }

    stateL += alpha * (left[i] - stateL);
    stateR += alpha * (right[i] - stateR);
    left[i] = stateL;
    right[i] = stateR;
  }
}

/// Chop the region before the drop into rhythmic open/closed steps.
inline void ApplyLastBarGate(std::vector<float>& left, std::vector<float>& right,
                             int anchorFrames, double bpm, double sampleRate,
                             EGateRegion region, int rateIdx, double beatsPerBar)
{
  const double regionBeats = GetGateRegionBeats(region, beatsPerBar);
  if (regionBeats <= 0.0 || bpm <= 0.0 || sampleRate <= 0.0 || anchorFrames <= 0 ||
      left.empty() || left.size() != right.size())
  {
    return;
  }

  const double framesPerBeat = sampleRate * 60.0 / bpm;
  const double stepFrames = GetGateRateBeats(rateIdx) * framesPerBeat;
  if (stepFrames < 4.0)
    return;

  const double openFrames = stepFrames * kGateDuty;
  const double rampFrames = std::max(
    1.0, std::min(kGateRampMs * 0.001 * sampleRate, openFrames * 0.25));

  const int regionFrames = static_cast<int>(std::lround(regionBeats * framesPerBeat));
  const int regionStart = std::max(0, anchorFrames - regionFrames);
  const int regionEnd = std::min(anchorFrames, static_cast<int>(left.size()));

  for (int i = regionStart; i < regionEnd; ++i)
  {
    const double posInStep = std::fmod(static_cast<double>(i - regionStart), stepFrames);

    float gain = 0.0f;
    if (posInStep < openFrames)
    {
      const double fadeIn = std::min(1.0, posInStep / rampFrames);
      const double fadeOut = std::min(1.0, (openFrames - posInStep) / rampFrames);
      gain = static_cast<float>(std::min(fadeIn, fadeOut));
    }

    left[static_cast<size_t>(i)] *= gain;
    right[static_cast<size_t>(i)] *= gain;
  }
}

/// Mix a forward reverb tail into the riser starting at `startFrame` (the hit onset).
/// The riser buffers grow if the tail extends past their current end.
inline void MixPostTail(std::vector<float>& left, std::vector<float>& right,
                        const std::vector<float>& tailL, const std::vector<float>& tailR,
                        int startFrame, double sampleRate)
{
  if (tailL.empty() || tailL.size() != tailR.size() || sampleRate <= 0.0 || startFrame < 0)
    return;

  const size_t tailFrames = tailL.size();
  const size_t needed = static_cast<size_t>(startFrame) + tailFrames;
  if (left.size() < needed)
  {
    left.resize(needed, 0.0f);
    right.resize(needed, 0.0f);
  }

  const size_t fadeFrames = std::min(
    tailFrames, static_cast<size_t>(std::max(1.0, kPostTailFadeOutMs * 0.001 * sampleRate)));

  for (size_t i = 0; i < tailFrames; ++i)
  {
    float gain = kPostTailGain;
    const size_t fromEnd = tailFrames - i;
    if (fromEnd < fadeFrames)
      gain *= static_cast<float>(fromEnd) / static_cast<float>(fadeFrames);

    left[static_cast<size_t>(startFrame) + i] += tailL[i] * gain;
    right[static_cast<size_t>(startFrame) + i] += tailR[i] * gain;
  }
}

/// Peak absolute sample across both channels.
inline float ComputePeak(const std::vector<float>& left, const std::vector<float>& right)
{
  float peak = 0.0f;
  for (const float v : left)
    peak = std::max(peak, std::abs(v));
  for (const float v : right)
    peak = std::max(peak, std::abs(v));
  return peak;
}

/// Linear gain that brings `peak` to the Auto target, bounded to the allowed range.
/// Returns 1 for effectively silent input so silence is never amplified.
inline float ComputeLoudnessGain(float peak)
{
  if (!(peak > kLoudnessSilencePeak))
    return 1.0f;

  const float targetPeak = std::pow(10.0f, kLoudnessTargetPeakDb / 20.0f);
  const float minGain = std::pow(10.0f, kLoudnessMinGainDb / 20.0f);
  const float maxGain = std::pow(10.0f, kLoudnessMaxGainDb / 20.0f);
  return std::clamp(targetPeak / peak, minGain, maxGain);
}

inline void ApplyGain(std::vector<float>& left, std::vector<float>& right, float gain)
{
  if (gain == 1.0f)
    return;

  for (float& v : left)
    v *= gain;
  for (float& v : right)
    v *= gain;
}

} // namespace rvrse
