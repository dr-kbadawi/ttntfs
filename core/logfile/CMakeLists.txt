# SPDX-License-Identifier: GPL-2.0
# $LogFile parser / analyzer / replay engine. Depends on libc only.
#
# Standalone:
#   cmake -S core/logfile -B build-logfile && cmake --build build-logfile && ctest --test-dir build-logfile
# From the top-level project: add_subdirectory(core/logfile) yields the
# static library target `ntfslogfile`; the CLI and tests are added only when
# this is the top-level project or NTFS_LOGFILE_TOOLS is ON.
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
project(ntfslogfile C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(NTFS_LOGFILE_TOOLS "Build the ntfslog CLI and the unit tests" OFF)

set(NTFS_LOGFILE_WARNINGS -Wall -Wextra -Wno-unused-parameter -Werror=implicit-function-declaration)
set(NTFS_LOGFILE_SOURCES
	${CMAKE_CURRENT_SOURCE_DIR}/logfile_open.c
	${CMAKE_CURRENT_SOURCE_DIR}/logfile_pages.c
	${CMAKE_CURRENT_SOURCE_DIR}/logfile_records.c
	${CMAKE_CURRENT_SOURCE_DIR}/logfile_tables.c
	${CMAKE_CURRENT_SOURCE_DIR}/logfile_replay.c)

add_library(ntfslogfile STATIC ${NTFS_LOGFILE_SOURCES})
target_include_directories(ntfslogfile PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(ntfslogfile PRIVATE ${NTFS_LOGFILE_WARNINGS})

if(PROJECT_IS_TOP_LEVEL OR NTFS_LOGFILE_TOOLS)
	# Minimal NTFS image access (boot sector, MFT record 0/2, run lists)
	# shared by the CLI and the tests.
	add_library(ntfslogimg STATIC ntfs_image.c)
	# The logfile root, where ntfs_image.h lives. It used to be cli/, and
	# leaving that stale entry here is what let an untracked pre-move copy of
	# the header shadow the real one for cli/ntfslog.c (quote-include looks
	# beside the including file first) and abort ntfslog at runtime.
	target_include_directories(ntfslogimg PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
	target_link_libraries(ntfslogimg PUBLIC ntfslogfile)
	target_compile_options(ntfslogimg PRIVATE ${NTFS_LOGFILE_WARNINGS})

	add_executable(ntfslog cli/ntfslog.c)
	target_link_libraries(ntfslog PRIVATE ntfslogimg ntfslogfile)
	target_compile_options(ntfslog PRIVATE ${NTFS_LOGFILE_WARNINGS})

	# Tests: the whole module is recompiled with sanitizers.
	# Sanitizers are opt-in (-DNTFS_LOGFILE_SANITIZE=ON), not the default.
	#
	# On macOS 26 / Apple Silicon the ASan runtime hangs inside
	# __asan::AsanInitInternal() before main() ever runs: the test never starts,
	# ctest never returns, and a runaway process sits burning a core. One did
	# for 23 hours before it was noticed on 2026-09-14, and because
	# logfile_unit is the first test in this project's suite it blocked
	# logfile_images behind it too. The suite itself is fine -- 243 checks,
	# 0 failures, a few seconds -- so the default build runs it and the
	# sanitizer build is there for whoever wants it on a toolchain where it works.
	option(NTFS_LOGFILE_SANITIZE "Build the logfile tests with ASan/UBSan" OFF)
	if(NTFS_LOGFILE_SANITIZE)
		set(NTFS_LOGFILE_SAN -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1)
	else()
		set(NTFS_LOGFILE_SAN -fno-omit-frame-pointer -g -O1)
	endif()
	add_executable(test_logfile tests/test_logfile.c ntfs_image.c ${NTFS_LOGFILE_SOURCES})
	target_include_directories(test_logfile PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
	target_compile_options(test_logfile PRIVATE ${NTFS_LOGFILE_WARNINGS} ${NTFS_LOGFILE_SAN})
	target_link_options(test_logfile PRIVATE ${NTFS_LOGFILE_SAN})

	enable_testing()
	# Fixture images from the tools stream (mkntfs-formatted, all-0xff
	# $LogFile). Override with -DNTFS_IMAGES=<dir>.
	set(NTFS_IMAGES "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/images" CACHE PATH
		"Directory of fixture .img files for the logfile tests")
	add_test(NAME logfile_unit COMMAND test_logfile)
	set_tests_properties(logfile_unit PROPERTIES
		ENVIRONMENT "NTFS_IMAGES=${NTFS_IMAGES};UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1")
	# The analyzer must report every fixture clean; skipped when there are none.
	add_test(NAME logfile_images
		COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_images.sh $<TARGET_FILE:ntfslog> ${NTFS_IMAGES})
	set_tests_properties(logfile_images PROPERTIES SKIP_RETURN_CODE 77)
endif()
