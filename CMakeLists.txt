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

configure_file(kestrel_gnd_config.h.in kestrel_gnd_config.h)
configure_file(kestrel_gnd_features.h.in kestrel_gnd_features.h)

if(USE_RKMPP)
    message(STATUS "Building with rkmpp")
    add_definitions(-DUSE_RKMPP)
endif()

# The system libraries, each linked by the parts that call it.
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED libavcodec libavformat libavutil libswscale)
pkg_check_modules(GFX REQUIRED gbm egl glesv2)
pkg_check_modules(DRM REQUIRED libdrm)
pkg_check_modules(CAIRO REQUIRED cairo)
set(_pkgs FFMPEG GFX DRM CAIRO)
set(_va_libs "")
if(CMAKE_SYSTEM_PROCESSOR MATCHES "i.86|x86_64|amd64")
    pkg_check_modules(VAAPI REQUIRED libva libva-drm)
    add_definitions(-DUSE_VAAPI)
    list(APPEND _pkgs VAAPI)
    set(_va_libs ${VAAPI_LIBRARIES})
endif()
foreach(_pkg IN LISTS _pkgs)
    include_directories(${${_pkg}_INCLUDE_DIRS})
    link_directories(${${_pkg}_LIBRARY_DIRS})
    add_compile_options(${${_pkg}_CFLAGS_OTHER})
endforeach()

# Includes are from the source root ("radio/model.hpp"), and the generated
# headers (kestrel_gnd_features.h, kestrel_gnd_config.h) from the build's.
include_directories(. ${PROJECT_BINARY_DIR})

if(USE_RKMPP)
    # ${CMAKE_SYSROOT}/usr/include holds rockchip/ subdir with rk_mpi.h etc.
    include_directories(${CMAKE_SYSROOT}/usr/include)
endif()

# AR8030 baseband client (libar8030_client.so from the VRX firmware).
# Not shipped by any SDK package - it is imported from the device, so look for
# it in the sysroot and let -DAR8030_LIB=... override.
if(NOT AR8030_LIB)
    find_library(AR8030_LIB NAMES ar8030_client
                 PATHS ${CMAKE_SYSROOT}/usr/lib ${CMAKE_SYSROOT}/lib)
endif()
if(AR8030_LIB)
    message(STATUS "AR8030 client library: ${AR8030_LIB}")
    set(_ar8030_libs ${AR8030_LIB} dl)
else()
    message(WARNING "libar8030_client.so not found - --link=ar8030 will not work")
    set(_ar8030_libs "")
endif()

# Rockchip RGA (librga.so.2 from the goggle's stock firmware, installed by
# fpvOS's fpvos-vendor-libs; its headers are in include/rga). The screen
# recorder composites with it; without it, screen recording uses writeback.
set(_rk_libs "")
if(USE_RKMPP)
    set(_rk_libs rockchip_mpp)
    if(NOT RGA_LIB)
        find_library(RGA_LIB NAMES rga librga.so.2
                     PATHS ${CMAKE_SYSROOT}/usr/lib ${CMAKE_SYSROOT}/lib)
    endif()
endif()
if(USE_RKMPP AND RGA_LIB)
    message(STATUS "RGA library: ${RGA_LIB}")
    list(APPEND _rk_libs ${RGA_LIB})
else()
    message(STATUS "librga not found - screen recording falls back to writeback")
endif()

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
# Its own library, under the HUD (which shows the version) and main.
add_library(kg_version STATIC ${KESTREL_GND_STAMP})
add_dependencies(kg_version kestrel-gnd-stamp)

