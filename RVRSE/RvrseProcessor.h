#pragma once

/// @file RvrseProcessor.h
/// @brief Offline pipeline orchestrator: reverb → reverse → time-stretch → shape.
///        Runs on a background thread, never on the audio thread.
///        On parameter changes, rebuilds from the earliest changed stage onwards.

#include "BufferUtils.h"
#include "Constants.h"
#include "ReverbEngineFactory.h"
#include "RiserShaping.h"
#include "SampleData.h"
#include "TimeStretch.h"
#include "TransitionTiming.h"
#include "TrimUtils.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace rvrse {

/// Result of the offline pipeline — the ready-to-play riser buffer plus
/// intermediate debug buffers for diagnostic playback (debug builds only).
struct RiserData
{
  std::vector<float> mLeft;    ///< Left channel of final_riser[] (stretched, shaped, faded)
  std::vector<float> mRight;   ///< Right channel of final_riser[]
  double mSampleRate = 0.0;    ///< Sample rate of the riser data
  int mBeatAnchorFrames = 0;       ///< Frame count at the exact musical beat anchor
  int mHitPreBeatFrames = 0;       ///< Early hit start before the beat (H/2)
  int mRequestedReleaseFrames = 0; ///< Requested release at render sample rate
  int mEffectiveReleaseFrames = 0; ///< Effective post-anchor release
  ETransitionMode mTransitionMode = ETransitionMode::RiserRelease;
  int mSequenceId = 0;         ///< Sequence token for coordinated riser+hit commits

#ifndef NDEBUG
  // Debug: intermediate pipeline buffers (populated alongside the main output)
  std::vector<float> mReverbedL;  ///< After reverb, before reverse/stretch
  std::vector<float> mReverbedR;
  std::vector<float> mReversedL;  ///< After reverse, before stretch
  std::vector<float> mReversedR;
#endif

  int NumFrames() const { return static_cast<int>(mLeft.size()); }
  bool IsReady() const { return !mLeft.empty() && mSampleRate > 0.0; }
  int HitStartFrame() const { return std::max(0, mBeatAnchorFrames - mHitPreBeatFrames); }
  int SequenceFrames(int hitFrames) const { return std::max(NumFrames(), HitStartFrame() + hitFrames); }
  bool IsReleaseLimited() const
  {
    return mTransitionMode == ETransitionMode::RiserRelease &&
           mEffectiveReleaseFrames < mRequestedReleaseFrames;
  }

#ifndef NDEBUG
  /// @return Number of frames in the reverbed debug buffer
  int ReverbedFrames() const { return static_cast<int>(mReverbedL.size()); }
  /// @return Number of frames in the reversed debug buffer
  int ReversedFrames() const { return static_cast<int>(mReversedL.size()); }
#endif
};

/// The offline pipeline orchestrator.
///
/// Usage:
///   1. Call `setSample()` when a new hit sample is loaded.
///   2. Call `setLush()` / `setReverbCharacter()` / `setRiserLength()` / `setBPM()` /
///      `setShape()` when parameters change.
///   3. Each setter triggers an async rebuild from the appropriate pipeline stage.
///   4. Poll `isNewRiserReady()` from the audio thread; if true, call `consumeRiser()`.
///
/// Stages and what invalidates them:
///   Reverb  (sample, trim, Lush, character, sample rate)  → reverb → reverse (cached)
///   Stretch (length, BPM, quality, release)               → stretch + fades  (cached "base")
///   Shape   (tension, gate, post tail, loudness, bar len) → LFO → gate → tail → loudness
///
/// Thread safety:
///   - Setters may be called from any thread (UI, main, etc.)
///   - `isNewRiserReady()` and `consumeRiser()` are safe from the audio thread (lock-free read)
///   - The pipeline itself runs on a detached background thread
class RvrseProcessor
{
public:
  RvrseProcessor() = default;

  // --- Setters (trigger async rebuild) ---

