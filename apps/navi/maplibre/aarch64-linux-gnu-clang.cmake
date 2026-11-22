# Cross-compiles MapLibre Native for arm64 with the host's clang against a Debian arm64 root filesystem
# (NAVI_SYSROOT, set by build-maplibre.sh). The root's own GCC headers and libraries are used, so the
# result links against exactly what the device runs.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

if(NOT NAVI_SYSROOT)
    set(NAVI_SYSROOT "$ENV{NAVI_SYSROOT}")
endif()
if(NOT NAVI_SYSROOT)
    message(FATAL_ERROR "NAVI_SYSROOT is not set: the arm64 root filesystem to build against")
endif()
# try_compile projects get the toolchain file again, without the cache: pass the root on to them
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES NAVI_SYSROOT)

set(CMAKE_SYSROOT "${NAVI_SYSROOT}")
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_C_COMPILER_TARGET aarch64-linux-gnu)
set(CMAKE_CXX_COMPILER_TARGET aarch64-linux-gnu)
# Name the root's GCC explicitly. Found on its own, clang takes it through the /lib -> usr/lib symlink and
# reports headers as <root>/lib/gcc/.../../../../include/..., which ninja folds to <root>/include/...: a
# path that does not exist, so every object looked out of date and every build was a full rebuild.
set(CMAKE_C_FLAGS_INIT "--gcc-toolchain=${NAVI_SYSROOT}/usr")
set(CMAKE_CXX_FLAGS_INIT "--gcc-toolchain=${NAVI_SYSROOT}/usr")
set(CMAKE_AR llvm-ar CACHE FILEPATH "")
set(CMAKE_RANLIB llvm-ranlib CACHE FILEPATH "")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-fuse-ld=lld")

set(CMAKE_FIND_ROOT_PATH "${NAVI_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_LIBRARY_ARCHITECTURE aarch64-linux-gnu)

# pkg-config must read the root's .pc files only, and prefix what they print with the root
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${NAVI_SYSROOT}")
set(ENV{PKG_CONFIG_LIBDIR} "${NAVI_SYSROOT}/usr/lib/aarch64-linux-gnu/pkgconfig:${NAVI_SYSROOT}/usr/share/pkgconfig")
set(ENV{PKG_CONFIG_PATH} "")