# kestrel_module(<part> FOLDERS <folder>... [DEPS <part>...] [SOURCES <file>...]
#                [LIBS <system library>...])
#
# One part of kestrel-gnd: the library kg_<part> of its SOURCES (an interface
# library when it has none), linked to the parts it is built on and to the
# system libraries it calls. Its line goes into KESTREL_LAYERS: the `layers`
# test fails on an include from a part that is not below it, and on parts
# built on each other in a circle (tools/check_layers.cmake).
set(KESTREL_LAYERS "")
function(kestrel_module name)
    cmake_parse_arguments(M "" "" "FOLDERS;DEPS;SOURCES;LIBS" ${ARGN})
    string(REPLACE ";" " " _folders "${M_FOLDERS}")
    string(REPLACE ";" " " _deps "${M_DEPS}")
    set(KESTREL_LAYERS ${KESTREL_LAYERS} "${name}: ${_folders} | ${_deps}" PARENT_SCOPE)
    set(_libs "")
    foreach(_d IN LISTS M_DEPS)
        list(APPEND _libs kg_${_d})
    endforeach()
    if(M_SOURCES)
        add_library(kg_${name} STATIC ${M_SOURCES})
        target_link_libraries(kg_${name} PUBLIC ${_libs} ${M_LIBS})
    else()
        add_library(kg_${name} INTERFACE)
        target_link_libraries(kg_${name} INTERFACE ${_libs} ${M_LIBS})
    endif()
endfunction()

# The parts, from the bottom up.
kestrel_module(third_party FOLDERS third_party include
    SOURCES third_party/qrcodegen.c)
kestrel_module(utils FOLDERS utils DEPS third_party
    SOURCES utils/Queue.cpp utils/Msg.cpp)
kestrel_module(base FOLDERS base DEPS utils
    SOURCES base/scheduling_helper.cpp base/prof.cpp base/sd_log.cpp base/frame_log.cpp
            base/ltrace.cpp base/video_pool.cpp base/restart.cpp base/settings.cpp base/shell.cpp
            base/frame_mode.cpp
    LIBS pthread m)
set(_hal_srcs hal/ffmpeg/vdec_ffmpeg.cpp hal/frame_length.cpp hal/plane_rules.cpp hal/wifi_ap.cpp
              hal/rockchip/vop2_frame_length.cpp)
if(USE_RKMPP)
    list(APPEND _hal_srcs hal/rockchip/vdec_rk.cpp hal/rockchip/mpp_encoder.cpp
                          hal/rockchip/rga_compositor.cpp)
endif()
kestrel_module(hal FOLDERS hal DEPS base
    SOURCES ${_hal_srcs}
    LIBS ${FFMPEG_LIBRARIES} ${DRM_LIBRARIES} ${_va_libs} ${_rk_libs})
if(USE_RKMPP AND RGA_LIB)
    target_compile_definitions(kg_hal PRIVATE USE_RGA)
endif()
kestrel_module(bsp FOLDERS bsp DEPS base
    SOURCES bsp/board.cpp ${KESTREL_BOARD_SRC})
# The parts of the system as the rest sees them (interfaces, no code).
kestrel_module(system FOLDERS system DEPS base)
kestrel_module(region FOLDERS region DEPS base
    SOURCES region/zones.cpp region/zones_data.cpp region/fpv_rules.cpp)
kestrel_module(render FOLDERS render DEPS base
    SOURCES render/gl_context.cpp render/canvas_gles.cpp render/gles_2d.cpp render/text_cache.cpp
    LIBS ${GFX_LIBRARIES} ${CAIRO_LIBRARIES})
# radio/bb_sdk_shim.cpp is not here: it overrides a function inside
# libar8030_client.so, nothing of ours calls it, and a library would leave it
# out - it is built into each program that runs the radio (below).
kestrel_module(radio FOLDERS radio DEPS hal region system
    SOURCES radio/ar8030_link.cpp radio/ar8030_link_bb.cpp radio/ar8030_link_bind.cpp radio/ar8030_link_air.cpp
            radio/ar8030_link_rf.cpp radio/ar8030_link_stats.cpp radio/ar8030_link_video.cpp radio/bb_watchdog.cpp radio/ar8030_sky.cpp radio/model.cpp
            radio/air_imu.cpp radio/streams.cpp radio/video_out.cpp
    LIBS ${_ar8030_libs})
