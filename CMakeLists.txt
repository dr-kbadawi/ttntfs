# SPDX-License-Identifier: GPL-2.0
#
# NTFS for macOS: user-space port of the Linux 7.1 fs/ntfs driver.
#
#   cmake -S . -B build [-DNTFS_SANITIZE=ON] && cmake --build build && ctest --test-dir build
#
# Targets:
#   ntfsplatform   platform/src/*.c + platform/pagecache/pagecache.c  (static)
#   ntfscore       core/ntfs/*.c [+ core/vfs/*.c + core/logfile/*.c] (static)
#   build/libntfscore.a   the two merged, what fskit/ and tools/ link
#   tests          platform/tests, platform/pagecache/tests (ctest)
cmake_minimum_required(VERSION 3.20)
# Build for the real host CPU even when cmake itself runs under Rosetta
# (Intel Homebrew cmake spawns x86_64 compilers by default).
if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
  execute_process(COMMAND sysctl -n hw.optional.arm64 OUTPUT_VARIABLE _ntfs_has_arm64
                  OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(_ntfs_has_arm64 STREQUAL "1")
    set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "Target architecture" FORCE)
  endif()
endif()
project(ntfsport C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)	# -std=gnu11

option(NTFS_SANITIZE "Build everything with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
option(NTFS_BUILD_TESTS "Build the unit tests" ON)

set(NTFS_C_FLAGS -Wall -Wextra -Wno-unused-parameter -Werror=implicit-function-declaration
    -fno-strict-aliasing -fwrapv)
add_compile_options(${NTFS_C_FLAGS})
if(NTFS_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g -O1)
  add_link_options(-fsanitize=address,undefined)
endif()

find_package(Threads REQUIRED)

# ---- platform: the linux-compat layer --------------------------------------
file(GLOB NTFS_PLATFORM_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/platform/src/*.c)
add_library(ntfsplatform STATIC
  ${NTFS_PLATFORM_SOURCES}
  ${CMAKE_CURRENT_SOURCE_DIR}/platform/pagecache/pagecache.c)
target_include_directories(ntfsplatform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/platform/include)
target_link_libraries(ntfsplatform PUBLIC Threads::Threads)

# ---- core: the ported driver -----------------------------------------------
add_subdirectory(core)

# ---- merged archive: build/libntfscore.a -----------------------------------
set(NTFS_MERGED_LIB ${CMAKE_BINARY_DIR}/libntfscore.a)
add_custom_command(
  OUTPUT ${NTFS_MERGED_LIB}
  COMMAND libtool -static -o ${NTFS_MERGED_LIB} $<TARGET_FILE:ntfscore> $<TARGET_FILE:ntfsplatform>
  DEPENDS ntfscore ntfsplatform
  COMMENT "Merging libntfscore.a"
  VERBATIM)
add_custom_target(ntfscore_merged ALL DEPENDS ${NTFS_MERGED_LIB})

# ---- tests -----------------------------------------------------------------
if(NTFS_BUILD_TESTS)
  enable_testing()

  # Mount-time decisions: the journal clean rule, the read-only reason, the
  # probe fast path, and the device-flush count. Every case is a bug that
  # reached a real disk before it had a test.
  add_executable(test_mount core/tests/test_mount.c)
  target_link_libraries(test_mount ntfscore ntfsplatform)
  target_include_directories(test_mount PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/core/include
    ${CMAKE_CURRENT_SOURCE_DIR}/core/logfile
    ${CMAKE_CURRENT_SOURCE_DIR}/platform/include)
  add_test(NAME mount COMMAND test_mount)
  set_tests_properties(mount PROPERTIES
    ENVIRONMENT "NTFS_IMAGES=${CMAKE_CURRENT_SOURCE_DIR}/tools/images")

  # enable-module.sh: two silent-failure bugs cost a mount each. Static checks
  # plus the real prune loop under stubbed tools; touches no system state.
  add_test(NAME enable_module_script
           COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/fskit/scripts/tests/test-enable-module.sh)

  # The $LogFile module keeps its own CMake project so it can be built alone.
  # Pull its tests in here as well: for a day they existed but were never run by
  # `ctest --test-dir build`, which is how a stale header that aborted ntfslog,
  # and an ASan hang that stopped logfile_unit's 243 checks from executing, both
  # went unnoticed. A test nobody runs is not coverage.
  set(NTFS_LOGFILE_TOOLS ON CACHE BOOL "" FORCE)
  set(NTFS_IMAGES "${CMAKE_CURRENT_SOURCE_DIR}/tools/images" CACHE PATH "" FORCE)
  add_subdirectory(core/logfile logfile-tools)

  add_executable(test_inode platform/tests/test_inode.c)
  target_link_libraries(test_inode ntfsplatform)
  add_test(NAME platform_inode COMMAND test_inode)

  add_executable(test_bdev platform/tests/test_bdev.c)
  target_link_libraries(test_bdev ntfsplatform)
  add_test(NAME platform_bdev COMMAND test_bdev)

  # The page cache's own tests, linked against the full platform library
  # (its standalone_shims.c is only for the standalone build).
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/platform/pagecache/tests/test_pagecache.c)
    add_executable(test_pagecache platform/pagecache/tests/test_pagecache.c)
    target_link_libraries(test_pagecache ntfsplatform)
    add_test(NAME pagecache COMMAND test_pagecache)
  endif()
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/platform/pagecache/tests/stress_pagecache.c)
    add_executable(stress_pagecache platform/pagecache/tests/stress_pagecache.c)
    target_link_libraries(stress_pagecache ntfsplatform)
    add_test(NAME pagecache_stress COMMAND stress_pagecache 3)
  endif()
endif()
