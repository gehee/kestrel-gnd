# Which HAL backends and which features go into this build.
#
# fpvOS sets these from its Kconfig options (one set per board defconfig).
# A plain build of this tree with -DUSE_RKMPP=ON, which is all fpvOS passed
# before these options existed, still gets the VRX Pro's set, so an older
# fpvOS keeps building the same binary.
#
# The choices land in kestrel_gnd_features.h as 0/1 macros (KESTREL_HAL_*,
# KESTREL_BOARD_*, KESTREL_*). A feature that is off is meant to be left
# out of the binary, not hidden at run time; what a particular screen or
# system can do is a separate, run-time question (Caps).

if(USE_RKMPP)
    set(_kestrel_hal_default rockchip)
else()
    set(_kestrel_hal_default pc)
endif()
set(KESTREL_HAL ${_kestrel_hal_default} CACHE STRING
    "HAL backend set: rockchip (VRX Pro), pc (FFmpeg) or ar9311 (native Ascent)")
set_property(CACHE KESTREL_HAL PROPERTY STRINGS rockchip pc ar9311)

if(KESTREL_HAL STREQUAL "rockchip")
    # The Rockchip backends are MPP, RGA and the VOP2 display lock; the
    # build below still keys them off USE_RKMPP.
    set(USE_RKMPP ON CACHE BOOL "Build the Rockchip MPP backends" FORCE)
elseif(KESTREL_HAL STREQUAL "pc" OR KESTREL_HAL STREQUAL "ar9311")
    if(USE_RKMPP)
        message(FATAL_ERROR "KESTREL_HAL=${KESTREL_HAL} with USE_RKMPP=ON: pick one")
    endif()
else()
    message(FATAL_ERROR "KESTREL_HAL=${KESTREL_HAL}: expected rockchip, pc or ar9311")
endif()

# The box this build is for: its buttons, supply ADC, thermal zone and radio
# firmware (bsp/<box>.cpp), and its backends - display, HUD layer, decoders,
# screen encoder (platform/<box>.cpp). fpvOS builds one image per box, so the
# box is known here and only its files are compiled in.
if(KESTREL_HAL STREQUAL "rockchip")
    set(_kestrel_board_default vrxpro)
elseif(KESTREL_HAL STREQUAL "ar9311")
    set(_kestrel_board_default ascent)
else()
    set(_kestrel_board_default pc)
endif()
set(KESTREL_BOARD ${_kestrel_board_default} CACHE STRING
    "Box: vrxpro (Caddx), ascent (AR9311) or pc")
set_property(CACHE KESTREL_BOARD PROPERTY STRINGS vrxpro pc ascent)
if(KESTREL_BOARD STREQUAL "vrxpro")
    if(NOT KESTREL_HAL STREQUAL "rockchip")
        message(FATAL_ERROR "KESTREL_BOARD=vrxpro needs KESTREL_HAL=rockchip")
    endif()
    set(KESTREL_BOARD_SRC bsp/vrx_pro.cpp)
    set(KESTREL_PLATFORM_SRC platform/vrx_pro.cpp)
elseif(KESTREL_BOARD STREQUAL "ascent")
    if(NOT KESTREL_HAL STREQUAL "ar9311")
        message(FATAL_ERROR "KESTREL_BOARD=ascent needs KESTREL_HAL=ar9311")
    endif()
    set(KESTREL_BOARD_SRC bsp/ascent.cpp)
    set(KESTREL_PLATFORM_SRC platform/ascent.cpp)
elseif(KESTREL_BOARD STREQUAL "pc")
    if(NOT KESTREL_HAL STREQUAL "pc")
        message(FATAL_ERROR "KESTREL_BOARD=pc needs KESTREL_HAL=pc: the Rockchip backends are the VRX Pro's platform")
    endif()
    set(KESTREL_BOARD_SRC bsp/pc.cpp)
    set(KESTREL_PLATFORM_SRC platform/pc.cpp)
else()
    message(FATAL_ERROR "KESTREL_BOARD=${KESTREL_BOARD}: expected vrxpro, ascent or pc")
endif()
string(TOUPPER "${KESTREL_BOARD}" _kestrel_board_upper)
foreach(_board VRXPRO PC ASCENT)
    if(_board STREQUAL _kestrel_board_upper)
        set(KESTREL_BOARD_${_board} 1)
    else()
        set(KESTREL_BOARD_${_board} 0)
    endif()
endforeach()

# Features only the Rockchip backends have default to on there and off elsewhere.
if(KESTREL_HAL STREQUAL "rockchip")
    set(_kestrel_rk_only ON)
else()
    set(_kestrel_rk_only OFF)
endif()

set(_kestrel_gles_default ON)
if(KESTREL_HAL STREQUAL "ar9311")
    set(_kestrel_gles_default OFF)
endif()
option(KESTREL_GLES "Draw the HUD, menu and gallery with GLES" ${_kestrel_gles_default})
option(KESTREL_STREAM_DECODE "Per-slice stream decode and the per-half presenter (fpvOS MPP + kernel)" ${_kestrel_rk_only})
option(KESTREL_SCREEN_ENC    "Screen recording and the phone view (encoder + compositor)" ${_kestrel_rk_only})
option(KESTREL_WIFI          "WiFi access point for the phone view" ${_kestrel_gles_default})

# The LVGL software frontend is currently selected only by native Ascent.
if(NOT KESTREL_GLES AND NOT KESTREL_HAL STREQUAL "ar9311")
    message(FATAL_ERROR "KESTREL_GLES=OFF: there is no CPU canvas yet to draw the HUD with")
endif()

if(KESTREL_HAL STREQUAL "ar9311" AND KESTREL_WIFI)
    message(FATAL_ERROR "AR9311 has no WiFi/phone-view integration; set KESTREL_WIFI=OFF")
endif()

if(KESTREL_HAL STREQUAL "ar9311" AND KESTREL_GLES)
    message(FATAL_ERROR "AR9311 uses the LVGL software frontend; set KESTREL_GLES=OFF")
endif()

# The same rules fpvOS's Kconfig states with "depends on", checked here too
# so a hand-made build can't ask for a combination that can't work.
foreach(_feat KESTREL_STREAM_DECODE KESTREL_SCREEN_ENC)
    if(${_feat} AND NOT KESTREL_HAL STREQUAL "rockchip")
        message(FATAL_ERROR "${_feat} needs KESTREL_HAL=rockchip (no other backend has it yet)")
    endif()
endforeach()

string(TOUPPER "${KESTREL_HAL}" _kestrel_hal_upper)
foreach(_hal ROCKCHIP PC AR9311)
    if(_hal STREQUAL _kestrel_hal_upper)
        set(KESTREL_HAL_${_hal} 1)
    else()
        set(KESTREL_HAL_${_hal} 0)
    endif()
endforeach()

message(STATUS "kestrel-gnd HAL: ${KESTREL_HAL}, board: ${KESTREL_BOARD}")
foreach(_feat GLES STREAM_DECODE SCREEN_ENC WIFI)
    message(STATUS "  KESTREL_${_feat}: ${KESTREL_${_feat}}")
endforeach()
