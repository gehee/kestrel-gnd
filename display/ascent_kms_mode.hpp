#pragma once
#include "display/ascent_mode.hpp"
#include <drm_mode.h>
#include <cstdio>

namespace display {
inline drm_mode_modeinfo ascent_kms_mode(AscentMode mode) {
    drm_mode_modeinfo m{};
    const bool small=mode.height==720, fifty=mode.hz==50 || mode.hz==100;
    m.clock = small && mode.hz<=60 ? 74250 : 148500;
    m.hdisplay=mode.width(); m.vdisplay=mode.height;
    m.hsync_start=small?(fifty?1720:1390):(fifty?2448:2008);
    m.hsync_end=m.hsync_start+(small?40:44);
    m.htotal=small?(fifty?1980:1650):(fifty?2640:2200);
    m.vsync_start=small?725:1084; m.vsync_end=small?730:1089; m.vtotal=small?750:1125;
    m.vrefresh=mode.hz;
    m.flags=DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC;
    m.type=DRM_MODE_TYPE_DRIVER;
    snprintf(m.name,sizeof(m.name),"%dx%d",mode.width(),mode.height);
    return m;
}
inline bool same_ascent_timing(const drm_mode_modeinfo& edid, const drm_mode_modeinfo& candidate) {
    // Atomic clients receive picture-aspect metadata in flags. It is not a
    // timing difference; rejecting it hides CEA-only modes such as 720p50.
    return edid.clock==candidate.clock && edid.hdisplay==candidate.hdisplay &&
        edid.hsync_start==candidate.hsync_start && edid.hsync_end==candidate.hsync_end &&
        edid.htotal==candidate.htotal && edid.vdisplay==candidate.vdisplay &&
        edid.vsync_start==candidate.vsync_start && edid.vsync_end==candidate.vsync_end &&
        edid.vtotal==candidate.vtotal && edid.hskew==0 && edid.vscan<=1 &&
        (edid.flags & ~DRM_MODE_FLAG_PIC_AR_MASK)==candidate.flags;
}
} // namespace display
