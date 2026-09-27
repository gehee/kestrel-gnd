cmake_minimum_required(VERSION 3.13)
project(rflink_stub C)

# A vendor plugin is just a shared object built against include/fpvos/rf_link.h.
# Cross-compile it with the same toolchain as the goggle image (aarch64) and
# install it under /usr/lib/fpvos/rflink/ - the host loads every *.so there.
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_library(rflink_stub SHARED rflink_stub.c)
target_include_directories(rflink_stub PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../include)
target_compile_options(rflink_stub PRIVATE -Wall -Wextra -fvisibility=hidden)
# Only the entry point is exported; everything else stays private to the .so.
target_link_options(rflink_stub PRIVATE -Wl,--exclude-libs,ALL)
target_link_libraries(rflink_stub PRIVATE m)

install(TARGETS rflink_stub LIBRARY DESTINATION lib/fpvos/rflink)
