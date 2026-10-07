cmake_minimum_required(VERSION 3.13.0)

project(kestrel-gnd VERSION 1.0)
# The version as people read it, pre-release suffix included. project() only
# takes numbers, so the label is kept beside it and is what the binary reports.
# A release build (fpvOS's release.sh) passes its own, so the menu always
# shows the release it came from.
set(KESTREL_GND_VERSION_LABEL "2026.10.beta2" CACHE STRING "Version shown in the menu and logs")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_options("-Wno-address-of-packed-member")
set(TARGET kestrel-gnd)
set(SOURCE_FILES
        drm/drm.c
        utils/copy_utils.c
        utils/screen_id.c
        utils/scheduling_helper.cpp
        utils/mpp_encoder.cpp
        utils/h264_cfr.cpp
        utils/prof.cpp
        utils/ltrace.cpp
        queue/Queue.cpp
        queue/Msg.cpp
        dvr.cpp
        dvr_library.cpp
        gallery.cpp
        gallery_stats.cpp
        osd_gallery.cpp
        osd_screen.cpp
        osd_channel.cpp
        zones.cpp
        zones_data.cpp
        fpv_rules.cpp
        dvr_recover.cpp
        screen_tap.cpp
        rga_compositor.cpp
        vdec/vdec_ffmpeg.cpp
        renderer.cpp
        drm.cpp
				osd.cpp
        hud_canopy.cpp
        hud_lock.cpp
        hud_theme.cpp
        hud_menu.cpp
        vec_icons.cpp
        bg_video.cpp
        betaflight_glyphs.cpp
        settings.cpp
        msp_osd.cpp
        msp_telem.cpp
        stab/imu_stream.cpp
        webstream.cpp
        wifi_ap.cpp
        artosyn/ar8030_source.cpp
        artosyn/bb_watchdog.cpp
        artosyn/ar8030_sky.cpp
        artosyn/bb_sdk_shim.cpp
        main.cpp)

if(USE_RKMPP)
    message(STATUS "Building with rkmpp")
    list(APPEND SOURCE_FILES vdec/vdec_rk.cpp)
    add_definitions(-DUSE_RKMPP)
endif()

find_package(PkgConfig REQUIRED)
set(VAAPI_PKGS)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "i.86|x86_64|amd64")
    set(VAAPI_PKGS libva libva-drm)
    add_definitions(-DUSE_VAAPI)
endif()

pkg_check_modules(DEPS REQUIRED 
    libavcodec
    libavformat
    libavutil
    libswscale
    gbm
    egl
    glesv2
    libdrm
    cairo
    ${VAAPI_PKGS}
)


include_directories(${DEPS_INCLUDE_DIRS})
link_directories(${DEPS_LIBRARY_DIRS})


include_directories(.)

if(USE_RKMPP)
    # ${CMAKE_SYSROOT}/usr/include holds rockchip/ subdir with rk_mpi.h etc.
    include_directories(${CMAKE_SYSROOT}/usr/include)
endif()

add_compile_options(${FFMPEG_CFLAGS_OTHER} ${DEPS_CFLAGS_OTHER})

add_executable(${TARGET} ${SOURCE_FILES})

target_link_libraries(${TARGET} ${DEPS_LIBRARIES} pthread m)

# AR8030 baseband client (libar8030_client.so from the VRX firmware).
# Not shipped by any SDK package - it is imported from the device, so look for
# it in the sysroot and let -DAR8030_LIB=... override.
if(NOT AR8030_LIB)
    find_library(AR8030_LIB NAMES ar8030_client
                 PATHS ${CMAKE_SYSROOT}/usr/lib ${CMAKE_SYSROOT}/lib)
endif()
if(AR8030_LIB)
    message(STATUS "AR8030 client library: ${AR8030_LIB}")
    target_link_libraries(${TARGET} ${AR8030_LIB} dl)
    # artosyn/bb_sdk_shim.cpp overrides get_bb_ioctl_cmdiptlen() inside
    # libar8030_client.so. That only works if the executable exports its
    # dynamic symbols, so this flag is load-bearing, not a debug convenience.
    set_target_properties(${TARGET} PROPERTIES ENABLE_EXPORTS ON)
    target_link_options(${TARGET} PRIVATE -rdynamic)
else()
    message(WARNING "libar8030_client.so not found - --link=ar8030 will not work")
endif()

if(USE_RKMPP)
    target_link_libraries(${TARGET} rockchip_mpp)
endif()

