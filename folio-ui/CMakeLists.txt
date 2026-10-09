# folio-ui : the component kit, screens and platform shells.
#
# LAYERING, ENFORCED BY TARGET STRUCTURE
# --------------------------------------
#   folio-ui-core   widgets, animation, icons, theme. Depends ONLY on
#                   folio-render's interface headers - never on Skia, Qt, SDL
#                   or anything that can open a window. This is the part that
#                   survives a toolkit change.
#   folio-ui-app    screens and application state. Depends on ui-core.
#   folio-shell-*   one per platform shell. These are the only targets
#                   allowed to mention a windowing system.
#
# A component that accidentally includes <SkCanvas.h> fails to compile in
# folio-ui-core, which is the cheapest possible enforcement of the seam the
# architecture plan is built around.

# ---------------------------------------------------------------------------
# Token generation
# ---------------------------------------------------------------------------
# Tokens.h is generated from design-tokens.json. Regenerating at configure
# time (not build time) keeps the header stable for IDE indexing, and the CI
# "boundaries" job re-runs the generator and fails on a diff, so a hand edit
# cannot survive review.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_FOUND)
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/gen-tokens.py"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        RESULT_VARIABLE _tok_rc
        OUTPUT_VARIABLE _tok_out
        ERROR_VARIABLE  _tok_err)
    if(NOT _tok_rc EQUAL 0)
        message(FATAL_ERROR "token generation failed:\n${_tok_err}")
    endif()
    string(STRIP "${_tok_out}" _tok_out)
    message(STATUS "  tokens      : ${_tok_out}")
else()
    message(WARNING "Python3 not found; using the checked-in Tokens.h as-is")
endif()