  /// Set the source hit sample. Triggers a full pipeline rebuild (reverb → reverse → stretch → shape).
  int setSample(std::shared_ptr<SampleData> sample,
                double trimStartMs = 0.0,
                double trimEndMs = 0.0)
  {
    int sequenceId = 0;
    {
      std::lock_guard<std::mutex> lock(mParamMutex);
      mSourceSample = std::move(sample);
      mTrimStartMs = trimStartMs;
      mTrimEndMs = trimEndMs;
      sequenceId = ++mSequenceId;
      clearReverbCachesLocked();
    }
    rebuildAsync(EPipelineStage::Reverb);
    return sequenceId;
  }

  /// Set the Lush amount (0.0–1.0). Triggers rebuild from reverb stage onwards.
  void setLush(float lush)
  {
    {
      std::lock_guard<std::mutex> lock(mParamMutex);
      if (std::abs(mLush - lush) < 1e-6f) return; // No change
      mLush = lush;
      clearReverbCachesLocked();
    }
    rebuildAsync(EPipelineStage::Reverb);
  }

  /// Set the reverb character (Matrix / Room / Hall / Plate). Triggers a reverb-stage rebuild.
  void setReverbCharacter(EReverbCharacter character)
  {
    {
      std::lock_guard<std::mutex> lock(mParamMutex);
      if (mReverbCharacter == character) return;
      mReverbCharacter = character;
      clearReverbCachesLocked();
    }
    rebuildAsync(EPipelineStage::Reverb);
  }

  /// Set manual sample trim in milliseconds from the original sample edges.
  /// Triggers a full rebuild because the source slice changes.
  int setTrimMs(double trimStartMs, double trimEndMs)
  {
    int sequenceId = 0;
    {
      std::lock_guard<std::mutex> lock(mParamMutex);
      if (std::abs(mTrimStartMs - trimStartMs) < 1e-9 &&
          std::abs(mTrimEndMs - trimEndMs) < 1e-9)
      {
        return mSequenceId;
      }

      mTrimStartMs = trimStartMs;
      mTrimEndMs = trimEndMs;
      sequenceId = ++mSequenceId;
      clearReverbCachesLocked();
    }
    rebuildAsync(EPipelineStage::Reverb);
    return sequenceId;
  }

  /// Set the riser length in beats. Only re-stretches (skips reverb + reverse).
  /// When offline, performs a synchronous rebuild so bounce/export picks up the
  /// change immediately. In real-time mode, uses async to avoid blocking audio.
  void setRiserLength(double beats, bool offline = false)
  {
    {
      std::lock_guard<std::mutex> lock(mParamMutex);
      if (std::abs(mRiserLengthBeats - beats) < 1e-9) return;
      mRiserLengthBeats = beats;
    }
    rebuild(EPipelineStage::Stretch, offline);
  }

  /// Set the host BPM. Only re-stretches.
  /// Same offline/real-time branching as setRiserLength.
  void setBPM(double bpm, bool offline = false)
  {
    {
      std::lock_guard<std::mutex> lock(mParamMutex);
      if (std::abs(mBPM - bpm) < 1e-9) return;
      mBPM = bpm;
    }
    rebuild(EPipelineStage::Stretch, offline);
  }

  /// Set the output sample rate. Triggers full rebuild if changed.
  void setSampleRate(double sr)
  {
    {
      std::lock_guard<std::mutex> lock(mParamMutex);
      if (std::abs(mOutputSampleRate - sr) < 1e-9) return;
      mOutputSampleRate = sr;
      clearReverbCachesLocked();
    }
    rebuildAsync(EPipelineStage::Reverb);
  }

  /// Set the stretch quality preset. Triggers stretch rebuild if changed.
  void setStretchQuality(EStretchQuality quality, bool offline = false)
  {
    {
      std::lock_guard<std::mutex> lock(mParamMutex);
      if (mStretchQuality == quality) return;
      mStretchQuality = quality;
    }
    rebuild(EPipelineStage::Stretch, offline);
  }

