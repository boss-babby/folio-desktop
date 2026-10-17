# FolioOptions.cmake  --  feature flags and per-platform defaults

# --- platform detection ----------------------------------------------------
if(EMSCRIPTEN)
    set(FOLIO_PLATFORM "wasm")
elseif(ANDROID)
    set(FOLIO_PLATFORM "android")
elseif(IOS)
    set(FOLIO_PLATFORM "ios")
elseif(APPLE)
    set(FOLIO_PLATFORM "macos")
elseif(WIN32)
    set(FOLIO_PLATFORM "windows")
else()
    set(FOLIO_PLATFORM "linux")
endif()

set(FOLIO_MOBILE  OFF)
set(FOLIO_DESKTOP OFF)
if(FOLIO_PLATFORM STREQUAL "ios" OR FOLIO_PLATFORM STREQUAL "android")
    set(FOLIO_MOBILE ON)
elseif(NOT FOLIO_PLATFORM STREQUAL "wasm")
    set(FOLIO_DESKTOP ON)
endif()

# --- build options ---------------------------------------------------------
option(FOLIO_BUILD_UI     "Build the UI and shell"       ON)
option(FOLIO_BUILD_TESTS  "Build tests"                  ON)
# Windows/Android do not ship PoDoFo by default. An explicit ON on Windows
# must resolve a library or configure will fail instead of silently dropping it.
if(WIN32 OR ANDROID)
    set(_folio_podofo_default OFF)
else()
    set(_folio_podofo_default ON)
endif()
option(FOLIO_WITH_PODOFO "Enable optional PoDoFo editing engine" ${_folio_podofo_default})
option(FOLIO_BUILD_FUZZ   "Build libFuzzer targets"      OFF)

# PDFium feature flags. XFA and V8 together add ~20-25 MB to the binary and a
# large attack surface. Default off everywhere; opt in on desktop only if
# JavaScript form actions are a hard requirement (plan §13.2, §17).
option(FOLIO_PDFIUM_XFA   "PDFium XFA forms"             OFF)
option(FOLIO_PDFIUM_V8    "PDFium JavaScript"            OFF)
option(FOLIO_PDFIUM_SKIA  "PDFium Skia backend"          OFF)  # see Appendix C

# Digital signing needs OpenSSL, which is heavy under Emscripten. Web ships
# as reader + annotator + organiser (plan §13.3).
if(FOLIO_PLATFORM STREQUAL "wasm")
    option(FOLIO_ENABLE_SIGNING "PAdES digital signatures" OFF)
    option(FOLIO_WASM_MINIMAL   "Drop signing and at-rest encryption" ON)
else()
    option(FOLIO_ENABLE_SIGNING "PAdES digital signatures" ON)
    set(FOLIO_WASM_MINIMAL OFF)
endif()

# --- memory budgets (plan §8.4) --------------------------------------------
if(FOLIO_DESKTOP)
    set(FOLIO_RASTER_CACHE_BYTES  268435456)  # 256 MB
    set(FOLIO_TEXTURE_CACHE_BYTES 402653184)  # 384 MB
elseif(FOLIO_MOBILE)
    set(FOLIO_RASTER_CACHE_BYTES  100663296)  #  96 MB
    set(FOLIO_TEXTURE_CACHE_BYTES 134217728)  # 128 MB
else()
    set(FOLIO_RASTER_CACHE_BYTES   67108864)  #  64 MB
    set(FOLIO_TEXTURE_CACHE_BYTES 100663296)  #  96 MB
endif()

function(folio_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /Zc:__cplusplus)
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-unused-parameter)
    endif()
endfunction()

# Targets that include Skia's headers cannot hold the project's own warning
# level: Skia is full of implicit conversions and shadowed members, and they
# are not ours to fix.
#
# This has to be a function rather than five copies of the flags, because
# MSVC *rejects* an unrecognised /W-style flag outright instead of warning
# about it the way GCC tolerates an unknown -Wno-*. Passing -Wno-conversion
# to cl.exe fails the build.
function(folio_relax_warnings target)
    if(MSVC)
        # C4244/C4267 narrowing, C4100 unused formal, C4458 shadows member.
        target_compile_options(${target} PRIVATE /wd4244 /wd4267 /wd4100 /wd4458)
    else()
        target_compile_options(${target} PRIVATE -Wno-conversion -Wno-shadow)
    endif()
endfunction()
