# Cross-compile for 64-bit Windows with MinGW-w64 (e.g. `brew install mingw-w64`
# on macOS, or the mingw-w64 packages on Linux). Used by tools/build_windows.sh.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(TOOLCHAIN_PREFIX x86_64-w64-mingw32)
# Debian/Ubuntu install win32- and posix-thread variants side by side; the
# launcher uses C++ threads, so prefer posix
find_program(_mingw_cc NAMES ${TOOLCHAIN_PREFIX}-gcc-posix ${TOOLCHAIN_PREFIX}-gcc REQUIRED)
find_program(_mingw_cxx NAMES ${TOOLCHAIN_PREFIX}-g++-posix ${TOOLCHAIN_PREFIX}-g++ REQUIRED)
set(CMAKE_C_COMPILER ${_mingw_cc})
set(CMAKE_CXX_COMPILER ${_mingw_cxx})
set(CMAKE_RC_COMPILER ${TOOLCHAIN_PREFIX}-windres)
# look for libraries/headers (SDL3) only in the target prefixes
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
