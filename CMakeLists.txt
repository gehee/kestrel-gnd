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
include(cmake/features.cmake)
set(TARGET kestrel-gnd)
set(SOURCE_FILES
        drm/drm.c
        utils/copy_utils.c
        utils/screen_id.c
        utils/qrcodegen.c
        utils/scheduling_helper.cpp
        utils/h264_cfr.cpp
        utils/hevc_repair.cpp
        utils/prof.cpp
        utils/sd_log.cpp
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
        vdec/vdec_ffmpeg.cpp
        renderer.cpp
        display_lock.cpp
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
        hal/probe.cpp
        render/gl_context.cpp
        render/canvas_gles.cpp
        hud/dispatch.cpp
        hud/state.cpp
        hal/decoder.cpp
        hal/screen_out.cpp
        hal/frame_length.cpp
        hal/plane_rules.cpp
        bsp/vrx_pro.cpp
        bsp/pc.cpp
        main.cpp)

if(USE_RKMPP)
    message(STATUS "Building with rkmpp")
    list(APPEND SOURCE_FILES hal/rockchip/vdec_rk.cpp hal/rockchip/mpp_encoder.cpp
                             hal/rockchip/rga_compositor.cpp hal/rockchip/vop2_frame_length.cpp)
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
configure_file(kestrel_gnd_features.h.in kestrel_gnd_features.h)
target_include_directories(${TARGET} PUBLIC "${PROJECT_BINARY_DIR}")

# Kept commented for years because kestrel was always deployed by copying the
# binary onto the goggle by hand. Buildroot installs packages with `make
# install`, so without this the package builds and installs nothing - the image
# comes out with no kestrel-gnd at all, and the build still exits 0.
include(GNUInstallDirs)
install(TARGETS ${TARGET} kestrel-splash
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# Unit tests: cmake -DKESTREL_TESTS=ON, then ctest. Each test is one program
# that exits non-zero on a failure, so a cross build's binaries also run on
# the unit as they are.
option(KESTREL_TESTS "Build the unit tests (run them with ctest)" OFF)
if(KESTREL_TESTS)
    enable_testing()
    function(kestrel_test name)
        add_executable(${name} tools/test/${name}.cpp ${ARGN})
        target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/stab ${PROJECT_BINARY_DIR})
        target_link_libraries(${name} pthread)
        add_test(NAME ${name} COMMAND ${name})
    endfunction()
    kestrel_test(bsp_test bsp/vrx_pro.cpp bsp/pc.cpp)
    kestrel_test(probe_test hal/probe.cpp bsp/vrx_pro.cpp bsp/pc.cpp)
    set(_hal_test_srcs hal/screen_out.cpp hal/frame_length.cpp hal/plane_rules.cpp hal/rockchip/vop2_frame_length.cpp)
    if(KESTREL_HAL STREQUAL "rockchip")
        list(APPEND _hal_test_srcs hal/rockchip/mpp_encoder.cpp hal/rockchip/rga_compositor.cpp)
    endif()
    kestrel_test(hal_test ${_hal_test_srcs})
    if(KESTREL_HAL STREQUAL "rockchip")
        target_link_libraries(hal_test rockchip_mpp)
        if(RGA_LIB)
            target_compile_definitions(hal_test PRIVATE USE_RGA)
            target_link_libraries(hal_test ${RGA_LIB})
        endif()
    endif()
    kestrel_test(canvas_test)
    kestrel_test(air_clock_test)
    # utils/sd_log.cpp first: the order in which the old exit crash showed
    # every time (see the test's header).
    add_executable(sd_log_test utils/sd_log.cpp tools/test/sd_log_test.cpp)
    target_include_directories(sd_log_test PRIVATE ${CMAKE_SOURCE_DIR} ${PROJECT_BINARY_DIR})
    target_link_libraries(sd_log_test pthread)
    add_test(NAME sd_log_test COMMAND sd_log_test)
    kestrel_test(imu_stream_test stab/imu_stream.cpp)
    kestrel_test(msp_telem_test msp_telem.cpp)
    kestrel_test(msp_osd_hide_test msp_osd.cpp msp_telem.cpp stab/imu_stream.cpp)
    kestrel_test(msp_osd_lq_test msp_osd.cpp msp_telem.cpp stab/imu_stream.cpp)
endif()