  /// Set the requested post-anchor riser release and transition mode.
  /// Numeric release changes rebuild only the stretch stage.
  void setRiserRelease(double releaseMs,
                       ETransitionMode mode = ETransitionMode::RiserRelease,
                       bool offline = false)
  {
    {
      std::lock_guard<std::mutex> lock(mParamMutex);
      releaseMs = std::clamp(releaseMs, kRiserReleaseMinMs, kRiserReleaseMaxMs);
      if (std::abs(mRiserReleaseMs - releaseMs) < 1e-9 && mTransitionMode == mode)
        return;
      mRiserReleaseMs = releaseMs;
      mTransitionMode = mode;
    }
    rebuild(EPipelineStage::Stretch, offline);
  }

  /// Set the shaping settings (Tension LFO, gate, post tail, loudness, bar length).
  /// Rebuilds only the cheap shape stage from the cached stretched riser.
  void setShape(const ShapeSettings& shape, bool offline = false)
  {
    {
      std::lock_guard<std::mutex> lock(mParamMutex);
      if (sameShape(mShape, shape)) return;

      // Leaving Auto/Locked forgets the remembered gain so a later lock starts fresh.
      if (shape.mLoudness == kLoudnessOff)
      {
        mHasAutoGain = false;
        mLockedGainValid = false;
      }
      else if (shape.mLoudness == kLoudnessAuto)
      {
        mLockedGainValid = false;
      }

      mShape = shape;
    }
    rebuild(EPipelineStage::Shape, offline);
  }

  // --- Audio-thread-safe polling ---

  /// @return true if a new riser buffer is ready to be consumed (lock-free).
  bool isNewRiserReady() const { return mNewRiserReady.load(std::memory_order_acquire); }

  /// Consume the new riser buffer (lock-free swap).
  /// Call this from the audio thread when `isNewRiserReady()` returns true.
  /// @return Shared pointer to the new riser data
  std::shared_ptr<RiserData> consumeRiser()
  {
    mNewRiserReady.store(false, std::memory_order_release);
    return std::atomic_load(&mRiserOutput);
  }

  /// Peek the latest published riser candidate without clearing the ready flag.
  std::shared_ptr<RiserData> peekRiser() const
  {
    return std::atomic_load(&mRiserOutput);
  }

  /// @return true if the pipeline is currently processing
  bool isProcessing() const
  {
    return mCompletedGeneration.load(std::memory_order_acquire) <
           mGeneration.load(std::memory_order_acquire);
  }

#ifdef RVRSE_TEST_BUILD
  /// Runs the reverb → reverse → stretch → shape pipeline synchronously with the
  /// processor's current settings (set them first via the normal setters).
  std::shared_ptr<RiserData> RunReverbPipelineForTests(std::shared_ptr<SampleData> sample,
                                                       double outputSampleRate,
                                                       float lush,
                                                       int sequenceId = 1)
  {
    {
      std::lock_guard<std::mutex> lock(mParamMutex);
      mSourceSample = std::move(sample);
      mOutputSampleRate = outputSampleRate;
      mLush = lush;
      mSequenceId = sequenceId;
      mTrimStartMs = 0.0;
      mTrimEndMs = 0.0;
      clearReverbCachesLocked();
    }

    const int generation = mGeneration.fetch_add(1, std::memory_order_release) + 1;
    runPipeline(EPipelineStage::Reverb, generation);
    return peekRiser();
  }

