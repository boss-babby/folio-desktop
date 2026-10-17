cmake_minimum_required(VERSION 3.28)

# C is enabled for exactly one file: the NDK's android_native_app_glue.c.
# With LANGUAGES CXX alone, CMake does not error on a .c source - it
# silently drops it, the .so links without ANativeActivity_onCreate, and
# Android refuses to start the app with no clue as to why.
project(Folio
    VERSION 0.1.0
    DESCRIPTION "Folio - PDF reader and editor"
    LANGUAGES CXX C)

if(WIN32)
    # These must be set for EVERY target, not just the ones that include
    # <windows.h> directly. PDFium's public headers pull it in, and
    # <windows.h> defines min() and max() as macros, so a perfectly normal
    # std::max(0, n) elsewhere in the translation unit expands to
    # std::(((0) > (n)) ? (0) : (n)) and fails with "illegal token on the
    # right side of ::" - an error that points at our code and says
    # nothing about its real cause.
    #
    # Set here, immediately after project(), because add_compile_definitions
    # only applies to targets created afterwards.
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
    # getenv, sscanf and friends are used deliberately and portably.
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)

    # Use the static MSVC runtime (/MT) for every target. The prebuilt Skia
    # from skia-pack is built with /MT, and MSVC refuses to link objects
    # built with the dynamic runtime (/MD) against it. Without this, every
    # object that touches Skia fails with LNK2038 'RuntimeLibrary' mismatch,
    # which is what produced the thousands of identical link errors on the
    # first Windows CI run. PDFium ships as a DLL with its own runtime, so it
    # is not affected. Set for all configurations: only Release Skia is
    # fetched, so a Debug /MDd build would mismatch the same way.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(ANDROID)
    # Gradle can limit its workers but externalNativeBuild runs Ninja itself.
    # Several simultaneous Clang/Skia translations exceed a 2 GiB builder;
    # job pools keep native peak memory bounded without penalising desktop.
    set_property(GLOBAL PROPERTY JOB_POOLS folio_compile=1 folio_link=1)
    set(CMAKE_JOB_POOL_COMPILE folio_compile)
    set(CMAKE_JOB_POOL_LINK folio_link)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

find_package(Threads REQUIRED)

include(FolioOptions)
include(FolioDependencies)
include(FolioCoreGuard)

# A Windows "successful" build without the reader or renderer is not a
# deliverable. Other platforms retain the headless core-only fallback.
if(WIN32 AND FOLIO_WITH_PODOFO AND NOT FOLIO_HAVE_PODOFO)
    message(FATAL_ERROR
        "PoDoFo was requested but no compatible library was found; "
        "install with vcpkg or use -DFOLIO_WITH_PODOFO=OFF")
endif()

if(WIN32 AND FOLIO_BUILD_UI AND (NOT FOLIO_HAVE_PDFIUM OR NOT FOLIO_HAVE_SKIA))
    message(FATAL_ERROR
        "Windows UI requires both PDFium and Skia prebuilts; run "
        "tools/bootstrap-deps.ps1 and check FOLIO_DEPS_DIR")
endif()

# ---------------------------------------------------------------------------
# folio-core : portable C++20. No Qt, no Skia, no platform headers. Unit
# testable without a display server, a GPU, or any third-party library.
# FolioCoreGuard enforces that mechanically (WP A8).
#
# Everything expensive to get right - the coordinate math, the cache eviction
# policy, the scheduler's priority and cancellation rules - lives here on
# purpose, because here it can be tested exhaustively and deterministically.
# ---------------------------------------------------------------------------
add_library(folio-core STATIC
    folio-core/src/engine/DocumentSnapshot.cpp
    folio-core/src/render/TileGrid.cpp
    folio-core/src/render/RasterCache.cpp        # WP C2
    folio-core/src/render/TileScheduler.cpp      # WP C4
    # folio-core/src/session/EditJournal.cpp     WP D3
    # folio-core/src/text/SearchService.cpp      WP D6
)
target_include_directories(folio-core PUBLIC folio-core/include)
target_compile_features(folio-core PUBLIC cxx_std_20)
target_link_libraries(folio-core PUBLIC Threads::Threads)
folio_enable_warnings(folio-core)
folio_guard_core_purity(folio-core)

