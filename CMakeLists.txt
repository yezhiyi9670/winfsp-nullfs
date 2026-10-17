# NullFS -- build with CMake (MSVC only)
#
#   cmake -S . -B build -G "Visual Studio 17 2022" -A x64
#   cmake --build build --config Release
#
# The result is build\Release\nullfs-x64.exe.
#
# If WinFsp is not installed in the default location:
#   cmake -S . -B build -DWINFSP_ROOT="D:/tools/WinFsp"

cmake_minimum_required(VERSION 3.20)

project(nullfs LANGUAGES C CXX)

if(NOT WIN32)
    message(FATAL_ERROR "NullFS is a Windows file system; it only builds on Windows with MSVC.")
endif()

# The directory that contains inc\ and lib\. The WinFsp installer puts it here
# by default (WinFsp is a 32-bit installer, hence Program Files (x86)).
set(WINFSP_ROOT "C:/Program Files (x86)/WinFsp"
    CACHE PATH "WinFsp installation root (contains inc/ and lib/)")

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(NULLFS_WINFSP_ARCH x64)
elseif(CMAKE_SIZEOF_VOID_P EQUAL 4)
    set(NULLFS_WINFSP_ARCH x86)
else()
    message(FATAL_ERROR "Unsupported architecture (pointer size ${CMAKE_SIZEOF_VOID_P}).")
endif()

set(NULLFS_WINFSP_DLL "winfsp-${NULLFS_WINFSP_ARCH}.dll")

find_path(NULLFS_WINFSP_INCLUDE_DIR
    NAMES winfsp/winfsp.h
    HINTS "${WINFSP_ROOT}/inc"
    NO_DEFAULT_PATH)

find_library(NULLFS_WINFSP_LIBRARY
    NAMES "winfsp-${NULLFS_WINFSP_ARCH}"
    HINTS "${WINFSP_ROOT}/lib"
    NO_DEFAULT_PATH)

if(NOT NULLFS_WINFSP_INCLUDE_DIR OR NOT NULLFS_WINFSP_LIBRARY)
    message(FATAL_ERROR
        "WinFsp developer files were not found under '${WINFSP_ROOT}'.\n"
        "Install WinFsp v2.1 and make sure the 'Developer' feature is selected,\n"
        "or configure with -DWINFSP_ROOT=<WinFsp install directory>.\n"
        "(Expected '${WINFSP_ROOT}/inc/winfsp/winfsp.h' and "
        "'${WINFSP_ROOT}/lib/winfsp-${NULLFS_WINFSP_ARCH}.lib'.)")
endif()

add_executable(nullfs nullfs.cpp nullfs-main.c)

set_target_properties(nullfs PROPERTIES
    OUTPUT_NAME "nullfs-${NULLFS_WINFSP_ARCH}"
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON)

target_compile_definitions(nullfs PRIVATE
    UNICODE
    _UNICODE
    _CRT_SECURE_NO_WARNINGS
    _CRT_NONSTDC_NO_WARNINGS)

target_include_directories(nullfs PRIVATE "${NULLFS_WINFSP_INCLUDE_DIR}")

# delayimp.lib provides the default delay-load helper; FspLoad() (called from
# wmain in nullfs-main.c) makes sure winfsp-<arch>.dll is in the process before
# the first WinFsp API call, so the executable finds the DLL through the WinFsp
# registry key instead of requiring bin\ on the PATH.
# advapi32.lib is needed for the SDDL/ACL calls the sample makes
# (GetSecurityDescriptorLength, ConvertStringSecurityDescriptorToSecurityDescriptorW)
# and for RegGetValueW, which the FspLoad() helper in winfsp.h uses.
target_link_libraries(nullfs PRIVATE "${NULLFS_WINFSP_LIBRARY}" advapi32 delayimp)
target_link_options(nullfs PRIVATE "/DELAYLOAD:${NULLFS_WINFSP_DLL}")

message(STATUS "NullFS: WinFsp include dir : ${NULLFS_WINFSP_INCLUDE_DIR}")
message(STATUS "NullFS: WinFsp library     : ${NULLFS_WINFSP_LIBRARY}")
message(STATUS "NullFS: delay-loaded DLL   : ${NULLFS_WINFSP_DLL}")