  /// Test hook: configure everything and run synchronously, no background threads.
  std::shared_ptr<RiserData> RunFullPipelineForTests(std::shared_ptr<SampleData> sample,
                                                     double outputSampleRate,
                                                     float lush,
                                                     EReverbCharacter character,
                                                     double bpm,
                                                     double riserLengthBeats,
                                                     const ShapeSettings& shape,
                                                     int sequenceId = 1)
  {
    {
      std::lock_guard<std::mutex> lock(mParamMutex);
      mSourceSample = std::move(sample);
      mOutputSampleRate = outputSampleRate;
      mLush = lush;
      mReverbCharacter = character;
      mBPM = bpm;
      mRiserLengthBeats = riserLengthBeats;
      mShape = shape;
      mSequenceId = sequenceId;
      mTrimStartMs = 0.0;
      mTrimEndMs = 0.0;
      clearReverbCachesLocked();
    }

    const int generation = mGeneration.fetch_add(1, std::memory_order_release) + 1;
    runPipeline(EPipelineStage::Reverb, generation);
    return peekRiser();
  }

  /// Test hook: re-run only the shape stage synchronously after changing settings.
  std::shared_ptr<RiserData> ReshapeForTests(const ShapeSettings& shape)
  {
    {
      std::lock_guard<std::mutex> lock(mParamMutex);
      mShape = shape;
    }

    const int generation = mGeneration.fetch_add(1, std::memory_order_release) + 1;
    runPipeline(EPipelineStage::Shape, generation);
    return peekRiser();
  }
#endif

private:
  /// Pipeline stages — rebuild starts from the specified stage onwards.
  enum class EPipelineStage
  {
    Reverb = 0, ///< Full rebuild: reverb → reverse → stretch → shape
    Stretch,    ///< Partial rebuild: time-stretch + shape (uses cached reversed buffer)
    Shape       ///< Partial rebuild: shape only (uses cached stretched base riser)
  };

  static bool sameShape(const ShapeSettings& a, const ShapeSettings& b)
  {
    return SameShape(a, b);
  }

  /// Drop everything derived from the reverb stage. Caller holds mParamMutex.
  void clearReverbCachesLocked()
  {
    mCachedReversedL.clear();
    mCachedReversedR.clear();
    mCachedTailL.clear();
    mCachedTailR.clear();
    mCachedBase.reset();
#ifndef NDEBUG
    mCachedReverbedL.clear();
    mCachedReverbedR.clear();
#endif
  }

  /// Offline renders rebuild synchronously (so bounce/export picks the change up
  /// immediately); real-time mode uses a background thread to never block audio.
  void rebuild(EPipelineStage stage, bool offline)
  {
    if (offline)
      rebuildSync(stage);
    else
      rebuildAsync(stage);
  }

  /// Launch a background thread to run the pipeline from the given stage.
  void rebuildAsync(EPipelineStage fromStage)
  {
    const int myGen = mGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;

    std::thread([this, fromStage, myGen]() {
      runPipeline(fromStage, myGen);
    }).detach();
  }

  /// Perform a partial rebuild synchronously on the calling thread.
  /// Falls back to async if the needed caches aren't available yet
  /// (i.e. the initial reverb+reverse pass hasn't completed).
  ///
  /// Only called during offline/bounce rendering where real-time deadlines
  /// don't apply. Never call this during real-time playback.
  void rebuildSync(EPipelineStage stage)
  {
    // Invalidate in-flight builds and capture our generation token
    const int myGen = mGeneration.fetch_add(1, std::memory_order_release) + 1;

    bool haveCaches = false;
    {
      std::lock_guard<std::mutex> lock(mParamMutex);
      const bool haveSample = mSourceSample && mSourceSample->IsLoaded();
      haveCaches = haveSample &&
                   (stage == EPipelineStage::Shape ? static_cast<bool>(mCachedBase)
                                                   : !mCachedReversedL.empty());
    }

    // No cached buffers yet — fall back to async (first load still in progress)
    if (!haveCaches)
    {
      rebuildAsync(stage);
      return;
    }

    runPipeline(stage, myGen);
  }