# Rockchip RGA (librga.so.2 from the goggle's stock firmware, installed by
# fpvOS's fpvos-vendor-libs; its headers are in include/rga). The screen
# recorder composites with it; without it, screen recording uses writeback.
if(USE_RKMPP AND NOT RGA_LIB)
    find_library(RGA_LIB NAMES rga librga.so.2
                 PATHS ${CMAKE_SYSROOT}/usr/lib ${CMAKE_SYSROOT}/lib)
endif()
if(USE_RKMPP AND RGA_LIB)
    message(STATUS "RGA library: ${RGA_LIB}")
    target_compile_definitions(${TARGET} PRIVATE USE_RGA)
    target_link_libraries(${TARGET} ${RGA_LIB})
else()
    message(STATUS "librga not found - screen recording falls back to writeback")
endif()

# kestrel-splash — tiny standalone DRM/cairo boot splash (no GL/MPP).
# Links only libdrm + cairo (both already in DEPS).
add_executable(kestrel-splash splash.c utils/screen_id.c)
target_link_libraries(kestrel-splash ${DEPS_LIBRARIES} m)

# kestrel-gpio-joystick — GPIO/ADC joystick → uinput mapper for OSD menu
# navigation (libc only; previously built ad hoc outside the build system).
add_executable(kestrel-gpio-joystick utils/gpio_joystick.c)

if(CMAKE_BUILD_TYPE MATCHES "Debug")
  set(
    CMAKE_C_FLAGS
    "${CMAKE_C_FLAGS} -g -fsanitize=undefined -fsanitize=address"
  )
  target_link_options(${TARGET}
    BEFORE PUBLIC -fsanitize=undefined PUBLIC -fsanitize=address
  )
endif()

# set(
#     CMAKE_C_FLAGS
#     "${CMAKE_C_FLAGS} -O3 -ffast-math -ftree-vectorize"
#   )



# Version stamping. Both the build time and the git hash used to be computed
# here, at configure time, and then cached in the build directory - so every
# binary reported whatever the tree looked like when cmake last ran, which
# could be many commits ago. They are generated at build time now instead.
set(KESTREL_GND_STAMP ${PROJECT_BINARY_DIR}/kestrel_gnd_version.cpp)
# A build from a source tarball has no .git to describe. Packagers that know
# the revision (fpvOS's Buildroot package) pass it here instead.
set(KESTREL_GND_GIT_HASH_FALLBACK "" CACHE STRING "Revision to report when the source has no git metadata")
add_custom_target(kestrel-gnd-stamp
    COMMAND ${CMAKE_COMMAND}
            -DPROJECT_VERSION=${KESTREL_GND_VERSION_LABEL}
            -DSRC_DIR=${CMAKE_SOURCE_DIR}
            -DGIT_HASH_FALLBACK=${KESTREL_GND_GIT_HASH_FALLBACK}
            -DIN_FILE=${CMAKE_CURRENT_SOURCE_DIR}/kestrel_gnd_version.cpp.in
            -DOUT_FILE=${KESTREL_GND_STAMP}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/stamp_version.cmake
    BYPRODUCTS ${KESTREL_GND_STAMP}
    COMMENT "Stamping version (git hash + build time)"
    VERBATIM
)
# Produce it once now as well, so the file exists for the first configure.
execute_process(COMMAND ${CMAKE_COMMAND}
    -DPROJECT_VERSION=${KESTREL_GND_VERSION_LABEL}
    -DSRC_DIR=${CMAKE_SOURCE_DIR}
    -DGIT_HASH_FALLBACK=${KESTREL_GND_GIT_HASH_FALLBACK}
    -DIN_FILE=${CMAKE_CURRENT_SOURCE_DIR}/kestrel_gnd_version.cpp.in
    -DOUT_FILE=${KESTREL_GND_STAMP}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/stamp_version.cmake)
set_source_files_properties(${KESTREL_GND_STAMP} PROPERTIES GENERATED TRUE)
target_sources(${TARGET} PRIVATE ${KESTREL_GND_STAMP})
add_dependencies(${TARGET} kestrel-gnd-stamp)

configure_file(kestrel_gnd_config.h.in kestrel_gnd_config.h)
target_include_directories(${TARGET} PUBLIC "${PROJECT_BINARY_DIR}")

# Kept commented for years because kestrel was always deployed by copying the
# binary onto the goggle by hand. Buildroot installs packages with `make
# install`, so without this the package builds and installs nothing - the image
# comes out with no kestrel-gnd at all, and the build still exits 0.
include(GNUInstallDirs)
install(TARGETS ${TARGET} kestrel-splash
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
