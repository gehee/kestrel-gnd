# Checks that every key of the settings file is declared once: each part
# declares its settings in its settings header (see Setting,
# base/settings.hpp), and two declarations of one key - two defaults that
# could disagree - fail here. Run by ctest as `settings_keys`, or by hand:
#
#   cmake -DSRC=<source dir> -P tools/check_settings.cmake

get_filename_component(SRC "${SRC}" ABSOLUTE)
file(GLOB_RECURSE headers RELATIVE "${SRC}" "${SRC}/*/settings.hpp" "${SRC}/base/frame_mode.hpp")
set(count 0)
set(dups 0)
foreach(h IN LISTS headers)
    file(STRINGS "${SRC}/${h}" decls REGEX "Setting<[^>]+>[ \t]+[A-Za-z_0-9]+[ \t]*\\{\"[^\"]+\"")
    foreach(d IN LISTS decls)
        string(REGEX REPLACE "^.*\\{\"([^\"]+)\".*$" "\\1" key "${d}")
        get_property(seen GLOBAL PROPERTY "kg_key:${key}")
        if(seen)
            message(SEND_ERROR "the setting ${key} is declared in ${seen} and in ${h}")
            math(EXPR dups "${dups} + 1")
        endif()
        set_property(GLOBAL PROPERTY "kg_key:${key}" "${h}")
        math(EXPR count "${count} + 1")
    endforeach()
endforeach()
if(dups EQUAL 0)
    message(STATUS "settings: ${count} keys, each declared once")
endif()
