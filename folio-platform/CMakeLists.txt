# folio-platform : windowing, GL context, clipboard, native file dialogs.
#
# Exactly one backend is compiled per platform. The library exists even when
# no backend is available (a headless CI container, this sandbox): it then
# compiles the "none" backend, whose createWindow() fails with a readable
# message. That keeps every downstream target buildable everywhere, which is
# what makes the cross-platform claim checkable rather than aspirational.

set(_folio_platform_sources src/PlatformNone.cpp)
set(_folio_platform_libs)
set(_folio_platform_backend "none")

# A backend is selected only when its source file actually exists. Pointing
# CMake at a file that has not been written yet turns "not implemented" into
# a configure error on that platform, which is a much worse failure than
# falling back to the "none" backend and saying so.
if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/win32/Win32Window.cpp")
    set(_folio_platform_sources win32/Win32Window.cpp)
    set(_folio_platform_libs user32 gdi32 opengl32 shell32 ole32 shlwapi dwmapi)
    set(_folio_platform_backend "win32")

elseif(APPLE AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cocoa/CocoaWindow.mm")
    set(_folio_platform_sources cocoa/CocoaWindow.mm)
    find_library(COCOA_LIB Cocoa REQUIRED)
    find_library(QUARTZCORE_LIB QuartzCore REQUIRED)
    find_library(OPENGL_LIB OpenGL REQUIRED)
    set(_folio_platform_libs ${COCOA_LIB} ${QUARTZCORE_LIB} ${OPENGL_LIB})
    set(_folio_platform_backend "cocoa")

elseif(UNIX AND NOT APPLE AND NOT ANDROID AND NOT EMSCRIPTEN)
    # X11 and EGL are resolved by hand rather than through find_package so a
    # sandbox with headers unpacked outside /usr can point at them with
    # FOLIO_GLDEV_ROOT. CI uses the system packages and ignores it.
    find_package(X11)
    find_path(FOLIO_EGL_INCLUDE_DIR EGL/egl.h
        HINTS "${FOLIO_GLDEV_ROOT}/usr/include" ENV FOLIO_GLDEV_ROOT)
    find_library(FOLIO_EGL_LIB NAMES EGL
        HINTS "${FOLIO_GLDEV_ROOT}/usr/lib/x86_64-linux-gnu"
              "${FOLIO_GLDEV_ROOT}/usr/lib" ENV FOLIO_GLDEV_ROOT)
    # libGL as well as libEGL: Skia's prebuilt archive contains
    # GrGLMakeGLXInterface.cpp whether or not we use GLX, so glXGetProcAddress
    # and glXGetCurrentContext have to resolve at link time.
    find_library(FOLIO_GL_LIB NAMES GL
        HINTS "${FOLIO_GLDEV_ROOT}/usr/lib/x86_64-linux-gnu"
              "${FOLIO_GLDEV_ROOT}/usr/lib" ENV FOLIO_GLDEV_ROOT)

    if(X11_FOUND AND FOLIO_EGL_INCLUDE_DIR AND FOLIO_EGL_LIB)
        set(_folio_platform_sources linux/X11Window.cpp)
        # --no-as-needed around the GL libraries.
        #
        # Skia's prebuilt archive is listed after these on the link line and
        # contains GrGLMakeGLXInterface.cpp, so the GLX symbols only become
        # undefined *after* the linker has already decided whether to keep
        # libGL. Under Debian's default --as-needed it has dropped it by
        # then, and the link fails on glXGetCurrentContext. Pinning them
        # keeps both libraries resolvable for everything that follows.
        set(_folio_platform_libs
            ${X11_LIBRARIES}
            -Wl,--no-as-needed ${FOLIO_EGL_LIB})
        if(FOLIO_GL_LIB)
            list(APPEND _folio_platform_libs ${FOLIO_GL_LIB})
        endif()
        list(APPEND _folio_platform_libs -Wl,--as-needed)
        set(_folio_platform_backend "x11")
    endif()
endif()

add_library(folio-platform STATIC ${_folio_platform_sources})
target_include_directories(folio-platform PUBLIC include)
target_link_libraries(folio-platform PUBLIC ${_folio_platform_libs})

if(_folio_platform_backend STREQUAL "x11")
    target_include_directories(folio-platform PRIVATE
        ${X11_INCLUDE_DIR} ${FOLIO_EGL_INCLUDE_DIR})
endif()

# Xlib's macros and the EGL headers both trip -Wold-style-cast and friends.
# The warning set is kept for our own code and relaxed only for the backends,
# which are thin and mostly consist of calls into C APIs.
folio_enable_warnings(folio-platform)
if(MSVC)
    # The relaxations below are GCC/Clang spellings; MSVC rejects unknown
    # /W flags outright rather than ignoring them. Its own noise here comes
    # from the SDK headers, which /external: cannot reach in a static lib.
    target_compile_options(folio-platform PRIVATE /wd4100 /wd4244 /wd4267)
    target_compile_definitions(folio-platform PRIVATE
        WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
else()
    target_compile_options(folio-platform PRIVATE
        -Wno-conversion -Wno-old-style-cast -Wno-sign-conversion -Wno-shadow)
endif()

set(FOLIO_PLATFORM_BACKEND "${_folio_platform_backend}" CACHE INTERNAL "")
message(STATUS "  platform    : ${_folio_platform_backend}")

add_library(Folio::Platform ALIAS folio-platform)