  /// Forward wet-only reverb signal (reverb output minus the dry component), used
  /// for the optional post tail.
  static void extractWetTail(const std::vector<float>& reverbedL, const std::vector<float>& reverbedR,
                             const std::vector<float>& dryL, const std::vector<float>& dryR,
                             float dryGain,
                             std::vector<float>& tailL, std::vector<float>& tailR)
  {
    const size_t n = reverbedL.size();
    tailL.resize(n);
    tailR.resize(n);
    for (size_t i = 0; i < n; ++i)
    {
      const float dl = i < dryL.size() ? dryL[i] : 0.0f;
      const float dr = i < dryR.size() ? dryR[i] : 0.0f;
      tailL[i] = reverbedL[i] - dl * dryGain;
      tailR[i] = reverbedR[i] - dr * dryGain;
    }
  }

  /// Resolve the loudness gain for the current mode and peak, updating the lock state.
  float resolveLoudnessGain(ELoudnessMode mode, float peak)
  {
    std::lock_guard<std::mutex> lock(mParamMutex);
    switch (mode)
    {
      case kLoudnessAuto:
      {
        const float gain = ComputeLoudnessGain(peak);
        mLastAutoGain = gain;
        mHasAutoGain = true;
        mLockedGainValid = false;
        return gain;
      }
      case kLoudnessLocked:
      {
        if (!mLockedGainValid)
        {
          mLockedGain = mHasAutoGain ? mLastAutoGain : ComputeLoudnessGain(peak);
          mLockedGainValid = true;
        }
        return mLockedGain;
      }
      case kLoudnessOff:
      default:
        return 1.0f;
    }
  }

  /// Shape stage: copy the base riser, then LFO → gate → post tail → loudness.
  std::shared_ptr<RiserData> buildShaped(const RiserData& base,
                                         const ShapeSettings& shape,
                                         double bpm,
                                         const std::vector<float>& tailL,
                                         const std::vector<float>& tailR)
  {
    auto riser = std::make_shared<RiserData>(base);

    ApplyTensionLfo(riser->mLeft, riser->mRight, riser->mBeatAnchorFrames, bpm,
                    riser->mSampleRate, shape.mTensionTarget, shape.mTensionRateIdx,
                    shape.mTensionDepth);

    ApplyLastBarGate(riser->mLeft, riser->mRight, riser->mBeatAnchorFrames, bpm,
                     riser->mSampleRate, shape.mGateRegion, shape.mGateRateIdx,
                     shape.mBeatsPerBar);

    if (shape.mPostTail)
      MixPostTail(riser->mLeft, riser->mRight, tailL, tailR,
                  riser->HitStartFrame(), riser->mSampleRate);

    const float gain = resolveLoudnessGain(shape.mLoudness,
                                           ComputePeak(riser->mLeft, riser->mRight));
    ApplyGain(riser->mLeft, riser->mRight, gain);

    return riser;
  }