# ---------------------------------------------------------------------------
# folio-ui-core
# ---------------------------------------------------------------------------
add_library(folio-ui-core STATIC
    src/Animation.cpp
    src/Widget.cpp
    src/Icons.cpp
    src/Draw.cpp
    components/Button.cpp
    components/Edits.cpp
    components/WindowChrome.cpp
    components/Controls.cpp
    components/Containers.cpp
    components/Gestures.cpp
    components/PageStage.cpp
    components/Overlays.cpp
    components/OpenDialog.cpp
    components/Annotations.cpp
)
target_include_directories(folio-ui-core PUBLIC include ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(folio-ui-core PUBLIC folio-render folio-core)
folio_enable_warnings(folio-ui-core)
add_library(Folio::UiCore ALIAS folio-ui-core)

# ---------------------------------------------------------------------------
# folio-ui-app : screens + application state
# ---------------------------------------------------------------------------
add_library(folio-ui-app STATIC
    screens/WelcomeScreen.cpp
    screens/ReaderScreen.cpp
    screens/AppState.cpp
    screens/FolioApp.cpp
)
target_include_directories(folio-ui-app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(folio-ui-app PUBLIC folio-ui-core)
folio_enable_warnings(folio-ui-app)
add_library(Folio::UiApp ALIAS folio-ui-app)

# ---------------------------------------------------------------------------
# Shells
# ---------------------------------------------------------------------------
# folio-ui-pdfium : the bridge between the widget layer and the PDF engine.
#
# Kept as its own target so neither side has to know about the other:
# folio-engine-pdfium stays free of UI headers, and folio-ui-core keeps its
# rule that it links only folio-render and folio-core. This is the only
# translation unit that includes both.
# ---------------------------------------------------------------------------
if(TARGET folio-engine-podofo)
    target_link_libraries(folio-ui-app PUBLIC folio-engine-podofo)
    target_compile_definitions(folio-ui-app PUBLIC FOLIO_HAVE_PODOFO_SOURCE=1)
endif()

if(TARGET folio-engine-pdfium)
    add_library(folio-ui-pdfium STATIC
        ../folio-engine/pdfium/PdfiumPageSource.cpp
    )
    target_link_libraries(folio-ui-pdfium PUBLIC folio-ui-core folio-engine-pdfium)
    folio_enable_warnings(folio-ui-pdfium)
    target_compile_definitions(folio-ui-pdfium PUBLIC FOLIO_HAVE_PDFIUM_SOURCE=1)
    target_link_libraries(folio-ui-app PUBLIC folio-ui-pdfium)
endif()

# ---------------------------------------------------------------------------
# The streaming shell needs no windowing system at all: it rasterises to an
# offscreen Skia surface and serves frames over HTTP. That makes it the only
# shell that can run in a headless CI container or this sandbox, and it
# doubles as the transport for remote/automated UI testing.
#
# UNIX only: the HTTP server is built directly on BSD sockets
# (<sys/socket.h>, <unistd.h>, a self-pipe for wakeups). Winsock needs a
# different accept loop and WSAStartup, which is not worth writing for a
# development transport - Windows builds the native shell instead. Without
# this guard the Windows build fails at the first #include.
if(TARGET folio-render-skia AND UNIX)
    add_library(folio-shell-stream STATIC
        shell/StreamShell.cpp
        shell/HttpServer.cpp
        shell/FrameEncoder.cpp
    )
    target_link_libraries(folio-shell-stream PUBLIC folio-ui-app folio-render-skia)
    folio_enable_warnings(folio-shell-stream)
    folio_relax_warnings(folio-shell-stream)

    add_executable(folio-stream shell/main_stream.cpp)
    target_link_libraries(folio-stream PRIVATE folio-shell-stream)
    folio_enable_warnings(folio-stream)
endif()

# ---------------------------------------------------------------------------
# folio-desktop : the shipping native shell.
#
# Always built when Skia is available, including on hosts with no windowing
# backend - there it links the "none" platform and exits with a message. That
# is deliberate: it means CI compiles and links this binary on every target,
# so the cross-platform claim is tested rather than asserted.
# ---------------------------------------------------------------------------
if(TARGET folio-render-skia)
    add_executable(folio-desktop
        shell/NativeShell.cpp
        shell/main_native.cpp
    )
    target_link_libraries(folio-desktop PRIVATE
        folio-ui-app folio-render-skia folio-platform)
    folio_enable_warnings(folio-desktop)
    folio_relax_warnings(folio-desktop)
    message(STATUS "  native shell: folio-desktop (${FOLIO_PLATFORM_BACKEND})")
endif()

# ---------------------------------------------------------------------------
# folio-android : the NativeActivity shared library
#
# Android loads a .so, not an executable, and the manifest names it in
# android.app.lib_name. Everything inside is the same NativeShell the
# desktop binary runs.
# ---------------------------------------------------------------------------
if(ANDROID AND NOT TARGET folio-render-skia)
    # Without this, the target simply does not exist and Gradle fails with
    # "Unexpected native build target folio-android", which says nothing
    # about the actual cause: prebuilts were never fetched for this ABI.
    message(FATAL_ERROR
        "folio-android needs Skia for ${CMAKE_ANDROID_ARCH_ABI}, which is "
        "not in ${FOLIO_DEPS_DIR}. Fetch it first:\n"
        "  FOLIO_TARGET=android FOLIO_ABI=<arm64|x64> "
        "bash tools/bootstrap-deps.sh")
endif()

if(ANDROID AND TARGET folio-render-skia)
    add_library(folio-android SHARED
        shell/main_android.cpp
        shell/NativeShell.cpp
        # See the note in folio-platform/CMakeLists.txt: the glue has to be
        # compiled into the .so, not linked from an archive.
        "${CMAKE_ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c")
    target_link_libraries(folio-android PRIVATE
        folio-ui-app folio-render-skia folio-platform
        android log EGL GLESv3)
    target_include_directories(folio-android PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/shell")
    folio_enable_warnings(folio-android)
    folio_relax_warnings(folio-android)
    # Belt and braces: even compiled in, --gc-sections can take an export
    # that nothing references from inside the library.
    target_link_options(folio-android PRIVATE
        "LINKER:--undefined=ANativeActivity_onCreate")
endif()
