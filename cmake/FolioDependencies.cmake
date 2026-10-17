# FolioDependencies.cmake  --  work package A2
#
# Resolves the three third-party libraries into imported targets:
#
#   PDFium::PDFium   required   BSD-3-Clause     render, text, forms
#   Skia::Skia       optional   BSD-3-Clause     vector UI backend (Appendix C)
#   podofo::podofo   optional   MPL-2.0 elected  document model, incremental save
#
# Resolution order for each: an explicit -DFOLIO_DEPS_DIR tree produced by
# tools/fetch-deps.sh, then vcpkg / system config packages, then give up.
#
# "Optional" here means the BUILD degrades, not the product: folio-core and its
# entire test suite must configure and pass with none of the three present.
# That headless, dependency-free lane is what keeps CI fast and what let the
# geometry and cache layers be written and verified before PDFium was vendored.

include_guard(GLOBAL)

if(DEFINED ENV{FOLIO_DEPS_DIR} AND NOT "$ENV{FOLIO_DEPS_DIR}" STREQUAL "")
    set(_folio_default_deps "$ENV{FOLIO_DEPS_DIR}")
else()
    set(_folio_default_deps "$ENV{HOME}/.cache/folio-deps")
endif()
set(FOLIO_DEPS_DIR "${_folio_default_deps}" CACHE PATH
    "Directory populated by tools/bootstrap-deps.sh")

set(FOLIO_HAVE_PDFIUM OFF)
set(FOLIO_HAVE_SKIA   OFF)
set(FOLIO_HAVE_PODOFO OFF)