  /// Execute the pipeline synchronously (called on background thread).
  void runPipeline(EPipelineStage fromStage, int generation)
  {
    // Snapshot parameters under lock
    std::shared_ptr<SampleData> sample;
    float lush;
    EReverbCharacter character;
    double riserLengthBeats, bpm, sampleRate, trimStartMs, trimEndMs, riserReleaseMs;
    EStretchQuality quality;
    ETransitionMode transitionMode;
    ShapeSettings shape;
    int sequenceId;
    std::vector<float> cachedRevL, cachedRevR, cachedTailL, cachedTailR;
    std::shared_ptr<const RiserData> cachedBase;
#ifndef NDEBUG
    std::vector<float> cachedRvbL, cachedRvbR;
#endif

    {
      std::lock_guard<std::mutex> lock(mParamMutex);
      sample = mSourceSample;
      lush = mLush;
      character = mReverbCharacter;
      riserLengthBeats = mRiserLengthBeats;
      bpm = mBPM;
      sampleRate = mOutputSampleRate;
      trimStartMs = mTrimStartMs;
      trimEndMs = mTrimEndMs;
      riserReleaseMs = mRiserReleaseMs;
      quality = mStretchQuality;
      transitionMode = mTransitionMode;
      shape = mShape;
      sequenceId = mSequenceId;

      if (fromStage != EPipelineStage::Reverb)
      {
        cachedRevL = mCachedReversedL;
        cachedRevR = mCachedReversedR;
        cachedTailL = mCachedTailL;
        cachedTailR = mCachedTailR;
#ifndef NDEBUG
        cachedRvbL = mCachedReverbedL;
        cachedRvbR = mCachedReverbedR;
#endif
      }
      if (fromStage == EPipelineStage::Shape)
        cachedBase = mCachedBase;
    }

    // Bail early if no sample loaded
    if (!sample || !sample->IsLoaded())
    {
      completeIfCurrent(generation);
      return;
    }

    // Check if this build has been superseded
    if (mGeneration.load(std::memory_order_acquire) != generation)
      return;

    // --- Shape-only rebuild: reuse the cached stretched base riser ---
    if (fromStage == EPipelineStage::Shape && cachedBase && cachedBase->IsReady())
    {
      auto shaped = buildShaped(*cachedBase, shape, bpm, cachedTailL, cachedTailR);
      publish(shaped, generation);
      return;
    }

    std::vector<float> reversedL, reversedR, tailL, tailR;
#ifndef NDEBUG
    std::vector<float> reverbedL, reverbedR;
#endif

    if (fromStage == EPipelineStage::Reverb || cachedRevL.empty())
    {
      // --- Stage 0: Resample to output sample rate if needed ---
      // The loaded sample may be at a different rate (e.g. 96kHz file, 48kHz DAW).
      // All pipeline processing must happen at the output rate so that frame counts
      // match what ProcessBlock expects.
      std::vector<float> srcL, srcR;
      double processingRate;

      const TrimRangeFrames trimRange = ResolveTrimRangeFrames(
        sample->NumFrames(), sample->mSampleRate, trimStartMs, trimEndMs, kTrimMinRegionMs
      );

      if (std::abs(sample->mSampleRate - sampleRate) > 0.5)
      {
        std::vector<float> trimmedL(sample->mLeft.begin() + trimRange.mStartFrame,
                                    sample->mLeft.begin() + trimRange.mEndFrameExclusive);
        std::vector<float> trimmedR(sample->mRight.begin() + trimRange.mStartFrame,
                                    sample->mRight.begin() + trimRange.mEndFrameExclusive);
        resampleLinearStereo(trimmedL, trimmedR,
                             sample->mSampleRate, sampleRate,
                             srcL, srcR);
        processingRate = sampleRate;
      }
      else
      {
        srcL.assign(sample->mLeft.begin() + trimRange.mStartFrame,
                    sample->mLeft.begin() + trimRange.mEndFrameExclusive);
        srcR.assign(sample->mRight.begin() + trimRange.mStartFrame,
                    sample->mRight.begin() + trimRange.mEndFrameExclusive);
        processingRate = sample->mSampleRate;
      }

      // --- Stage 1: Reverb (with tail extension) ---
      // The reverb needs room to ring out. We append silence to the source
      // buffer so the delay lines can decay naturally. Without this, the reverb
      // tail is truncated and the reversed riser has no "whoosh" build-up —
      // defeating the entire purpose of the effect.
      const size_t srcFrames = srcL.size();
      const size_t tailFrames = static_cast<size_t>(processingRate * kReverbTailSeconds);
      const size_t totalFrames = srcFrames + tailFrames;

      // Pad source with silence for the reverb tail
      srcL.resize(totalFrames, 0.0f);
      srcR.resize(totalFrames, 0.0f);

      std::vector<float> lushedL(totalFrames);
      std::vector<float> lushedR(totalFrames);

      ReverbSettings reverbSettings;
      reverbSettings.mLush = lush;
      reverbSettings.mCharacter = character;

      auto reverbEngine = MakeReverbEngine(character);
      if (!reverbEngine)
      {
        completeIfCurrent(generation);
        return;
      }

      reverbEngine->ProcessStereo(
        srcL.data(), srcR.data(),
        lushedL.data(), lushedR.data(),
        totalFrames, processingRate, reverbSettings
      );

      // Trim trailing silence so the stretcher doesn't waste work on dead air.
      // For quiet sources, clamp the threshold to the reverbed peak so a fixed
      // absolute gate can't erase the whole buffer at high Lush settings.
      float peakAbs = 0.0f;
      for (const float sampleValue : lushedL)
        peakAbs = std::max(peakAbs, std::abs(sampleValue));
      for (const float sampleValue : lushedR)
        peakAbs = std::max(peakAbs, std::abs(sampleValue));

      const float silenceThreshold = peakAbs > 0.0f
        ? std::min(kSilenceThreshold, peakAbs * kSilenceThresholdPeakFraction)
        : kSilenceThreshold;
      trimTrailingSilenceStereo(lushedL, lushedR, silenceThreshold);

      // Abort check
      if (mGeneration.load(std::memory_order_acquire) != generation)
        return;

      // Forward wet-only tail for the optional post tail (before the buffers are reversed).
      extractWetTail(lushedL, lushedR, srcL, srcR, GetReverbDryGain(lush), tailL, tailR);

      // --- Stage 2: Reverse ---
#ifndef NDEBUG
      // Save pre-reverse (reverbed) buffers for debug playback
      reverbedL = lushedL;
      reverbedR = lushedR;
#endif

      reverseBufferStereo(lushedL, lushedR);
      reversedL = std::move(lushedL);
      reversedR = std::move(lushedR);

      // Cache the reversed buffers and forward tail for future partial rebuilds
      {
        std::lock_guard<std::mutex> lock(mParamMutex);
        if (mGeneration.load(std::memory_order_acquire) != generation)
          return;

        mCachedReversedL = reversedL;
        mCachedReversedR = reversedR;
        mCachedTailL = tailL;
        mCachedTailR = tailR;
#ifndef NDEBUG
        mCachedReverbedL = reverbedL;
        mCachedReverbedR = reverbedR;
#endif
      }
    }
    else
    {
      // Use cached buffers (stretch/shape rebuild)
      reversedL = std::move(cachedRevL);
      reversedR = std::move(cachedRevR);
      tailL = std::move(cachedTailL);
      tailR = std::move(cachedTailR);
#ifndef NDEBUG
      reverbedL = std::move(cachedRvbL);
      reverbedR = std::move(cachedRvbR);
#endif
    }

    // Abort check
    if (mGeneration.load(std::memory_order_acquire) != generation)
      return;

    // --- Stage 3: Stretch + fades → base riser ---
    const TransitionTiming transitionTiming = CalculateTransitionTiming(
      static_cast<int>(reversedL.size()), riserLengthBeats, bpm, sampleRate,
      riserReleaseMs, transitionMode
    );

    auto base = std::make_shared<RiserData>();
    stretchBufferStereo(reversedL, reversedR, transitionTiming.mStretchFactor,
                        base->mLeft, base->mRight, sampleRate, quality);

    const int headFadeFrames = std::max(0, static_cast<int>(
      std::lround(sampleRate * kTechnicalHeadFadeInMs / 1000.0)
    ));
    if (headFadeFrames > 0)
      applyHeadFadeInStereo(base->mLeft, base->mRight, headFadeFrames);

    base->mSampleRate = sampleRate;
    base->mSequenceId = sequenceId;
    base->mBeatAnchorFrames = transitionTiming.mBeatAnchorFrames;
    base->mHitPreBeatFrames = transitionTiming.mHitPreBeatFrames;
    base->mRequestedReleaseFrames = transitionTiming.mRequestedReleaseFrames;
    base->mEffectiveReleaseFrames = transitionTiming.mEffectiveReleaseFrames;
    base->mTransitionMode = transitionTiming.mMode;

#ifndef NDEBUG
    // Store intermediate buffers for debug playback
    base->mReverbedL = std::move(reverbedL);
    base->mReverbedR = std::move(reverbedR);
    base->mReversedL = std::move(reversedL);
    base->mReversedR = std::move(reversedR);
#endif

    if (transitionTiming.mMode == ETransitionMode::LegacyAdaptive &&
        transitionTiming.mTailFadeFrames > 0)
    {
      applyTailFadeOutStereo(base->mLeft, base->mRight, transitionTiming.mTailFadeFrames);
    }
    else if (transitionTiming.mEffectiveReleaseFrames > 0)
    {
      applyLinearFadeOutRangeStereo(
        base->mLeft, base->mRight,
        transitionTiming.mBeatAnchorFrames, transitionTiming.RiserEndFrame()
      );
    }

    // Abort check, then keep the base so shape-only changes skip the stretch.
    {
      std::lock_guard<std::mutex> lock(mParamMutex);
      if (mGeneration.load(std::memory_order_acquire) != generation)
        return;
      mCachedBase = base;
    }

    // --- Stage 4: Shape (Tension LFO → gate → post tail → loudness) ---
    auto shaped = buildShaped(*base, shape, bpm, tailL, tailR);
    publish(shaped, generation);
  }

