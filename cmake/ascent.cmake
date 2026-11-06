# Native Ascent: no vendor daemon, EGL/GBM, FFmpeg or Rockchip dependencies.
option(KESTREL_TESTS "Build unit tests" OFF)
if(KESTREL_TESTS)
    add_executable(stream_recorder_test tools/test/stream_recorder_test.cpp)
    target_link_libraries(stream_recorder_test PRIVATE kg_dvr)
    add_test(NAME stream_recorder COMMAND stream_recorder_test)
    enable_testing()
endif()
include_directories("${PROJECT_SOURCE_DIR}" "${PROJECT_BINARY_DIR}")
find_path(DRM_UAPI_INCLUDE_DIR drm.h PATH_SUFFIXES libdrm drm)
find_library(AR8030_LIB NAMES ar8030_client)
if(NOT DRM_UAPI_INCLUDE_DIR OR NOT AR8030_LIB)
    message(FATAL_ERROR "Native Ascent needs DRM_UAPI_INCLUDE_DIR and AR8030_LIB (ar_libre)")
endif()
add_library(kg_base STATIC utils/Queue.cpp utils/Msg.cpp base/settings.cpp base/video_pool.cpp
    base/scheduling_helper.cpp base/prof.cpp base/sd_log.cpp base/ltrace.cpp base/restart.cpp base/frame_mode.cpp)
target_link_libraries(kg_base PUBLIC pthread m)
add_library(kg_bsp STATIC bsp/board.cpp ${KESTREL_BOARD_SRC})
add_library(kg_hal STATIC hal/v4l2/vdec_v4l2.cpp)
target_link_libraries(kg_hal PUBLIC kg_base)
add_library(kg_region STATIC region/zones.cpp region/zones_data.cpp region/fpv_rules.cpp)
target_link_libraries(kg_region PUBLIC kg_base)
add_library(kg_radio STATIC radio/ar8030_link.cpp radio/ar8030_link_bb.cpp radio/ar8030_link_bind.cpp
    radio/ar8030_link_air.cpp radio/ar8030_link_rf.cpp radio/ar8030_link_stats.cpp radio/ar8030_link_video.cpp
    radio/bb_watchdog.cpp radio/ar8030_sky.cpp radio/model.cpp radio/air_imu.cpp radio/streams.cpp radio/video_out.cpp)
target_link_libraries(kg_radio PUBLIC kg_hal kg_region "${AR8030_LIB}" dl)
add_library(kg_display STATIC display/ar9311_screen.cpp)
target_include_directories(kg_display PRIVATE "${DRM_UAPI_INCLUDE_DIR}")
target_link_libraries(kg_display PUBLIC kg_hal)
add_library(kg_platform STATIC platform/ascent.cpp platform/ascent_panel.cpp)
target_link_libraries(kg_platform PUBLIC kg_display kg_bsp)
add_executable(ascent-planes-probe tools/test/ascent_planes_probe.cpp)
target_link_libraries(ascent-planes-probe PRIVATE kg_display kg_hal)
add_subdirectory(hud/lvgl)
add_library(kg_dvr STATIC dvr/stream_recorder.cpp dvr/dvr_sink.cpp dvr/hevc_repair.cpp dvr/writer.cpp dvr/mp4.cpp dvr/dvr_library.cpp)
target_link_libraries(kg_dvr PUBLIC kg_base)
add_executable(kestrel-gnd app/ascent_main.cpp radio/bb_sdk_shim.cpp)
target_link_libraries(kestrel-gnd PRIVATE kg_platform kg_radio kg_lvgl_view kg_dvr kg_version)
# The native ar_libre API is intentionally linked, including optional fast-path
# symbols when packaged as a static archive; no SDK runtime interposition needed.
target_link_options(kestrel-gnd PRIVATE -Wl,-u,arlink_version)
install(TARGETS kestrel-gnd RUNTIME DESTINATION bin)
if(KESTREL_TESTS)
    add_executable(ascent_menu_test tools/test/ascent_menu_test.cpp)
    target_link_libraries(ascent_menu_test PRIVATE kg_lvgl_core kg_region)
    add_test(NAME ascent_menu COMMAND ascent_menu_test)
    file(WRITE "${PROJECT_BINARY_DIR}/layers.txt"
        "utils: utils |\nbase: base | utils\nsystem: system | base\nhal: hal | base\n"
        "bsp: bsp | base\nregion: region | base\nrender: render | base\n"
        "radio: radio | hal region system\ndisplay: display | hal\n"
        "platform: platform | hal bsp render display\nhud: hud/lvgl | render system\n"
        "dvr: dvr | platform\napp: app | platform radio hud region dvr\n")
    add_test(NAME layers COMMAND ${CMAKE_COMMAND} -DSRC=${PROJECT_SOURCE_DIR}
        -DLAYERS=${PROJECT_BINARY_DIR}/layers.txt -DNM=${CMAKE_NM} -DLIBDIR=${PROJECT_BINARY_DIR}
        -P ${PROJECT_SOURCE_DIR}/tools/check_layers.cmake)
    add_executable(ui_damage_test tools/test/damage_test.cpp)
    add_test(NAME ui_damage COMMAND ui_damage_test)
    add_executable(v4l2_queue_test tools/test/v4l2_queue_test.cpp)
    target_link_libraries(v4l2_queue_test PRIVATE kg_hal)
    add_test(NAME v4l2_queue COMMAND v4l2_queue_test)
    add_executable(ascent_panel_test tools/test/ascent_panel_test.cpp)
    add_test(NAME ascent_panel COMMAND ascent_panel_test)
    add_executable(i420_overlay_test tools/test/i420_overlay_test.cpp)
    add_test(NAME i420_overlay COMMAND i420_overlay_test)
endif()
