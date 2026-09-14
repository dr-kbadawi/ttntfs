# SPDX-License-Identifier: GPL-2.0
#
# NTFS for macOS: user-space port of the Linux 7.1 fs/ntfs driver.
#
#   cmake -S . -B build && cmake --build build && ctest --test-dir build
#   UBSan is on by default; -DNTFS_ASAN=ON adds ASan, which hangs on macOS 26.
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

# Sanitizers, split because they are not equally usable here.
#
# UBSan works and is cheap, so it is ON by default for the tests: this port does
# a lot of unaligned reads out of packed on-disk structures, and UBSan is what
# catches that class. Turning it off is the unusual choice, not turning it on.
#
# ASan HANGS on macOS 26 / Apple silicon. It deadlocks inside
# __asan::AsanInitInternal() before main() ever runs, so the process produces no
# output and never exits -- confirmed 2026-09-14 on every test in this project,
# not just one. One runaway sat burning a core for 23 hours before anyone
# noticed, and because the logfile suite ran ASan by default its 243 checks had
# silently not executed for a day. So ASan is opt-in, and anyone who turns it on
# should expect that and check the toolchain first. It is worth retrying after a
# Xcode update: ASan is what originally found the work-queue use-after-free and
# the lookup-versus-invalidate race (docs/progress/platform-review.md).
#
# Both default OFF, and that is a constraint rather than a preference: this
# build tree produces libntfscore.a, which the Xcode app links directly
# (NTFS_CORE_MODE=real). An instrumented static library needs the sanitizer
# runtime on the final link line, which the app does not provide and must not
# ship, so turning a sanitizer on here breaks the app build. Use a second build
# directory instead, which is what tools/ci.sh does:
#
#   cmake -S . -B build-ubsan -DNTFS_UBSAN=ON && ctest --test-dir build-ubsan
#
#   -DNTFS_UBSAN=ON      UBSan (works; on in CI)
#   -DNTFS_ASAN=ON       add ASan (see above before you do)
#   -DNTFS_SANITIZE=ON   both, the old spelling, kept working
option(NTFS_SANITIZE "Deprecated: same as -DNTFS_ASAN=ON -DNTFS_UBSAN=ON" OFF)
option(NTFS_UBSAN "Build with UndefinedBehaviorSanitizer (use a separate build dir)" OFF)
option(NTFS_ASAN "Build the tests with AddressSanitizer (hangs on macOS 26)" OFF)
option(NTFS_BUILD_TESTS "Build the unit tests" ON)

set(NTFS_C_FLAGS -Wall -Wextra -Wno-unused-parameter -Werror=implicit-function-declaration
    -fno-strict-aliasing -fwrapv)
add_compile_options(${NTFS_C_FLAGS})
if(NTFS_SANITIZE)
  set(NTFS_ASAN ON)
  set(NTFS_UBSAN ON)
endif()
set(NTFS_SAN_LIST "")
if(NTFS_ASAN)
  list(APPEND NTFS_SAN_LIST address)
endif()
if(NTFS_UBSAN)
  list(APPEND NTFS_SAN_LIST undefined)
endif()
if(NTFS_SAN_LIST)
  list(JOIN NTFS_SAN_LIST "," NTFS_SAN_ARG)
  message(STATUS "sanitizers: ${NTFS_SAN_ARG}")
  add_compile_options(-fsanitize=${NTFS_SAN_ARG} -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=${NTFS_SAN_ARG})
  # Report rather than continue: an unaligned load that "works" today is still a
  # bug, and a test that prints a warning and passes is a test nobody reads.
  add_compile_options(-fno-sanitize-recover=undefined)
else()
  message(STATUS "sanitizers: none")
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