  /// Publish a finished riser if no newer rebuild has been requested.
  void publish(const std::shared_ptr<RiserData>& riser, int generation)
  {
    // Final abort check before publishing
    if (mGeneration.load(std::memory_order_acquire) != generation)
      return;

    // Atomic store for lock-free audio-thread read
    std::atomic_store(&mRiserOutput, riser);
    mNewRiserReady.store(true, std::memory_order_release);
    completeIfCurrent(generation);
  }

  void completeIfCurrent(int generation)
  {
    if (mGeneration.load(std::memory_order_acquire) == generation)
      mCompletedGeneration.store(generation, std::memory_order_release);
  }

  // --- Parameters (protected by mParamMutex) ---
  std::mutex mParamMutex;
  std::shared_ptr<SampleData> mSourceSample;
  float mLush = static_cast<float>(kLushDefault / 100.0);
  EReverbCharacter mReverbCharacter = static_cast<EReverbCharacter>(kReverbCharacterDefault);
  double mRiserLengthBeats = kRiserLengthValues[kRiserLengthDefault];
  double mBPM = kDefaultBPM;
  double mOutputSampleRate = 44100.0;
  double mTrimStartMs = 0.0;
  double mTrimEndMs = 0.0;
  double mRiserReleaseMs = kRiserReleaseDefaultMs;
  ETransitionMode mTransitionMode = ETransitionMode::RiserRelease;
  EStretchQuality mStretchQuality = static_cast<EStretchQuality>(kStretchQualityDefault);
  ShapeSettings mShape;
  int mSequenceId = 0;

