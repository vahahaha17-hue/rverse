#pragma once

/// @file Presets.h
/// @brief Factory presets as plain data. Applying them only changes parameters —
///        it never touches the loaded sample (iPlug2's chunk presets would reload it).
///
/// Entry 0 is "Init" (the plugin defaults); entries 1–15 are the factory presets in four
/// categories: Clean, Rhythmic, Cinematic, Character.

#include "Constants.h"

#include <string>

namespace rvrse {

enum class EPresetCategory
{
  Init = 0,
  Clean,
  Rhythmic,
  Cinematic,
  Character
};

struct Preset
{
  const char* mName;
  EPresetCategory mCategory;
  EReverbCharacter mCharacter;
  double mLushPct;           ///< 0–100
  int mLengthIdx;            ///< Riser Length enum index (beats or bars)
  double mFadeInPct;         ///< 0–100
  ETensionTarget mTensionTarget;
  int mTensionRateIdx;
  double mTensionDepthPct;   ///< 0–100
  EGateRegion mGateRegion;
  int mGateRateIdx;
  bool mPostTail;
};

constexpr int kNumPresetEntries = kNumUserPresets + 1; ///< "Init" + factory presets

constexpr int kLenBar1 = kRiserLengthBarsFirstIdx + 0;
constexpr int kLenBar2 = kRiserLengthBarsFirstIdx + 1;
constexpr int kLenBar4 = kRiserLengthBarsFirstIdx + 2;

inline constexpr Preset kPresets[kNumPresetEntries] = {
  // name               category                      character     lush  length    fade tension target     rate depth  gate region          rate tail
  { "Init",             EPresetCategory::Init,       kReverbMatrix, 40.0, kRiserLen_4, 60.0, kTensionOff,    1,  50.0, kGateOff,          1, false },

  // Clean
  { "Room",             EPresetCategory::Clean,      kReverbRoom,   35.0, kLenBar1,    60.0, kTensionOff,    1,  50.0, kGateOff,          1, false },
  { "Plate",            EPresetCategory::Clean,      kReverbPlate,  40.0, kLenBar2,    50.0, kTensionOff,    1,  50.0, kGateOff,          1, false },
  { "Soft Hall",        EPresetCategory::Clean,      kReverbHall,   50.0, kLenBar2,    70.0, kTensionOff,    1,  50.0, kGateOff,          1, true  },
  { "Airy Lift",        EPresetCategory::Clean,      kReverbPlate,  60.0, kLenBar1,    80.0, kTensionFilter, 0,  30.0, kGateOff,          1, false },

  // Rhythmic
  { "Pulse Eighths",    EPresetCategory::Rhythmic,   kReverbRoom,   45.0, kLenBar2,    40.0, kTensionVolume, 1,  60.0, kGateOff,          1, false },
  { "Pulse Sixteenths", EPresetCategory::Rhythmic,   kReverbPlate,  45.0, kLenBar1,    30.0, kTensionVolume, 2,  70.0, kGateOff,          1, false },
  { "Last Bar Chop",    EPresetCategory::Rhythmic,   kReverbRoom,   40.0, kLenBar2,    50.0, kTensionOff,    1,  50.0, kGateLastBar,      1, false },
  { "Last Beat Stutter",EPresetCategory::Rhythmic,   kReverbPlate,  40.0, kLenBar1,    40.0, kTensionOff,    1,  50.0, kGateLastBeat,     2, false },

  // Cinematic
  { "Grand Hall",       EPresetCategory::Cinematic,  kReverbHall,   80.0, kLenBar4,    90.0, kTensionOff,    1,  50.0, kGateOff,          1, true  },
  { "Long Swell",       EPresetCategory::Cinematic,  kReverbHall,   70.0, kLenBar4,   100.0, kTensionFilter, 0,  40.0, kGateOff,          1, false },
  { "Dark Rise",        EPresetCategory::Cinematic,  kReverbHall,   90.0, kLenBar2,    85.0, kTensionFilter, 1,  70.0, kGateOff,          1, true  },
  { "Wide Impact",      EPresetCategory::Cinematic,  kReverbPlate,  75.0, kLenBar2,    60.0, kTensionOff,    1,  50.0, kGateLastTwoBeats, 0, true  },

  // Character
  { "Tight Plate",      EPresetCategory::Character,  kReverbPlate,  20.0, kRiserLen_2, 20.0, kTensionOff,    1,  50.0, kGateOff,          1, false },
  { "Matrix Original",  EPresetCategory::Character,  kReverbMatrix, 40.0, kRiserLen_4, 60.0, kTensionOff,    1,  50.0, kGateOff,          1, false },
  { "Breathing Filter", EPresetCategory::Character,  kReverbMatrix, 65.0, kLenBar2,    70.0, kTensionFilter, 2,  80.0, kGateOff,          1, false }
};

inline const char* GetPresetCategoryName(EPresetCategory category)
{
  switch (category)
  {
    case EPresetCategory::Clean:     return "Clean";
    case EPresetCategory::Rhythmic:  return "Rhythmic";
    case EPresetCategory::Cinematic: return "Cinematic";
    case EPresetCategory::Character: return "Character";
    case EPresetCategory::Init:
    default:                         return "Init";
  }
}

/// Menu/display text, e.g. "Clean - Room". "Init" has no category prefix.
inline std::string GetPresetDisplayName(int index)
{
  if (index < 0 || index >= kNumPresetEntries)
    return "Init";

  const Preset& preset = kPresets[index];
  if (preset.mCategory == EPresetCategory::Init)
    return preset.mName;

  return std::string(GetPresetCategoryName(preset.mCategory)) + " - " + preset.mName;
}

} // namespace rvrse