kestrel_module(motion FOLDERS motion DEPS system
    SOURCES motion/imu_stream.cpp motion/imu_reader.cpp)
kestrel_module(msp FOLDERS msp DEPS motion
    SOURCES msp/betaflight_glyphs.cpp msp/msp_osd.cpp msp/msp_telem.cpp
    LIBS ${CAIRO_LIBRARIES})
kestrel_module(display FOLDERS display DEPS hal
    SOURCES display/kms/drm.c display/screen_id.c display/screen_tap.cpp display/renderer.cpp
            display/display_lock.cpp display/drm.cpp
    LIBS ${DRM_LIBRARIES} ${FFMPEG_LIBRARIES})
# The box's backends, each picked here: a file per box (KESTREL_BOARD), and
# what the boxes with KMS and GLES share.
kestrel_module(platform FOLDERS platform DEPS hal bsp render display
    SOURCES platform/caps.cpp platform/kms_gles.cpp platform/gles_hud_layer.cpp ${KESTREL_PLATFORM_SRC}
    LIBS ${GFX_LIBRARIES} ${DRM_LIBRARIES})
kestrel_module(dvr FOLDERS dvr DEPS platform
    SOURCES dvr/h264_cfr.cpp dvr/hevc_repair.cpp dvr/dvr.cpp dvr/dvr_sink.cpp
            dvr/dvr_library.cpp dvr/dvr_recover.cpp)
kestrel_module(hud FOLDERS hud icons DEPS bsp render msp display platform dvr region system
    SOURCES hud/gallery.cpp hud/gallery_stats.cpp hud/osd_gallery.cpp hud/osd_screen.cpp
            hud/osd_images.cpp hud/osd_frame.cpp hud/osd_video.cpp hud/osd_radio.cpp hud/osd_menu_rows.cpp hud/osd_keys.cpp
            hud/hud_idle.cpp hud/hud_strip.cpp hud/hud_arena.cpp hud/hud_betaflight.cpp
            hud/osd_channel.cpp hud/osd.cpp hud/hud_canopy.cpp hud/hud_lock.cpp hud/hud_theme.cpp
            hud/hud_menu.cpp hud/vec_icons.cpp hud/bg_video.cpp hud/link_view.cpp
    LIBS kg_version ${GFX_LIBRARIES} ${CAIRO_LIBRARIES} ${DRM_LIBRARIES} ${FFMPEG_LIBRARIES})
kestrel_module(web FOLDERS web DEPS hud
    SOURCES web/webstream.cpp)
kestrel_module(app FOLDERS app DEPS web radio)
kestrel_module(splash FOLDERS splash DEPS display)

# kestrel-gnd: main and the wiring, on every part.
add_executable(${TARGET} app/main.cpp app/options.cpp app/signals.cpp app/input.cpp app/wiring.cpp
                         radio/bb_sdk_shim.cpp)
target_link_libraries(${TARGET} kg_app kg_version ${GFX_LIBRARIES} ${_ar8030_libs})
if(AR8030_LIB)
    # radio/bb_sdk_shim.cpp overrides get_bb_ioctl_cmdiptlen() inside
    # libar8030_client.so. That only works if the executable exports its
    # dynamic symbols, so this flag is load-bearing, not a debug convenience.
    set_target_properties(${TARGET} PROPERTIES ENABLE_EXPORTS ON)
    target_link_options(${TARGET} PRIVATE -rdynamic)
endif()

# kestrel-splash — tiny standalone DRM/cairo boot splash (no GL/MPP).
# Links only libdrm + cairo.
add_executable(kestrel-splash splash/splash.c display/screen_id.c)
target_link_libraries(kestrel-splash ${DRM_LIBRARIES} ${CAIRO_LIBRARIES} m)