  // --- Loudness lock state (protected by mParamMutex) ---
  float mLastAutoGain = 1.0f;     ///< Gain most recently computed in Auto mode
  bool mHasAutoGain = false;      ///< Whether mLastAutoGain is valid
  float mLockedGain = 1.0f;       ///< Gain held while in Locked mode
  bool mLockedGainValid = false;  ///< Whether mLockedGain has been captured

  // --- Cached intermediate buffers (protected by mParamMutex) ---
  std::vector<float> mCachedReversedL;
  std::vector<float> mCachedReversedR;
  std::vector<float> mCachedTailL;  ///< Forward wet-only reverb tail (for Post tail)
  std::vector<float> mCachedTailR;
  std::shared_ptr<const RiserData> mCachedBase; ///< Stretched + faded riser before shaping
#ifndef NDEBUG
  std::vector<float> mCachedReverbedL;
  std::vector<float> mCachedReverbedR;
#endif

  // --- Output (lock-free handoff to audio thread) ---
  std::shared_ptr<RiserData> mRiserOutput;
  std::atomic<bool> mNewRiserReady { false };

  // --- Build management ---
  std::atomic<int> mGeneration { 0 };   ///< Incremented on every rebuild request
  std::atomic<int> mCompletedGeneration { 0 }; ///< Latest current generation that finished
};

} // namespace rvrse
