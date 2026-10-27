# Which HAL backends and which features go into this build.
#
# fpvOS sets these from its Kconfig options (one set per board defconfig).
# A plain build of this tree with -DUSE_RKMPP=ON, which is all fpvOS passed
# before these options existed, still gets the VRX Pro's set, so an older
# fpvOS keeps building the same binary.
#
# The choices land in kestrel_gnd_features.h as 0/1 macros (KESTREL_HAL_*,
# KESTREL_FEAT_*). A feature that is off is meant to be left out of the
# binary, not hidden at run time; what a particular unit or screen can do
# is a separate, run-time question (Caps).

if(USE_RKMPP)
    set(_kestrel_hal_default rockchip)
else()
    set(_kestrel_hal_default pc)
endif()
set(KESTREL_HAL ${_kestrel_hal_default} CACHE STRING
    "HAL backend set: rockchip (VRX Pro) or pc (ffmpeg, for the host and sim)")
set_property(CACHE KESTREL_HAL PROPERTY STRINGS rockchip pc)

if(KESTREL_HAL STREQUAL "rockchip")
    # The Rockchip backends are MPP, RGA and the VOP2 display lock; the
    # build below still keys them off USE_RKMPP.
    set(USE_RKMPP ON CACHE BOOL "Build the Rockchip MPP backends" FORCE)
elseif(KESTREL_HAL STREQUAL "pc")
    if(USE_RKMPP)
        message(FATAL_ERROR "KESTREL_HAL=pc with USE_RKMPP=ON: pick one")
    endif()
elseif(KESTREL_HAL STREQUAL "ar9311")
    message(FATAL_ERROR "KESTREL_HAL=ar9311: the AR9311 backends (wave5 decoder, artosyn_vo display, CPU canvas) are not written yet")
else()
    message(FATAL_ERROR "KESTREL_HAL=${KESTREL_HAL}: expected rockchip or pc")
endif()

# Features only the Rockchip backends have default to on there and off elsewhere.
if(KESTREL_HAL STREQUAL "rockchip")
    set(_kestrel_rk_only ON)
else()
    set(_kestrel_rk_only OFF)
endif()

option(KESTREL_GLES          "Draw the HUD, menu and gallery with GLES" ON)
option(KESTREL_STREAM_DECODE "Per-slice stream decode and the per-half presenter (fpvOS MPP + kernel)" ${_kestrel_rk_only})
option(KESTREL_SCREEN_ENC    "Screen recording and the phone view (encoder + compositor)" ${_kestrel_rk_only})
option(KESTREL_WIFI          "WiFi access point for the phone view" ON)

# Without GLES nothing draws the HUD yet: the CPU canvas is still to come.
if(NOT KESTREL_GLES)
    message(FATAL_ERROR "KESTREL_GLES=OFF: there is no CPU canvas yet to draw the HUD with")
endif()

# The same rules fpvOS's Kconfig states with "depends on", checked here too
# so a hand-made build can't ask for a combination that can't work.
foreach(_feat KESTREL_STREAM_DECODE KESTREL_SCREEN_ENC)
    if(${_feat} AND NOT KESTREL_HAL STREQUAL "rockchip")
        message(FATAL_ERROR "${_feat} needs KESTREL_HAL=rockchip (no other backend has it yet)")
    endif()
endforeach()

string(TOUPPER "${KESTREL_HAL}" _kestrel_hal_upper)
foreach(_hal ROCKCHIP AR9311 PC)
    if(_hal STREQUAL _kestrel_hal_upper)
        set(KESTREL_HAL_${_hal} 1)
    else()
        set(KESTREL_HAL_${_hal} 0)
    endif()
endforeach()

message(STATUS "kestrel-gnd HAL: ${KESTREL_HAL}")
foreach(_feat GLES STREAM_DECODE SCREEN_ENC WIFI)
    message(STATUS "  KESTREL_${_feat}: ${KESTREL_${_feat}}")
endforeach()
