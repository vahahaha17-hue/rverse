#pragma once

/// @file FdnReverbEngine.h
/// @brief Offline 8-line feedback-delay-network reverb with Room / Hall / Plate voicings.
///
/// Signal flow per channel: input diffusion (series allpasses) → injected into the
/// delay lines → orthogonal Hadamard feedback matrix → per-line decay gain and
/// one-pole damping. Output is tapped from the lines (even lines → left, odd → right).
/// Offline only: allocates its delay lines per call, never used on the audio thread.

#include "Constants.h"
#include "ReverbEngine.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

namespace rvrse {

/// Static voicing description for one FDN character.
struct FdnVoicing
{
  std::array<double, 8> mDelayMs;  ///< Delay line lengths (mutually non-harmonic)
  double mRt60Seconds;             ///< Reverb time at Lush = 100% reference scale
  double mDampingHz;               ///< High-frequency damping cutoff in the loop
  double mModDepthMs;              ///< Delay modulation depth (decorrelates modes)
  double mModRateHz;               ///< Delay modulation rate
  int    mDiffusionStages;         ///< Input allpass stages (1–4)
  double mOutputGain;              ///< Wet output trim for this voicing
};

inline FdnVoicing GetFdnVoicing(EReverbCharacter character)
{
  switch (character)
  {
    case kReverbRoom:
      return { { 11.3, 13.7, 17.9, 19.1, 23.3, 27.7, 31.1, 37.3 }, 1.1, 6500.0, 0.15, 0.35, 2, 1.0 };
    case kReverbPlate:
      return { { 7.1, 9.3, 11.7, 13.9, 17.3, 19.7, 23.1, 29.9 }, 2.4, 11000.0, 0.30, 0.55, 4, 0.9 };
    case kReverbHall:
    default:
      return { { 29.7, 37.1, 41.1, 43.7, 53.9, 59.3, 67.1, 79.3 }, 3.6, 4200.0, 0.45, 0.30, 3, 0.9 };
  }
}

/// Maps Lush (0–1) to a reverb-time multiplier: shorter at low Lush, longer at high Lush.
inline double GetFdnRt60Scale(float lush)
{
  return 0.55 + 0.95 * static_cast<double>(ClampLush(lush));
}

namespace fdn_detail {

/// Schroeder allpass with a fixed delay, used for input diffusion.
class Allpass
{
public:
  void Init(size_t delayFrames, float coefficient)
  {
    mBuffer.assign(std::max<size_t>(1, delayFrames), 0.0f);
    mIndex = 0;
    mCoefficient = coefficient;
  }

  float Process(float input)
  {
    const float delayed = mBuffer[mIndex];
    const float v = input - mCoefficient * delayed;
    mBuffer[mIndex] = v;
    mIndex = (mIndex + 1) % mBuffer.size();
    return delayed + mCoefficient * v;
  }

private:
  std::vector<float> mBuffer;
  size_t mIndex = 0;
  float mCoefficient = 0.5f;
};

/// Fast in-place 8-point Walsh–Hadamard transform, scaled to be orthonormal.
inline void Hadamard8(std::array<float, 8>& v)
{
  for (size_t half = 1; half < 8; half *= 2)
  {
    for (size_t start = 0; start < 8; start += half * 2)
    {
      for (size_t i = start; i < start + half; ++i)
      {
        const float a = v[i];
        const float b = v[i + half];
        v[i] = a + b;
        v[i + half] = a - b;
      }
    }
  }

  constexpr float kScale = 0.35355339059327373f; // 1 / sqrt(8)
  for (float& x : v)
    x *= kScale;
}

} // namespace fdn_detail

class FdnReverbEngine final : public IReverbEngine
{
public:
  explicit FdnReverbEngine(EReverbCharacter character = kReverbHall)
  : mCharacter(character == kReverbMatrix ? kReverbHall : character)
  {
  }

  const char* GetName() const override { return "RvrseFDN"; }

