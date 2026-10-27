#pragma once

// HUD colour themes.
//
// The canopy uses colour to mean things, not to decorate: one hue for measured
// values, one for live/armed state, one for the ground the panels sit on, and a
// grey for words that name a state rather than report a number. A theme swaps
// those hues; it does not change which is which, so a pilot who has learned the
// layout on one theme can read any of them.
//
// The constraint they are all designed against is that this is drawn over live
// video of unknown brightness. That rules out dark accents, and it is why every
// theme keeps its type near-white rather than tinting it.

struct HudTheme {
    const char* name;
    float text[3];    // headline figures and row type
    float data[3];    // the measured tracks - cell level, link quality
    float accent[3];  // live state: armed rails, the sag track, VIDEO
    float ground[3];  // the wedge the panels are cut from
    float quiet[3];   // state words, track labels, anything qualifying a number
};

// How many themes exist, and the one in force. The index is clamped, so a
// settings file naming a theme that no longer exists falls back to the first
// rather than reading off the end of the table.
int              hud_theme_count();
const HudTheme&  hud_theme_at(int i);
const HudTheme&  hud_theme_current();
void             hud_theme_set(int i);
