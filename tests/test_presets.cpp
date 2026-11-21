#include <catch2/catch_test_macros.hpp>

#include "Presets.h"

#include <cstring>

TEST_CASE("Preset table is complete and names fit the host's 32-char param label", "[presets]")
{
  REQUIRE(rvrse::kNumPresetEntries == 16);

  for (int i = 0; i < rvrse::kNumPresetEntries; ++i)
  {
    const std::string name = rvrse::GetPresetDisplayName(i);
    REQUIRE_FALSE(name.empty());
    REQUIRE(name.size() < 32); // strcpy'd into a 32-byte buffer by iPlug2

    const auto& p = rvrse::kPresets[i];
    REQUIRE(p.mLengthIdx >= 0);
    REQUIRE(p.mLengthIdx < rvrse::kNumRiserLengthEntries);
    REQUIRE(p.mLushPct >= 0.0);
    REQUIRE(p.mLushPct <= 100.0);
    REQUIRE(p.mTensionRateIdx >= 0);
    REQUIRE(p.mTensionRateIdx < rvrse::kNumTensionRates);
    REQUIRE(p.mGateRateIdx >= 0);
    REQUIRE(p.mGateRateIdx < rvrse::kNumGateRates);
  }
}
