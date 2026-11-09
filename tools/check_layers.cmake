cmake_minimum_required(VERSION 3.13)
# Checks that each part of kestrel-gnd includes only its own headers and
# those of the parts it is built on - the libraries CMakeLists.txt makes, each
# with the ones it links (kestrel_module). Run by ctest as `layers`, or by
# hand:
#
#   cmake -DSRC=<source dir> -DLAYERS=<build dir>/layers.txt -P tools/check_layers.cmake
#
# Given NM and LIBDIR (the build's), it also reads each part's library
# (libkg_<part>.a) and fails on a function or variable one part uses that is
# defined in a part it is not built on - what includes alone cannot show (a
# declaration below, its definition above).
#
# LAYERS has one line per part: its name, its folders, then the parts it is
# built on, as "name: folder folder | dep dep". A part may include from its
# own folders and from any part it is built on, directly or through another.
# The parts must not be built on each other in a circle.

get_filename_component(SRC "${SRC}" ABSOLUTE)
if(NOT SRC OR NOT LAYERS)
    message(FATAL_ERROR "usage: cmake -DSRC=<source dir> -DLAYERS=<layers.txt> -P check_layers.cmake")
endif()

file(STRINGS "${LAYERS}" lines)
set(parts "")
foreach(line IN LISTS lines)
    if(NOT line MATCHES "^([a-z_]+): *([^|]*)\\|(.*)$")
        message(FATAL_ERROR "layers.txt: cannot read '${line}'")
    endif()
    set(name "${CMAKE_MATCH_1}")
    string(STRIP "${CMAKE_MATCH_2}" dirs)
    string(STRIP "${CMAKE_MATCH_3}" deps)
    separate_arguments(dirs)
    separate_arguments(deps)
    list(APPEND parts ${name})
    set(dirs_${name} ${dirs})
    set(deps_${name} ${deps})
    foreach(d IN LISTS dirs)
        set(owner_${d} ${name})
    endforeach()
endforeach()

# Every part's closure: what it may include from. A part met again on its own
# path is a circle.
function(closure name path out)
    set(all "")
    foreach(dep IN LISTS deps_${name})
        if(dep IN_LIST path OR dep STREQUAL name)
            set(p ${path} ${name} ${dep})
            string(REPLACE ";" " -> " p "${p}")
            message(FATAL_ERROR "the parts are built on each other in a circle: ${p}")
        endif()
        if(NOT DEFINED dirs_${dep})
            message(SEND_ERROR "${name} is built on '${dep}', which is not a part")
            continue()
        endif()
        closure(${dep} "${path};${name}" sub)
        list(APPEND all ${dep} ${sub})
    endforeach()
    list(REMOVE_DUPLICATES all)
    set(${out} "${all}" PARENT_SCOPE)
endfunction()
foreach(name IN LISTS parts)
    closure(${name} "" c)
    set(may_${name} ${name} ${c})
endforeach()

# The part a path belongs to: the longest folder it starts with.
function(owner_of path out)
    set(best "")
    set(best_len 0)
    foreach(name IN LISTS parts)
        foreach(d IN LISTS dirs_${name})
            string(LENGTH "${d}/" len)
            string(SUBSTRING "${path}" 0 ${len} head)
            if(head STREQUAL "${d}/" AND len GREATER best_len)
                set(best ${name})
                set(best_len ${len})
            endif()
        endforeach()
    endforeach()
    set(${out} "${best}" PARENT_SCOPE)
endfunction()

set(count 0)
foreach(name IN LISTS parts)
    foreach(d IN LISTS dirs_${name})
        file(GLOB_RECURSE files RELATIVE "${SRC}"
             "${SRC}/${d}/*.c" "${SRC}/${d}/*.h" "${SRC}/${d}/*.cpp" "${SRC}/${d}/*.hpp")
        foreach(f IN LISTS files)
            owner_of("${f}" fown)
            if(NOT fown STREQUAL name)
                continue()   # in a deeper folder another part owns
            endif()
            file(STRINGS "${SRC}/${f}" incs REGEX "^[ \t]*#[ \t]*include[ \t]+\"[^\"]+/[^\"]+\"")
            foreach(inc IN LISTS incs)
                string(REGEX REPLACE "^[ \t]*#[ \t]*include[ \t]+\"([^\"]+)\".*$" "\\1" h "${inc}")
                owner_of("${h}" hown)
                if(hown STREQUAL "")
                    continue()   # not one of ours (a system or generated header)
                endif()
                if(NOT hown IN_LIST may_${name})
                    message(SEND_ERROR "${f} (${name}) includes ${h} (${hown}), which ${name} is not built on")
                endif()
                math(EXPR count "${count} + 1")
            endforeach()
        endforeach()
    endforeach()