target_compile_definitions(folio-core PUBLIC
    FOLIO_RASTER_CACHE_BYTES=${FOLIO_RASTER_CACHE_BYTES}
    FOLIO_TEXTURE_CACHE_BYTES=${FOLIO_TEXTURE_CACHE_BYTES})

# ---------------------------------------------------------------------------
# folio-engine : the only target that links PDFium and PoDoFo.
#
# Split in two so the halves can advance independently. The rasterizer needs
# only PDFium; the document store needs only PoDoFo. Neither is required for
# folio-core's tests to pass, which is why the core was finishable before the
# binaries were vendored.
# ---------------------------------------------------------------------------
if(FOLIO_HAVE_PDFIUM)
    add_library(folio-engine-pdfium STATIC
        folio-engine/pdfium/PdfiumLibrary.cpp     # WP B2
        folio-engine/pdfium/PdfiumRasterizer.cpp  # WP B3 / B4
        folio-engine/pdfium/PdfiumWorker.cpp     # async render + text thread
        folio-engine/pdfium/PdfiumAnnotationWriter.cpp
    )
    # PDFium is PUBLIC because PdfiumRasterizer.h includes <fpdfview.h> for
    # FPDF_DOCUMENT. That is deliberate: hiding it behind a pimpl would cost
    # an indirection on the hottest path in the application to protect a
    # boundary that folio-core already protects properly.
    if(FOLIO_HAVE_SKIA)
        target_sources(folio-engine-pdfium PRIVATE folio-engine/pdfium/PdfiumOutlinedText.cpp)
        target_include_directories(folio-engine-pdfium PRIVATE "${FOLIO_DEPS_DIR}/skia${_folio_dep_suffix}/third_party/externals/harfbuzz/src")
        target_link_libraries(folio-engine-pdfium PRIVATE Skia::Skia)
        target_compile_definitions(folio-engine-pdfium PRIVATE FOLIO_SHAPED_PDF_TEXT=1)
    endif()
    target_include_directories(folio-engine-pdfium PUBLIC folio-engine)
    target_link_libraries(folio-engine-pdfium PUBLIC folio-core PDFium::PDFium)
    folio_enable_warnings(folio-engine-pdfium)
    add_library(Folio::EnginePdfium ALIAS folio-engine-pdfium)
endif()

if(FOLIO_HAVE_PODOFO)
    add_library(folio-engine-podofo STATIC
        folio-engine/podofo/PodofoDocument.cpp    # WP B6 / B7
    )
    target_include_directories(folio-engine-podofo PUBLIC folio-engine)
    target_link_libraries(folio-engine-podofo PUBLIC folio-core PRIVATE podofo::podofo)
    # PoDoFo 0.9.8's headers predate C++17: std::iterator, throw() specs and
    # unqualified NULL comparisons throughout. Not ours to fix.
    #
    # MSVC rejects unknown /W-style flags outright (D8021) rather than
    # ignoring them the way GCC tolerates an unrecognised -Wno-*, so these
    # must never reach cl.exe.
    if(MSVC)
        target_compile_options(folio-engine-podofo PRIVATE
            /wd4996 /wd4244 /wd4267 /wd4458)
    else()
        target_compile_options(folio-engine-podofo PRIVATE
            -Wno-deprecated-declarations -Wno-conversion -Wno-shadow
            -Wno-old-style-cast)
    endif()
    folio_enable_warnings(folio-engine-podofo)
    add_library(Folio::EnginePodofo ALIAS folio-engine-podofo)
endif()

# ---------------------------------------------------------------------------
# folio-render : the backend-neutral paint layer.
#
# folio-render/include defines IPaintTarget and the scene vocabulary. Nothing
# in it mentions Skia, Qt or OpenGL. Backends implement it:
#
#   folio-render/skia/   SkCanvas             (built here when Skia is found)
#   folio-render/qt/     QSGNode / QPainter   (plan §7, when Qt is present)
#
# This interface is what makes the Qt-versus-Skia decision reversible instead
# of terminal. The screens, the component kit and the animation driver all
# paint through it, so swapping backends recompiles one directory rather than
# the whole application.
# ---------------------------------------------------------------------------
add_library(folio-render INTERFACE)
target_include_directories(folio-render INTERFACE folio-render/include)
target_link_libraries(folio-render INTERFACE folio-core)