# ---------------------------------------------------------------------------
# PDFium
# ---------------------------------------------------------------------------
# Cross builds keep their prebuilts in target-specific directories, exactly
# as tools/bootstrap-deps.sh writes them. The host build keeps the
# unsuffixed paths it has always used.
set(_folio_dep_suffix "")
if(ANDROID)
    if(CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a")
        set(_folio_dep_suffix "-android-arm64")
    elseif(CMAKE_ANDROID_ARCH_ABI STREQUAL "x86_64")
        set(_folio_dep_suffix "-android-x64")
    else()
        message(FATAL_ERROR
            "no Folio prebuilts for Android ABI ${CMAKE_ANDROID_ARCH_ABI} "
            "(skia-pack ships arm64 and x64 only)")
    endif()
endif()

set(_pdfium_root "${FOLIO_DEPS_DIR}/pdfium${_folio_dep_suffix}")
if(EXISTS "${_pdfium_root}/include/fpdfview.h")
    # The bblanchon archives ship a shared library on every platform; static
    # builds exist only via their local build script. For iOS we will need
    # either that script or NucleusFramework's static fork (plan §2.1).
    if(WIN32)
        set(_pdfium_lib "${_pdfium_root}/bin/pdfium.dll")
        set(_pdfium_imp "${_pdfium_root}/lib/pdfium.dll.lib")
    elseif(APPLE)
        set(_pdfium_lib "${_pdfium_root}/lib/libpdfium.dylib")
    else()
        set(_pdfium_lib "${_pdfium_root}/lib/libpdfium.so")
    endif()

    if(EXISTS "${_pdfium_lib}")
        add_library(PDFium::PDFium SHARED IMPORTED GLOBAL)
        set_target_properties(PDFium::PDFium PROPERTIES
            IMPORTED_LOCATION "${_pdfium_lib}"
            INTERFACE_INCLUDE_DIRECTORIES "${_pdfium_root}/include")
        if(WIN32 AND EXISTS "${_pdfium_imp}")
            set_target_properties(PDFium::PDFium PROPERTIES IMPORTED_IMPLIB "${_pdfium_imp}")
        endif()
        set(FOLIO_HAVE_PDFIUM ON)

        if(EXISTS "${_pdfium_root}/VERSION")
            file(STRINGS "${_pdfium_root}/VERSION" _v)
            string(REGEX MATCH "MAJOR=([0-9]+)" _ "${_v}")
            set(FOLIO_PDFIUM_VERSION "${CMAKE_MATCH_1}")
        endif()
    endif()
endif()

if(NOT FOLIO_HAVE_PDFIUM)
    find_package(PDFium CONFIG QUIET)      # vcpkg / system
    if(PDFium_FOUND)
        set(FOLIO_HAVE_PDFIUM ON)
    endif()
endif()

# ---------------------------------------------------------------------------
# Skia
#
# There is no find_package(Skia). There is no .pc file, no version macro worth
# testing against, and no ABI guarantee between milestones. The only safe way
# to consume it is an exact pinned archive and a hand-written imported target -
# which is itself the strongest practical argument in the Skia-vs-toolkit
# decision, and the reason Appendix C insists on a pinned-revision policy.
#
# Link order matters for static archives with GNU ld: skparagraph -> skshaper
# -> skunicode -> skia -> codecs -> icu. Expressed below as an explicit list
# rather than INTERFACE_LINK_LIBRARIES on separate targets, because CMake will
# happily reorder the latter.
# ---------------------------------------------------------------------------
set(_skia_root "${FOLIO_DEPS_DIR}/skia${_folio_dep_suffix}")

# Static library spelling differs by toolchain: libfoo.a on Unix, foo.lib
# under MSVC. Everything below is written once against these two, so adding
# a platform is a naming question rather than a second copy of the list.
if(MSVC)
    set(_sk_pre "")
    set(_sk_ext ".lib")
else()
    set(_sk_pre "lib")
    set(_sk_ext ".a")
endif()

if(EXISTS "${_skia_root}/include/core/SkCanvas.h")
    file(GLOB _skia_outdirs "${_skia_root}/out/*")
    foreach(_d IN LISTS _skia_outdirs)
        if(EXISTS "${_d}/${_sk_pre}skia${_sk_ext}")
            set(_skia_libdir "${_d}")
            break()
        endif()
    endforeach()

    if(DEFINED _skia_libdir)
        file(STRINGS "${_skia_root}/include/core/SkMilestone.h" _ms REGEX "SK_MILESTONE [0-9]+")
        string(REGEX MATCH "[0-9]+" FOLIO_SKIA_MILESTONE "${_ms}")

        add_library(Skia::Skia INTERFACE IMPORTED GLOBAL)

        # Skia's Windows build calls into GDI for font enumeration, DirectWrite
        # for shaping fallback and opengl32 for Ganesh. These are undefined
        # symbols in the archive, not optional extras.
        set(_skia_sys "")
        if(WIN32)
            # dwrite: DWriteCreateFactory, used by SkFontMgr_New_DirectWrite.
            # d3d12 / d3dcompiler: D3D12SerializeRootSignature and D3DCompile,
            # both referenced from skia.lib in the m144 Windows prebuilt.
            set(_skia_sys opengl32 user32 gdi32 ole32 oleaut32 advapi32
                          usp10 fontsub shlwapi windowscodecs
                          dwrite d3d12 d3dcompiler)
        elseif(ANDROID)
            # Skia's Android pack calls __android_log_vprint from SkDebugf
            # and uses the platform GLES/EGL loaders. These are platform
            # libraries, not optional extras: without them the archive has
            # undefined symbols and nothing links.
            set(_skia_sys log android EGL GLESv3)
        endif()

        # Link order matters and CMake reorders INTERFACE_LINK_LIBRARIES on
        # separate targets, so the sequence is spelled out here.
        set(_skia_link "")
        foreach(_m IN ITEMS skparagraph skshaper skunicode_icu skunicode_core
                            skia skcms harfbuzz freetype2 png jpeg webp
                            webp_sse41 wuffs expat zlib icu)
            list(APPEND _skia_link
                 "${_skia_libdir}/${_sk_pre}${_m}${_sk_ext}")
        endforeach()

        # Prune entries this particular pack did not build, so a slimmer
        # archive still links instead of failing on a missing .a.
        set(_skia_link_present "")
        foreach(_l IN LISTS _skia_link)
            if(EXISTS "${_l}")
                list(APPEND _skia_link_present "${_l}")
            endif()
        endforeach()

        set_target_properties(Skia::Skia PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES
                "${_skia_root};${_skia_root}/include;${_skia_root}/modules/skparagraph/include;${_skia_root}/modules/skshaper/include;${_skia_root}/modules/skunicode/include"
            INTERFACE_COMPILE_DEFINITIONS
                "SK_GANESH;SK_GL;SK_ENABLE_SKSL;SK_UNICODE_ICU_IMPLEMENTATION"
            INTERFACE_LINK_LIBRARIES
                "${_skia_link_present};${_skia_sys};${CMAKE_DL_LIBS};Threads::Threads")
        set(FOLIO_HAVE_SKIA ON)

        # Did this pack actually build Graphite? Headers alone prove nothing.
        # Recorded as a cache variable so CI can assert on it rather than
        # discovering at link time that skgpu::graphite::Context is undefined.
        set(FOLIO_SKIA_HAS_GRAPHITE OFF)
        if(CMAKE_NM AND NOT MSVC AND EXISTS "${_skia_libdir}/libskia.a")
            execute_process(
                COMMAND ${CMAKE_NM} -C --defined-only "${_skia_libdir}/libskia.a"
                OUTPUT_VARIABLE _nm OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET RESULT_VARIABLE _nmrc)
            if(_nmrc EQUAL 0 AND _nm MATCHES "skgpu::graphite::Context")
                set(FOLIO_SKIA_HAS_GRAPHITE ON)
            endif()
        endif()
    endif()
endif()

# ---------------------------------------------------------------------------
# PoDoFo
# ---------------------------------------------------------------------------
# Two shapes are accepted. A source build installs a CMake package config;
# the Debian prebuilt that tools/bootstrap-deps.sh extracts does not, so the
# headers and shared object are located by hand.
if(FOLIO_WITH_PODOFO)
    set(_podofo_root "${FOLIO_DEPS_DIR}/podofo/install")
    if(EXISTS "${_podofo_root}")
        list(APPEND CMAKE_PREFIX_PATH "${_podofo_root}")
    endif()
    find_package(podofo CONFIG QUIET)
    if(podofo_FOUND)
        set(FOLIO_HAVE_PODOFO ON)
        set(FOLIO_PODOFO_FLAVOUR "package config")
    else()
        set(_podofo_deb "${FOLIO_DEPS_DIR}/podofo/usr")
        find_path(FOLIO_PODOFO_INCLUDE podofo/podofo.h
                  PATHS "${_podofo_deb}/include" NO_DEFAULT_PATH)
        find_library(FOLIO_PODOFO_LIB NAMES podofo
                     PATHS "${_podofo_deb}/lib" NO_DEFAULT_PATH)
        find_library(FOLIO_IDN_LIB NAMES idn
                     PATHS "${_podofo_deb}/lib/x86_64-linux-gnu" NO_DEFAULT_PATH)
        if(FOLIO_PODOFO_INCLUDE AND FOLIO_PODOFO_LIB)
            add_library(podofo::podofo UNKNOWN IMPORTED)
            set_target_properties(podofo::podofo PROPERTIES
                IMPORTED_LOCATION "${FOLIO_PODOFO_LIB}"
                INTERFACE_INCLUDE_DIRECTORIES "${FOLIO_PODOFO_INCLUDE}")
            if(FOLIO_IDN_LIB)
                set_target_properties(podofo::podofo PROPERTIES
                    INTERFACE_LINK_LIBRARIES "${FOLIO_IDN_LIB}")
            endif()
            # PoDoFo 0.9.8's headers predate C++17 and use std::iterator and
            # friends. They are not ours to fix and the warnings are noise.
            set_target_properties(podofo::podofo PROPERTIES
                INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${FOLIO_PODOFO_INCLUDE}")
            set(FOLIO_HAVE_PODOFO ON)
            set(FOLIO_PODOFO_FLAVOUR "Debian prebuilt 0.9.8")
            # The prebuilt shared objects live in the dep cache, not on the
            # loader's default path. Bake the directories into the RPATH so the
            # binaries run without LD_LIBRARY_PATH; a packaged build would
            # instead vendor the .so next to the executable.
            list(APPEND CMAKE_BUILD_RPATH
                 "${_podofo_deb}/lib" "${_podofo_deb}/lib/x86_64-linux-gnu")
            # DT_RUNPATH, which modern linkers emit by default, applies only to
            # a binary's *direct* dependencies. libpodofo needs libidn, and that
            # lookup does not consult the executable's RUNPATH - so the library
            # is reported missing despite sitting in a listed directory. DT_RPATH
            # is inherited by the whole dependency chain, which is what we want
            # for a dep cache that lives outside the loader's default paths.
            add_link_options(-Wl,--disable-new-dtags)

        endif()
    endif()
endif()

# ---------------------------------------------------------------------------
# Report. Printed every configure because silently losing a backend and
# wondering why the golden tests stopped running is a bad afternoon.
# ---------------------------------------------------------------------------
message(STATUS "Folio dependencies (${FOLIO_DEPS_DIR})")
if(FOLIO_HAVE_PDFIUM)
    message(STATUS "  PDFium  : yes (major ${FOLIO_PDFIUM_VERSION})")
else()
    message(STATUS "  PDFium  : NO  -> folio-engine disabled, core tests still run")
endif()
if(FOLIO_HAVE_SKIA)
    message(STATUS "  Skia    : yes (m${FOLIO_SKIA_MILESTONE}, graphite=${FOLIO_SKIA_HAS_GRAPHITE})")
else()
    message(STATUS "  Skia    : NO  -> Skia render backend disabled")
endif()
if(FOLIO_HAVE_PODOFO)
    message(STATUS "  PoDoFo  : yes (${FOLIO_PODOFO_FLAVOUR})")
else()
    message(STATUS "  PoDoFo  : NO  -> document store / incremental save disabled")
endif()
