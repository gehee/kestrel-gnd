# Regenerate the version stamp. Run at BUILD time (not configure time) from a
# custom target, so a binary can never claim a commit or a build time that
# belongs to whenever the build directory happened to be configured.
#
# Expects: PROJECT_VERSION, SRC_DIR, IN_FILE, OUT_FILE; optionally
# GIT_HASH_FALLBACK, the revision to report instead of asking git.
string(TIMESTAMP KESTREL_GND_BUILD_TIME "%Y-%m-%d %H:%M UTC" UTC)

# A packager that names the revision wins over git: its source tree is an
# unpacked tarball, and `git describe` there would walk up into whatever
# repository it happens to sit in - Buildroot's own, in fpvOS - and report
# that project's tag as ours.
if(GIT_HASH_FALLBACK)
    set(KESTREL_GND_GIT_HASH "${GIT_HASH_FALLBACK}")
endif()
if(NOT KESTREL_GND_GIT_HASH)
    execute_process(
        COMMAND git describe --tags --always --dirty
        WORKING_DIRECTORY ${SRC_DIR}
        OUTPUT_VARIABLE KESTREL_GND_GIT_HASH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(NOT KESTREL_GND_GIT_HASH)
        execute_process(
            COMMAND git rev-parse --short HEAD
            WORKING_DIRECTORY ${SRC_DIR}
            OUTPUT_VARIABLE KESTREL_GND_GIT_HASH
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
    endif()
endif()
if(NOT KESTREL_GND_GIT_HASH)
    set(KESTREL_GND_GIT_HASH "unknown")
endif()

# configure_file leaves the file alone when the content is unchanged, so a
# rebuild within the same minute does not relink for nothing.
configure_file(${IN_FILE} ${OUT_FILE} @ONLY)