# kestrel-gpio-joystick — GPIO/ADC joystick → uinput mapper for OSD menu
# navigation (libc only; previously built ad hoc outside the build system).
add_executable(kestrel-gpio-joystick bsp/gpio_joystick.c)

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
    # Each part includes only from the parts it is built on (KESTREL_LAYERS).
    string(REPLACE ";" "\n" _layers "${KESTREL_LAYERS}")
    file(WRITE ${PROJECT_BINARY_DIR}/layers.txt "${_layers}\n")
    add_test(NAME layers COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR}
             -DLAYERS=${PROJECT_BINARY_DIR}/layers.txt -DNM=${CMAKE_NM} -DLIBDIR=${PROJECT_BINARY_DIR}
             -P ${CMAKE_SOURCE_DIR}/tools/check_layers.cmake)
    # Each key of the settings file declared once (tools/check_settings.cmake).
    add_test(NAME settings_keys COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR}
             -P ${CMAKE_SOURCE_DIR}/tools/check_settings.cmake)
    # The radio's video path on a recorded stream, with a fake decoder (not a
    # ctest: it takes the stream as its argument) - see the source. A host
    # check: the PC's HAL.
    if(NOT KESTREL_HAL STREQUAL "rockchip")
        add_executable(video_replay_check tools/test/video_replay_check.cpp radio/bb_sdk_shim.cpp)
        target_link_libraries(video_replay_check kg_radio kg_dvr)
        if(AR8030_LIB)
            set_target_properties(video_replay_check PROPERTIES ENABLE_EXPORTS ON)
        endif()
    endif()
    # The HUD's screens drawn on this machine's GPU, to compare before and
    # after a change to how they are drawn (not a ctest: see the source).
    if(NOT KESTREL_HAL STREQUAL "rockchip")
        add_executable(hud_shot tools/test/hud_shot.cpp)
        target_link_libraries(hud_shot kg_hud kg_radio)   # the camera's modes (radio/ar8030_sky)
    endif()
    # kestrel_test(<name> <part>... [SOURCES <file>...]): tools/test/<name>.cpp,
    # linked to the parts it tests.
    function(kestrel_test name)
        cmake_parse_arguments(T "" "" "SOURCES" ${ARGN})
        add_executable(${name} tools/test/${name}.cpp ${T_SOURCES})
        foreach(_part IN LISTS T_UNPARSED_ARGUMENTS)
            target_link_libraries(${name} kg_${_part})
        endforeach()
        target_link_libraries(${name} pthread)
        add_test(NAME ${name} COMMAND ${name})
    endfunction()
    # Both boxes' facts, whichever this build is for.
    kestrel_test(bsp_test base SOURCES bsp/board.cpp bsp/vrx_pro.cpp bsp/pc.cpp)
    kestrel_test(hal_test hal)
    kestrel_test(canvas_test render)
    kestrel_test(platform_test platform)
    kestrel_test(air_clock_test radio)
    kestrel_test(gallery_stats_test hud)
    kestrel_test(link_view_test hud)
    kestrel_test(bus_test base)
    kestrel_test(settings_test base)
    kestrel_test(hevc_repair_test dvr)
    kestrel_test(model_test radio)
    # base/sd_log.cpp first: the order in which the old exit crash showed
    # every time (see the test's header).
    add_executable(sd_log_test base/sd_log.cpp tools/test/sd_log_test.cpp)
    target_link_libraries(sd_log_test pthread)
    add_test(NAME sd_log_test COMMAND sd_log_test)
    kestrel_test(imu_stream_test motion radio)
    kestrel_test(msp_telem_test msp)
    kestrel_test(msp_osd_hide_test msp)
    kestrel_test(msp_osd_lq_test msp)
endif()

# Benchmarks: cmake -DKESTREL_BENCH=ON. bus_bench replays a recorded stream
# through today's video and state paths and through base/bus.hpp's, and prints
# what each costs the radio thread (see its header).
option(KESTREL_BENCH "Build the benchmarks" OFF)
if(KESTREL_BENCH)
    add_executable(bus_bench tools/bench/bus_bench.cpp)
    target_link_libraries(bus_bench pthread)
endif()
