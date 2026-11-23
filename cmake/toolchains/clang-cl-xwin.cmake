# Cross-compiles XValkyrie for 64-bit Windows from Linux with clang-cl and lld-link.
# The Windows SDK and the MSVC CRT come from `xwin splat` (https://github.com/Jake-Shadle/xwin);
# XWIN_DIR is the folder it wrote (default ~/.local/share/xwin).
#
#   cmake -S . -B build/linux -G Ninja -DCMAKE_BUILD_TYPE=Release \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/clang-cl-xwin.cmake
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_VERSION 10.0)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

if(NOT XWIN_DIR)
  if(DEFINED ENV{XWIN_DIR})
    set(XWIN_DIR "$ENV{XWIN_DIR}")
  else()
    set(XWIN_DIR "$ENV{HOME}/.local/share/xwin")
  endif()
endif()
set(XWIN_DIR "${XWIN_DIR}" CACHE PATH "Output folder of xwin splat")
# try_compile runs this file again in a fresh project: hand it the folder.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES XWIN_DIR)

set(CMAKE_C_COMPILER clang-cl)
set(CMAKE_CXX_COMPILER clang-cl)
set(CMAKE_LINKER lld-link)
set(CMAKE_AR llvm-lib)
set(CMAKE_RC_COMPILER llvm-rc)
set(CMAKE_MT llvm-mt)

set(xwin_includes
  "${XWIN_DIR}/crt/include" "${XWIN_DIR}/sdk/include/ucrt" "${XWIN_DIR}/sdk/include/um"
  "${XWIN_DIR}/sdk/include/shared" "${XWIN_DIR}/sdk/include/winrt" "${XWIN_DIR}/sdk/include/cppwinrt")
set(xwin_flags "--target=x86_64-pc-windows-msvc -fms-compatibility-version=19.40")
foreach(dir IN LISTS xwin_includes)
  string(APPEND xwin_flags " /imsvc \"${dir}\"")
endforeach()
set(CMAKE_C_FLAGS_INIT "${xwin_flags}")
set(CMAKE_CXX_FLAGS_INIT "${xwin_flags}")

set(xwin_link "/libpath:\"${XWIN_DIR}/crt/lib/x86_64\" /libpath:\"${XWIN_DIR}/sdk/lib/um/x86_64\" /libpath:\"${XWIN_DIR}/sdk/lib/ucrt/x86_64\"")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${xwin_link}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${xwin_link}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${xwin_link}")
set(CMAKE_RC_FLAGS_INIT "/I \"${XWIN_DIR}/sdk/include/um\" /I \"${XWIN_DIR}/sdk/include/shared\"")

set(CMAKE_FIND_ROOT_PATH "${XWIN_DIR}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
