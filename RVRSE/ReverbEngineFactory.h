#pragma once

/// @file ReverbEngineFactory.h
/// @brief Factory for the offline reverb engines.

#include "AirwindowsReverbEngine.h"
#include "FdnReverbEngine.h"

#include <memory>

namespace rvrse {

/// Original engine (Airwindows MatrixVerb). Kept for callers that predate characters.
inline std::unique_ptr<IReverbEngine> MakeActiveReverbEngine()
{
  return std::make_unique<AirwindowsReverbEngine>();
}

/// Engine for the requested character: Matrix → Airwindows, Room/Hall/Plate → FDN.
inline std::unique_ptr<IReverbEngine> MakeReverbEngine(EReverbCharacter character)
{
  if (character == kReverbMatrix)
    return std::make_unique<AirwindowsReverbEngine>();

  return std::make_unique<FdnReverbEngine>(character);
}

} // namespace rvrse
