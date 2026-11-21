/// @file test_fdn_reverb.cpp
/// @brief Unit tests for the FDN reverb (Room / Hall / Plate) and the engine factory.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "FdnReverbEngine.h"
#include "ReverbEngineFactory.h"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace rvrse;

namespace {

constexpr double kSr = 48000.0;

struct Rendered
{
  std::vector<float> mL;
  std::vector<float> mR;
};

/// Render a unit impulse (both channels) through the engine for `seconds`.
Rendered renderImpulse(EReverbCharacter character, float lush, double seconds)
{
  const size_t frames = static_cast<size_t>(seconds * kSr);
  std::vector<float> inL(frames, 0.0f);
  std::vector<float> inR(frames, 0.0f);
  inL[0] = 1.0f;
  inR[0] = 1.0f;

  Rendered out;
  out.mL.assign(frames, 0.0f);
  out.mR.assign(frames, 0.0f);

  ReverbSettings settings;
  settings.mLush = lush;
  settings.mCharacter = character;

  FdnReverbEngine engine(character);
  engine.ProcessStereo(inL.data(), inR.data(), out.mL.data(), out.mR.data(), frames, kSr, settings);
  return out;
}

/// Energy (sum of squares) of a slice, both channels.
double sliceEnergy(const Rendered& r, size_t from, size_t to)
{
  double e = 0.0;
  for (size_t i = from; i < to && i < r.mL.size(); ++i)
    e += static_cast<double>(r.mL[i]) * r.mL[i] + static_cast<double>(r.mR[i]) * r.mR[i];
  return e;
}

bool allFinite(const Rendered& r)
{
  return std::all_of(r.mL.begin(), r.mL.end(), [](float v) { return std::isfinite(v); }) &&
         std::all_of(r.mR.begin(), r.mR.end(), [](float v) { return std::isfinite(v); });
}

} // namespace

TEST_CASE("FDN: output is finite for every character", "[fdn]")
{
  for (const EReverbCharacter c : { kReverbRoom, kReverbHall, kReverbPlate })
  {
    for (const float lush : { 0.2f, 0.5f, 1.0f })
    {
      const Rendered r = renderImpulse(c, lush, 6.0);
      REQUIRE(allFinite(r));
    }
  }
}

TEST_CASE("FDN: Lush at zero passes the input through untouched", "[fdn]")
{
  const Rendered r = renderImpulse(kReverbHall, 0.0f, 0.5);
  REQUIRE(r.mL[0] == Catch::Approx(1.0f));
  REQUIRE(sliceEnergy(r, 1, r.mL.size()) == Catch::Approx(0.0).margin(1e-12));
}

TEST_CASE("FDN: the tail rings and then decays (stable feedback)", "[fdn]")
{
  for (const EReverbCharacter c : { kReverbRoom, kReverbHall, kReverbPlate })
  {
    const Rendered r = renderImpulse(c, 1.0f, 8.0);

    // Skip the dry impulse at frame 0.
    const double early = sliceEnergy(r, 100, static_cast<size_t>(0.5 * kSr));
    const double mid = sliceEnergy(r, static_cast<size_t>(2.0 * kSr), static_cast<size_t>(2.5 * kSr));
    const double late = sliceEnergy(r, static_cast<size_t>(7.0 * kSr), static_cast<size_t>(7.5 * kSr));

    REQUIRE(early > 0.0);                 // there is a reverb tail
    REQUIRE(mid < early);                 // it decays
    REQUIRE(late < mid + 1e-12);          // and keeps decaying, never grows
    REQUIRE(late < early * 1e-3);         // ends well below the early energy
  }
}

TEST_CASE("FDN: Hall rings longer than Room", "[fdn]")
{
  const Rendered room = renderImpulse(kReverbRoom, 0.8f, 6.0);
  const Rendered hall = renderImpulse(kReverbHall, 0.8f, 6.0);

  const size_t from = static_cast<size_t>(1.5 * kSr);
  const size_t to = static_cast<size_t>(2.0 * kSr);
  REQUIRE(sliceEnergy(hall, from, to) > sliceEnergy(room, from, to));
}

TEST_CASE("FDN: higher Lush gives a longer tail", "[fdn]")
{
  const Rendered lowLush = renderImpulse(kReverbHall, 0.3f, 6.0);
  const Rendered highLush = renderImpulse(kReverbHall, 1.0f, 6.0);

  const size_t from = static_cast<size_t>(2.0 * kSr);
  const size_t to = static_cast<size_t>(2.5 * kSr);
  REQUIRE(sliceEnergy(highLush, from, to) > sliceEnergy(lowLush, from, to));
}

TEST_CASE("FDN: characters sound different", "[fdn]")
{
  const Rendered room = renderImpulse(kReverbRoom, 0.7f, 2.0);
  const Rendered hall = renderImpulse(kReverbHall, 0.7f, 2.0);
  const Rendered plate = renderImpulse(kReverbPlate, 0.7f, 2.0);

  REQUIRE(room.mL != hall.mL);
  REQUIRE(hall.mL != plate.mL);
  REQUIRE(room.mL != plate.mL);
}

TEST_CASE("FDN: left and right are decorrelated", "[fdn]")
{
  const Rendered r = renderImpulse(kReverbPlate, 0.8f, 1.0);

  double diff = 0.0;
  for (size_t i = 100; i < r.mL.size(); ++i)
    diff += std::abs(static_cast<double>(r.mL[i]) - r.mR[i]);

  REQUIRE(diff > 1e-3);
}

TEST_CASE("FDN: rendering is deterministic", "[fdn]")
{
  const Rendered a = renderImpulse(kReverbHall, 0.6f, 1.0);
  const Rendered b = renderImpulse(kReverbHall, 0.6f, 1.0);
  REQUIRE(a.mL == b.mL);
  REQUIRE(a.mR == b.mR);
}

TEST_CASE("Factory: picks the engine for each character", "[fdn][factory]")
{
  REQUIRE(std::string(MakeReverbEngine(kReverbMatrix)->GetName()) == "AirwindowsMatrixVerb");
  REQUIRE(std::string(MakeReverbEngine(kReverbRoom)->GetName()) == "RvrseFDN");
  REQUIRE(std::string(MakeReverbEngine(kReverbHall)->GetName()) == "RvrseFDN");
  REQUIRE(std::string(MakeReverbEngine(kReverbPlate)->GetName()) == "RvrseFDN");
  REQUIRE(std::string(MakeActiveReverbEngine()->GetName()) == "AirwindowsMatrixVerb");
}