  void ProcessStereo(const float* inL, const float* inR,
                     float* outL, float* outR,
                     size_t numFrames,
                     double sampleRate,
                     const ReverbSettings& settings) const override
  {
    const float dryGain = GetReverbDryGain(settings.mLush);
    const float wetGain = GetReverbWetGain(settings.mLush);

    if (wetGain <= 0.0f || numFrames == 0 || sampleRate <= 0.0)
    {
      std::copy_n(inL, numFrames, outL);
      std::copy_n(inR, numFrames, outR);
      return;
    }

    const FdnVoicing voicing = GetFdnVoicing(mCharacter);
    const double rt60 = voicing.mRt60Seconds * GetFdnRt60Scale(settings.mLush);

    // --- Delay lines ---
    constexpr size_t kLines = 8;
    const double modDepthFrames = voicing.mModDepthMs * 0.001 * sampleRate;
    std::array<std::vector<float>, kLines> lines;
    std::array<double, kLines> delayFrames {};
    std::array<float, kLines> decayGain {};
    std::array<size_t, kLines> writeIndex {};
    std::array<float, kLines> lowpassState {};

    for (size_t i = 0; i < kLines; ++i)
    {
      delayFrames[i] = std::max(2.0, voicing.mDelayMs[i] * 0.001 * sampleRate);
      const size_t length = static_cast<size_t>(std::ceil(delayFrames[i] + modDepthFrames)) + 4;
      lines[i].assign(length, 0.0f);

      // -60 dB after rt60 seconds of recirculation through this line.
      const double delaySeconds = delayFrames[i] / sampleRate;
      decayGain[i] = static_cast<float>(std::pow(10.0, -3.0 * delaySeconds / std::max(0.05, rt60)));
    }

    const float dampCoeff = static_cast<float>(
      std::exp(-kTwoPi * std::min(voicing.mDampingHz, sampleRate * 0.45) / sampleRate)
    );

    // --- Input diffusion (different lengths per channel to decorrelate L/R) ---
    constexpr std::array<double, 4> kDiffusionMsL = { 4.77, 3.63, 2.71, 1.93 };
    constexpr std::array<double, 4> kDiffusionMsR = { 5.13, 3.91, 2.89, 2.07 };
    std::array<fdn_detail::Allpass, 4> diffusionL;
    std::array<fdn_detail::Allpass, 4> diffusionR;
    const int stages = std::clamp(voicing.mDiffusionStages, 0, 4);
    for (int s = 0; s < stages; ++s)
    {
      diffusionL[static_cast<size_t>(s)].Init(
        static_cast<size_t>(kDiffusionMsL[static_cast<size_t>(s)] * 0.001 * sampleRate), 0.6f);
      diffusionR[static_cast<size_t>(s)].Init(
        static_cast<size_t>(kDiffusionMsR[static_cast<size_t>(s)] * 0.001 * sampleRate), 0.6f);
    }

    const double modIncrement = kTwoPi * voicing.mModRateHz / sampleRate;
    std::array<double, kLines> modPhase {};
    for (size_t i = 0; i < kLines; ++i)
      modPhase[i] = kTwoPi * static_cast<double>(i) / static_cast<double>(kLines);

    const float outputScale = static_cast<float>(voicing.mOutputGain) * 0.5f;
    constexpr float kInputScale = 0.5f;

    for (size_t n = 0; n < numFrames; ++n)
    {
      float dl = inL[n];
      float dr = inR[n];
      for (int s = 0; s < stages; ++s)
      {
        dl = diffusionL[static_cast<size_t>(s)].Process(dl);
        dr = diffusionR[static_cast<size_t>(s)].Process(dr);
      }

      // Read each line at its (modulated) delay, apply damping and decay.
      std::array<float, kLines> taps {};
      for (size_t i = 0; i < kLines; ++i)
      {
        const double mod = modDepthFrames * std::sin(modPhase[i]);
        modPhase[i] += modIncrement;

        const double readPos = static_cast<double>(writeIndex[i]) - (delayFrames[i] + mod);
        const double wrapped = readPos < 0.0 ? readPos + static_cast<double>(lines[i].size()) : readPos;
        const size_t i0 = static_cast<size_t>(wrapped) % lines[i].size();
        const size_t i1 = (i0 + 1) % lines[i].size();
        const float frac = static_cast<float>(wrapped - std::floor(wrapped));
        const float raw = lines[i][i0] + (lines[i][i1] - lines[i][i0]) * frac;

        lowpassState[i] = raw * (1.0f - dampCoeff) + lowpassState[i] * dampCoeff;
        taps[i] = lowpassState[i] * decayGain[i];
      }

      // Output: even lines left, odd lines right (alternating signs keep it balanced).
      float wetL = 0.0f;
      float wetR = 0.0f;
      for (size_t i = 0; i < kLines; i += 2)
      {
        const float sign = (i % 4 == 0) ? 1.0f : -1.0f;
        wetL += sign * taps[i];
        wetR += sign * taps[i + 1];
      }

      // Feedback through the orthogonal matrix, plus input injection.
      std::array<float, kLines> feedback = taps;
      fdn_detail::Hadamard8(feedback);
      for (size_t i = 0; i < kLines; ++i)
      {
        const float injected = ((i % 2 == 0) ? dl : dr) * kInputScale;
        lines[i][writeIndex[i]] = feedback[i] + injected;
        writeIndex[i] = (writeIndex[i] + 1) % lines[i].size();
      }

      outL[n] = inL[n] * dryGain + wetL * outputScale * wetGain;
      outR[n] = inR[n] * dryGain + wetR * outputScale * wetGain;
    }
  }

private:
  EReverbCharacter mCharacter;
};

} // namespace rvrse
