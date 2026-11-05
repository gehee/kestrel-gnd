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
