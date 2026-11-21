#pragma once
/// @file GUIColors.h
/// Centralised brand color palette and shared style definitions for the RVRSE GUI.
/// Light silver-white + orange theme with navy header/waveform (personal build, after Scream). Names are semantic: "Dark" = main bg.

#include "IGraphicsStructs.h"

namespace rvrse {
namespace gui {

// ── Brand palette ──────────────────────────────────────────────────────
const IColor kColorGold          {255, 0xFF, 0x8A, 0x1F}; // #FF8A1F — primary accent (orange)
const IColor kColorBlue          {255, 0xD9, 0x5F, 0x0E}; // #D95F0E — alt-accent (deep orange)
const IColor kColorDark       {255, 0xD7, 0xDC, 0xE5}; // #D7DCE5 — main bg (silver white, Scream-like)
const IColor kColorDarkGrey   {255, 0xE6, 0xEA, 0xF1}; // #E6EAF1 — panel bg
const IColor kColorHighlight  {255, 0xFF, 0xE2, 0xC4}; // #FFE2C4 — playhead (on dark waveform)
const IColor kColorOnDark        {255, 0xE8, 0xEC, 0xF4}; // #E8ECF4 — text on navy header/footer/waveform
const IColor kColorOnDarkMuted   {255, 0x9A, 0xA4, 0xBA}; // #9AA4BA — secondary text on navy
const IColor kColorWhite         {255, 0xFF, 0xFF, 0xFF}; // #FFFFFF

// ── Derived surface colors ─────────────────────────────────────────────
const IColor kColorHeaderBg   {255, 0x14, 0x19, 0x2B}; // #14192B — navy header/footer
const IColor kColorWaveformBg {255, 0x0B, 0x0F, 0x1E}; // #0B0F1E — dark navy waveform panel
const IColor kColorSeparator  {255, 0x2A, 0x32, 0x4A}; // #2A324A — lines on dark panels
const IColor kColorKnobTrack  {255, 0xB4, 0xBC, 0xCB}; // #B4BCCB — knob/slider track on light panels

// ── Text colors ────────────────────────────────────────────────────────
const IColor kColorTextPrimary{255, 0x2A, 0x31, 0x45}; // #2A3145 — text on light panels
const IColor kColorTextSecondary{255, 0x7A, 0x84, 0x99}; // #7A8499 — secondary text on light panels
const IColor kColorTextMuted     {255, 0xA8, 0x98, 0x88}; // #A89888

// ── Knob accent variants ──────────────────────────────────────────────
const IColor kColorSteel      {255, 0x8A, 0x93, 0xA8}; // #8A93A8 — offline knobs (cool steel)

// ── Layout proportions (in pixels at default 1024×760) ─────────────────
constexpr float kHeaderHeight  = 60.f;
constexpr float kFooterHeight  = 55.f;
constexpr float kWaveformHeight = 170.f;
constexpr float kZoneGap       = 4.f;
constexpr float kRiserPanelPct = 0.6f; // riser panel = 60% of width

} // namespace gui
} // namespace rvrse
