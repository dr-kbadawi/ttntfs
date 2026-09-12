# SPDX-License-Identifier: GPL-2.0
# $LogFile parser / analyzer / replay engine. Depends on libc only.
#
# Standalone:
#   cmake -S core/logfile -B build-logfile && cmake --build build-logfile && ctest --test-dir build-logfile
# From the top-level project: add_subdirectory(core/logfile) yields the
# static library target `ntfslogfile`; the CLI and tests are added only when
# this is the top-level project or NTFS_LOGFILE_TOOLS is ON.
cmake_minimum_required(VERSION 3.20)
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
	add_library(ntfslogimg STATIC cli/ntfs_image.c)
	target_include_directories(ntfslogimg PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/cli)
	target_link_libraries(ntfslogimg PUBLIC ntfslogfile)
	target_compile_options(ntfslogimg PRIVATE ${NTFS_LOGFILE_WARNINGS})

	add_executable(ntfslog cli/ntfslog.c)
	target_link_libraries(ntfslog PRIVATE ntfslogimg ntfslogfile)
	target_compile_options(ntfslog PRIVATE ${NTFS_LOGFILE_WARNINGS})

	# Tests: the whole module is recompiled with sanitizers.
	set(NTFS_LOGFILE_SAN -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1)
	add_executable(test_logfile tests/test_logfile.c cli/ntfs_image.c ${NTFS_LOGFILE_SOURCES})
	target_include_directories(test_logfile PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/cli)
	target_compile_options(test_logfile PRIVATE ${NTFS_LOGFILE_WARNINGS} ${NTFS_LOGFILE_SAN})
	target_link_options(test_logfile PRIVATE ${NTFS_LOGFILE_SAN})

	enable_testing()
	add_test(NAME logfile_unit COMMAND test_logfile)
	# Fixture images from the tools stream, if generated.
	set_tests_properties(logfile_unit PROPERTIES
		ENVIRONMENT "NTFS_IMAGES=${CMAKE_CURRENT_SOURCE_DIR}/../../tools/images;UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1;ASAN_OPTIONS=detect_leaks=1")
endif()
