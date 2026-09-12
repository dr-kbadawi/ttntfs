# ntfscli - command-line harness for the NTFS core (core/include/ntfscore.h).
#
# Standalone:
#   cmake -S tools/ntfscli -B tools/build/ntfscli && cmake --build tools/build/ntfscli
# or from the top-level project via add_subdirectory(tools/ntfscli), in which
# case the `ntfscore` target is linked directly.
#
# Core selection, in order:
#   1. a `ntfscore` CMake target (top-level build)            -> link it
#   2. NTFSCLI_USE_STUB=OFF and ${NTFSCORE_LIB} exists         -> imported .a
#      (+ libntfsplatform.a next to it, linked after the core, + pthread;
#      the executable is built for the archive's architecture, see below)
#   3. otherwise                                               -> core/stub/ntfscore_stub.c
#                                                                 + bdev_compat.c
cmake_minimum_required(VERSION 3.16)
project(ntfscli C)

get_filename_component(NTFS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

option(NTFSCLI_USE_STUB "Link core/stub/ntfscore_stub.c instead of libntfscore.a" OFF)
option(NTFSCLI_SANITIZE "Build with -fsanitize=address,undefined" OFF)
set(NTFSCORE_LIB "${NTFS_ROOT}/build/libntfscore.a" CACHE FILEPATH
    "Path to libntfscore.a produced by the top-level CMake build")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_executable(ntfscli
  main.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../common/json.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../common/sha256.c)

target_include_directories(ntfscli PRIVATE
  ${NTFS_ROOT}/core/include
  ${NTFS_ROOT}/platform/include
  ${CMAKE_CURRENT_SOURCE_DIR}/../common)

target_compile_options(ntfscli PRIVATE
  -Wall -Wextra -Wno-unused-parameter -Werror=implicit-function-declaration)

if(NTFSCLI_SANITIZE)
  target_compile_options(ntfscli PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(ntfscli PRIVATE -fsanitize=address,undefined)
endif()

find_package(Threads REQUIRED)

if(TARGET ntfscore)
  message(STATUS "ntfscli: linking ntfscore target")
  target_link_libraries(ntfscli PRIVATE ntfscore Threads::Threads)
  target_compile_definitions(ntfscli PRIVATE "NTFSCLI_CORE=\"libntfscore (target)\"")
elseif(NOT NTFSCLI_USE_STUB AND EXISTS "${NTFSCORE_LIB}")
  message(STATUS "ntfscli: linking ${NTFSCORE_LIB}")
  add_library(ntfscore_imported STATIC IMPORTED)
  set_target_properties(ntfscore_imported PROPERTIES IMPORTED_LOCATION "${NTFSCORE_LIB}")
  # Link order: core, then platform (the merged build/libntfscore.a already
  # contains the platform objects; a separate libntfsplatform.a next to it
  # is linked afterwards so a core-only archive resolves too), then pthread.
  get_filename_component(NTFSCORE_LIBDIR "${NTFSCORE_LIB}" DIRECTORY)
  set(NTFSPLATFORM_LIB "${NTFSCORE_LIBDIR}/libntfsplatform.a")
  if(EXISTS "${NTFSPLATFORM_LIB}")
    message(STATUS "ntfscli: linking ${NTFSPLATFORM_LIB} after the core")
    add_library(ntfsplatform_imported STATIC IMPORTED)
    set_target_properties(ntfsplatform_imported PROPERTIES IMPORTED_LOCATION "${NTFSPLATFORM_LIB}")
    target_link_libraries(ntfscli PRIVATE ntfscore_imported ntfsplatform_imported Threads::Threads)
  else()
    target_link_libraries(ntfscli PRIVATE ntfscore_imported Threads::Threads)
  endif()
  target_compile_definitions(ntfscli PRIVATE "NTFSCLI_CORE=\"${NTFSCORE_LIB}\"")
  # The archive may not be built for the host architecture: a top-level tree
  # configured by an x86_64 cmake (Homebrew under Rosetta) produces x86_64
  # objects, which ld silently ignores ("found architecture 'x86_64',
  # required architecture 'arm64'") and every symbol comes out undefined.
  # Build ntfscli for the archive's architecture unless the caller chose one.
  if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    execute_process(COMMAND lipo -archs "${NTFSCORE_LIB}"
      OUTPUT_VARIABLE NTFSCORE_ARCHS OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    string(REPLACE " " ";" NTFSCORE_ARCHS "${NTFSCORE_ARCHS}")
    if(NTFSCORE_ARCHS AND NOT "${CMAKE_SYSTEM_PROCESSOR}" IN_LIST NTFSCORE_ARCHS)
      list(GET NTFSCORE_ARCHS 0 NTFSCORE_ARCH)
      message(WARNING "ntfscli: ${NTFSCORE_LIB} is ${NTFSCORE_ARCH}, host is ${CMAKE_SYSTEM_PROCESSOR}; "
                      "building ntfscli for ${NTFSCORE_ARCH} (runs under Rosetta). "
                      "Rebuild the top-level tree with a native cmake (tools/env.sh: ensure_cmake) to avoid this.")
      set_target_properties(ntfscli PROPERTIES OSX_ARCHITECTURES "${NTFSCORE_ARCH}")
    endif()
  endif()
else()
  if(NOT NTFSCLI_USE_STUB)
    message(WARNING "ntfscli: ${NTFSCORE_LIB} not found; falling back to core/stub (every op returns ENOSYS)")
  else()
    message(STATUS "ntfscli: using core/stub (NTFSCLI_USE_STUB=ON)")
  endif()
  target_sources(ntfscli PRIVATE
    ${NTFS_ROOT}/core/stub/ntfscore_stub.c
    bdev_compat.c)
  target_compile_definitions(ntfscli PRIVATE NTFSCLI_STUB=1 NTFSCLI_CORE="core/stub")
  target_link_libraries(ntfscli PRIVATE Threads::Threads)
endif()