if(FOLIO_HAVE_SKIA)
    add_library(folio-render-skia STATIC
        folio-render/skia/SkiaPaintTarget.cpp
        folio-render/skia/SkiaTileCompositor.cpp
        folio-render/skia/PageThemeShader.cpp
        folio-render/skia/SkiaTextSystem.cpp
        folio-render/skia/ShadowCache.cpp
        folio-render/skia/SkiaGpuTarget.cpp
    )
    target_include_directories(folio-render-skia PUBLIC folio-render folio-render/skia)
    # SkiaGpuTarget is the one file here that sees a platform header (for
    # IGlContext). It still does not see a windowing header: the platform
    # layer hands it function pointers, nothing more.
    target_link_libraries(folio-render-skia PUBLIC folio-render folio-core Skia::Skia folio-platform)

    # Skia's prebuilt archive always contains GrGLMakeGLXInterface.cpp,
    # whether or not Folio uses GLX and whichever platform backend is
    # selected - so glXGetCurrentContext has to resolve even in a headless
    # build with the "none" backend. This is Skia's dependency, not the
    # platform layer's, so it is declared here.
    #
    # --no-as-needed because the archive is listed after these on the link
    # line: under Debian's default the linker has already discarded libGL by
    # the time those symbols come up undefined.
    if(UNIX AND NOT APPLE AND NOT ANDROID AND NOT EMSCRIPTEN)
        find_library(FOLIO_SKIA_GL_LIB NAMES GL
            HINTS "${FOLIO_GLDEV_ROOT}/usr/lib/x86_64-linux-gnu"
                  "${FOLIO_GLDEV_ROOT}/usr/lib" ENV FOLIO_GLDEV_ROOT)
        if(FOLIO_SKIA_GL_LIB)
            target_link_libraries(folio-render-skia PUBLIC
                -Wl,--no-as-needed ${FOLIO_SKIA_GL_LIB} -Wl,--as-needed)
        endif()
    endif()
    # Where the bundled DM Sans lives at runtime. Overridable so an installed
    # bundle can point at its own resources directory.
    if(NOT DEFINED FOLIO_ASSET_DIR)
        set(FOLIO_ASSET_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets")
    endif()
    target_compile_definitions(folio-render-skia PUBLIC
        FOLIO_ASSET_DIR="${FOLIO_ASSET_DIR}")
    folio_enable_warnings(folio-render-skia)
    # Skia's own headers are not -Wconversion clean and we do not control
    # them. folio_relax_warnings picks the right spelling per compiler;
    # passing -Wno-conversion directly fails the MSVC build with D8021.
    folio_relax_warnings(folio-render-skia)
    add_library(Folio::RenderSkia ALIAS folio-render-skia)
endif()

# ---------------------------------------------------------------------------
# UI and shell : skipped for headless/CI-core builds and for the corpus runner.
# ---------------------------------------------------------------------------
if(FOLIO_BUILD_UI AND EXISTS "${CMAKE_SOURCE_DIR}/folio-ui/CMakeLists.txt")
    add_subdirectory(folio-platform)
    add_subdirectory(folio-ui)
    folio_guard_design_tokens()
endif()

if(FOLIO_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(EXISTS "${CMAKE_SOURCE_DIR}/tools/CMakeLists.txt")
    add_subdirectory(tools)
endif()

message(STATUS "")
message(STATUS "Folio ${PROJECT_VERSION}")
message(STATUS "  platform    : ${FOLIO_PLATFORM}")
message(STATUS "  compiler    : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  UI          : ${FOLIO_BUILD_UI}")
message(STATUS "  tests       : ${FOLIO_BUILD_TESTS}")
message(STATUS "  PDFium XFA  : ${FOLIO_PDFIUM_XFA}")
message(STATUS "  PDFium V8   : ${FOLIO_PDFIUM_V8}")
message(STATUS "  signing     : ${FOLIO_ENABLE_SIGNING}")
message(STATUS "  raster cache: ${FOLIO_RASTER_CACHE_BYTES} bytes")
message(STATUS "")