endforeach()
message(STATUS "layers: ${count} includes between folders checked")

# The parts above the platform name no backend: the HUD, the recorder and the
# web view see the display, the canvas and the decoders as interfaces
# (display/display.hpp, render/canvas.hpp, hal/vdec.hpp) and get them from the
# platform (platform/platform.hpp), so a box with another GPU, display or
# decoder - or none - is a platform file, not a change to them. No GL, EGL,
# GBM or DRM header, and none of the backends' own.
set(backend_free hud web dvr)
set(backend_headers "<GLES" "<EGL/" "<gbm.h>" "<xf86drm" "<drm_fourcc.h>" "\"render/canvas_gles.hpp\""
    "\"render/gles_2d.hpp\"" "\"render/gl_context.hpp\"" "\"display/drm.hpp\"" "\"display/renderer.hpp\""
    "\"display/kms/" "\"display/display_lock.hpp\"" "\"platform/gles_hud_layer.hpp\"" "\"hal/rockchip/" "\"hal/ffmpeg/"
    "\"hal/v4l2/" "\"display/ar9311_screen.hpp\"")
set(nbk 0)
foreach(name IN LISTS backend_free)
    if(NOT DEFINED dirs_${name})
        continue()
    endif()
    foreach(d IN LISTS dirs_${name})
        file(GLOB_RECURSE files RELATIVE "${SRC}"
             "${SRC}/${d}/*.c" "${SRC}/${d}/*.h" "${SRC}/${d}/*.cpp" "${SRC}/${d}/*.hpp")
        foreach(f IN LISTS files)
            file(STRINGS "${SRC}/${f}" incs REGEX "^[ \t]*#[ \t]*include[ \t]+[<\"]")
            foreach(inc IN LISTS incs)
                foreach(bad IN LISTS backend_headers)
                    string(FIND "${inc}" "${bad}" at)
                    if(NOT at EQUAL -1)
                        message(SEND_ERROR "${f} (${name}) includes ${bad}...: a backend's header, which only the platform names")
                    endif()
                endforeach()
                math(EXPR nbk "${nbk} + 1")
            endforeach()
        endforeach()
    endforeach()
endforeach()
message(STATUS "layers: ${nbk} includes of the parts above the platform checked for backends")

# The libraries: what each defines (strongly - not the inline functions and
# templates every one has a copy of), and what each uses that it does not.
if(NM AND LIBDIR)
    set(nsyms 0)
    foreach(name IN LISTS parts)
        set(lib "${LIBDIR}/libkg_${name}.a")
        if(NOT EXISTS "${lib}")
            continue()   # a part with no sources
        endif()
        execute_process(COMMAND ${NM} -g "${lib}" OUTPUT_VARIABLE out RESULT_VARIABLE rc)
        if(NOT rc EQUAL 0)
            message(FATAL_ERROR "${NM} -g ${lib} failed")
        endif()
        string(REPLACE "\n" ";" out "${out}")
        set(used_${name} "")
        foreach(l IN LISTS out)
            if(l MATCHES "^[0-9a-fA-F]+ [TDRB] (.+)$")
                set_property(GLOBAL PROPERTY "kg_def:${CMAKE_MATCH_1}" ${name})
            elseif(l MATCHES "^ +U (.+)$")
                list(APPEND used_${name} "${CMAKE_MATCH_1}")
            endif()
        endforeach()
    endforeach()
    foreach(name IN LISTS parts)
        foreach(sym IN LISTS used_${name})
            get_property(def GLOBAL PROPERTY "kg_def:${sym}")
            if(def AND NOT def IN_LIST may_${name})
                message(SEND_ERROR "libkg_${name}.a uses ${sym}, defined in ${def}, which ${name} is not built on")
            endif()
            math(EXPR nsyms "${nsyms} + 1")
        endforeach()
    endforeach()
    message(STATUS "layers: ${nsyms} symbols the libraries use checked")
endif()
