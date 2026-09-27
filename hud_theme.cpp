#include "hud_theme.hpp"

namespace {

// Ordered so the first is the one the HUD was designed in. Everything after it
// is a re-hue of the same roles, and each was checked against the two grounds
// that actually occur in flight: bright sky, and dark ground in shadow.
const HudTheme kThemes[] = {
    // The original. Cyan reads as instrumentation, lime as live, and the deep
    // blue ground is dark enough to lift thin type off bright video without
    // becoming a slab.
    { "KESTREL",
      { 1.00f, 1.00f, 1.00f },
      { 0.00f, 0.90f, 1.00f },
      { 0.70f, 1.00f, 0.00f },
      { 0.024f, 0.125f, 0.200f },
      { 0.55f, 0.62f, 0.70f } },

    // Warm. Amber for the readings and a hotter orange for live state - the two
    // are close in hue, so this leans harder on the tracks' position than on
    // their colour, and is the theme to avoid if you read state by hue alone.
    { "EMBER",
      { 1.00f, 0.96f, 0.90f },
      { 1.00f, 0.69f, 0.13f },
      { 1.00f, 0.42f, 0.10f },
      { 0.102f, 0.055f, 0.024f },
      { 0.70f, 0.63f, 0.56f } },

    // Cool. Pale blue readings under a cyan accent: the quietest of the set
    // over bright sky, where lime can glare.
    { "ICE",
      { 1.00f, 1.00f, 1.00f },
      { 0.56f, 0.83f, 1.00f },
      { 0.00f, 0.90f, 1.00f },
      { 0.024f, 0.094f, 0.169f },
      { 0.58f, 0.66f, 0.74f } },

    // No chroma at all. The most legible over anything, and the only theme that
    // survives a goggle with a colour-shifted panel - state is carried by
    // brightness alone, which is also what makes it the accessible choice.
    { "MONO",
      { 1.00f, 1.00f, 1.00f },
      { 0.81f, 0.85f, 0.87f },
      { 1.00f, 1.00f, 1.00f },
      { 0.063f, 0.075f, 0.078f },
      { 0.50f, 0.54f, 0.56f } },

    // Cold blue readings against a magenta live state - the widest hue
    // separation in the set, so state is unmistakable at a glance.
    { "VIOLET",
      { 1.00f, 1.00f, 1.00f },
      { 0.49f, 0.77f, 1.00f },
      { 0.83f, 0.42f, 1.00f },
      { 0.090f, 0.031f, 0.169f },
      { 0.62f, 0.58f, 0.72f } },
};

constexpr int kCount = (int)(sizeof(kThemes) / sizeof(kThemes[0]));
int g_current = 0;

}  // namespace

int hud_theme_count() { return kCount; }

const HudTheme& hud_theme_at(int i) {
    if (i < 0 || i >= kCount) i = 0;
    return kThemes[i];
}

const HudTheme& hud_theme_current() { return kThemes[g_current]; }

void hud_theme_set(int i) { g_current = (i < 0 || i >= kCount) ? 0 : i; }
