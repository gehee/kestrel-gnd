# Asset generation always runs on the build host, including cross builds.
set(KESTREL_UI_FONT "" CACHE FILEPATH "Source idle artwork font (ChakraPetch-SemiBold.ttf)")
set(KESTREL_UI_BACKGROUND "" CACHE FILEPATH "Source idle artwork background PNG")
set(KESTREL_UI_WIDTH 1920 CACHE STRING "Generated idle image width")
set(KESTREL_UI_HEIGHT 1080 CACHE STRING "Generated idle image height")
if(KESTREL_UI_FONT OR KESTREL_UI_BACKGROUND)
    if(NOT EXISTS "${KESTREL_UI_FONT}" OR NOT EXISTS "${KESTREL_UI_BACKGROUND}")
        message(FATAL_ERROR "Static idle assets need both KESTREL_UI_FONT and KESTREL_UI_BACKGROUND")
    endif()
    find_program(KESTREL_HOST_CXX NAMES c++ g++ clang++ NO_CMAKE_FIND_ROOT_PATH)
    find_program(KESTREL_HOST_PKG_CONFIG NAMES pkg-config NO_CMAKE_FIND_ROOT_PATH)
    if(NOT KESTREL_HOST_CXX OR NOT KESTREL_HOST_PKG_CONFIG)
        message(FATAL_ERROR "Static idle assets need a host C++ compiler and pkg-config")
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env --unset=PKG_CONFIG_SYSROOT_DIR
        --unset=PKG_CONFIG_LIBDIR --unset=PKG_CONFIG_PATH
        "${KESTREL_HOST_PKG_CONFIG}" --cflags --libs cairo fontconfig
        OUTPUT_VARIABLE _idle_flags OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE _idle_result)
    if(NOT _idle_result EQUAL 0)
        message(FATAL_ERROR "Static idle assets need host Cairo and Fontconfig development packages")
    endif()
    separate_arguments(_idle_flags UNIX_COMMAND "${_idle_flags}")
    set(_idle_tool "${PROJECT_BINARY_DIR}/host-tools/kestrel-idle-bake")
    set(_idle_output "${PROJECT_BINARY_DIR}/ui-assets/idle-scenes.kui")
    add_custom_command(OUTPUT "${_idle_tool}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${PROJECT_BINARY_DIR}/host-tools"
        COMMAND "${KESTREL_HOST_CXX}" -std=c++17 -O2 "-I${PROJECT_SOURCE_DIR}"
            "${PROJECT_SOURCE_DIR}/tools/ui_idle_bake.cpp" "${PROJECT_SOURCE_DIR}/hud/hud_theme.cpp"
            ${_idle_flags} -o "${_idle_tool}"
        DEPENDS "${PROJECT_SOURCE_DIR}/tools/ui_idle_bake.cpp"
            "${PROJECT_SOURCE_DIR}/hud/hud_theme.cpp" "${PROJECT_SOURCE_DIR}/hud/hud_theme.hpp"
        VERBATIM)
    add_custom_command(OUTPUT "${_idle_output}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${PROJECT_BINARY_DIR}/ui-assets"
        COMMAND "${_idle_tool}" "${KESTREL_UI_FONT}" "${KESTREL_UI_BACKGROUND}"
            "${KESTREL_UI_WIDTH}" "${KESTREL_UI_HEIGHT}" "${_idle_output}.tmp"
        COMMAND "${CMAKE_COMMAND}" -E rename "${_idle_output}.tmp" "${_idle_output}"
        DEPENDS "${_idle_tool}" "${KESTREL_UI_FONT}" "${KESTREL_UI_BACKGROUND}"
        VERBATIM)
    add_custom_target(kestrel-idle-assets ALL DEPENDS "${_idle_output}")
    install(FILES "${_idle_output}" DESTINATION share/kestrel/ui)
    set(_bf_tool "${PROJECT_BINARY_DIR}/host-tools/kestrel-bf-bake")
    set(_bf_output "${PROJECT_BINARY_DIR}/ui-assets/betaflight.kui")
    add_custom_command(OUTPUT "${_bf_tool}"
        COMMAND "${KESTREL_HOST_CXX}" -std=c++17 -O2 "-I${PROJECT_SOURCE_DIR}"
            "${PROJECT_SOURCE_DIR}/tools/ui_bf_bake.cpp" "${PROJECT_SOURCE_DIR}/msp/betaflight_glyphs.cpp"
            ${_idle_flags} -o "${_bf_tool}"
        DEPENDS "${_idle_tool}" "${PROJECT_SOURCE_DIR}/tools/ui_bf_bake.cpp"
            "${PROJECT_SOURCE_DIR}/msp/betaflight_glyphs.cpp" "${PROJECT_SOURCE_DIR}/msp/betaflight_glyphs.hpp"
        VERBATIM)
    add_custom_command(OUTPUT "${_bf_output}"
        COMMAND "${_bf_tool}" "${KESTREL_UI_FONT}" "${_bf_output}.tmp"
        COMMAND "${CMAKE_COMMAND}" -E rename "${_bf_output}.tmp" "${_bf_output}"
        DEPENDS "${_bf_tool}" "${_idle_output}" "${KESTREL_UI_FONT}" VERBATIM)
    add_custom_target(kestrel-bf-assets ALL DEPENDS "${_bf_output}")
    install(FILES "${_bf_output}" DESTINATION share/kestrel/ui)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(_profile_wedge "${PROJECT_BINARY_DIR}/host-tools/kestrel-ui-wedge-bake")
    add_custom_command(OUTPUT "${_profile_wedge}"
        COMMAND "${KESTREL_HOST_CXX}" -std=c++17 -O2 "-I${PROJECT_SOURCE_DIR}"
            "${PROJECT_SOURCE_DIR}/hud/lvgl/assetgen.cpp" -o "${_profile_wedge}"
        DEPENDS "${_idle_tool}" "${PROJECT_SOURCE_DIR}/hud/lvgl/assetgen.cpp"
            "${PROJECT_SOURCE_DIR}/render/software/canopy.hpp" VERBATIM)
    add_custom_command(OUTPUT "${PROJECT_BINARY_DIR}/ui-assets/profiles.stamp"
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/ui_profiles_bake.py"
            "${KESTREL_UI_FONT}" "${KESTREL_UI_BACKGROUND}" "${_idle_tool}"
            "${_bf_tool}" "${_profile_wedge}" "${PROJECT_BINARY_DIR}/ui-assets"
        DEPENDS "${_idle_tool}" "${_bf_tool}" "${_profile_wedge}"
            "${PROJECT_SOURCE_DIR}/tools/ui_profiles_bake.py" "${PROJECT_SOURCE_DIR}/tools/ui_font_bake.py"
            "${KESTREL_UI_FONT}" "${KESTREL_UI_BACKGROUND}" VERBATIM)
    add_custom_target(kestrel-ui-profiles ALL DEPENDS "${PROJECT_BINARY_DIR}/ui-assets/profiles.stamp")
    install(DIRECTORY "${PROJECT_BINARY_DIR}/ui-assets/1280x720"
        "${PROJECT_BINARY_DIR}/ui-assets/1920x1080" "${PROJECT_BINARY_DIR}/ui-assets/menu-fonts"
        DESTINATION share/kestrel/ui)
endif()
