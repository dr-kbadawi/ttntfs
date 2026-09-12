# SPDX-License-Identifier: GPL-2.0
# ntfscore: Tier 0/1 driver sources (core/ntfs) plus, when present, the
# Tier 2 replacement (core/vfs) and the $LogFile module (core/logfile).
#
# core/ntfs is a quote-only include directory (-iquote): the driver's own
# time.h must not shadow the system <time.h>.
file(GLOB NTFS_CORE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/ntfs/*.c)
file(GLOB NTFS_VFS_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/vfs/*.c)
file(GLOB NTFS_LOGFILE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/logfile/*.c)

add_library(ntfscore STATIC ${NTFS_CORE_SOURCES} ${NTFS_VFS_SOURCES} ${NTFS_LOGFILE_SOURCES})
target_include_directories(ntfscore PUBLIC
  ${CMAKE_SOURCE_DIR}/platform/include
  ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(ntfscore PRIVATE -iquote ${CMAKE_CURRENT_SOURCE_DIR}/ntfs)
target_link_libraries(ntfscore PUBLIC ntfsplatform)
