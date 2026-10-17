# tests/CMakeLists.txt
#
# No GoogleTest, no Catch2. The suites are plain executables with a main() and
# a nonzero exit code on failure, registered with ctest.
#
# That is a deliberate choice, not laziness. These tests must run on six
# platforms including WebAssembly and a bare iOS simulator, inside a corpus
# runner with no network, and - as actually happened on this project - on a
# machine that had g++ and nothing else. A test harness with its own dependency
# graph is a liability in that situation. The cost is losing fixtures and
# parameterised cases; the benefit is that `ctest` works anywhere a compiler
# does. Revisit if the suite passes ~5k cases.

add_executable(folio-test-core core/test_document_snapshot.cpp)
target_link_libraries(folio-test-core PRIVATE folio-core)
folio_enable_warnings(folio-test-core)
add_test(NAME core.snapshot COMMAND folio-test-core)

# XI2 valuator decoding is a pure header with no X11 dependency, so it is
# tested everywhere, not only where libXi happens to be installed.
add_executable(folio-test-xi-valuator platform/test_xi_valuator.cpp)
folio_enable_warnings(folio-test-xi-valuator)
add_test(NAME platform.xi.valuator COMMAND folio-test-xi-valuator)

add_executable(folio-test-tilegrid render/test_tile_grid.cpp)
target_link_libraries(folio-test-tilegrid PRIVATE folio-core)
folio_enable_warnings(folio-test-tilegrid)
add_test(NAME render.tilegrid COMMAND folio-test-tilegrid)

add_executable(folio-test-rastercache render/test_raster_cache.cpp)
target_link_libraries(folio-test-rastercache PRIVATE folio-core)
folio_enable_warnings(folio-test-rastercache)
add_test(NAME render.rastercache COMMAND folio-test-rastercache)

add_executable(folio-test-scheduler render/test_tile_scheduler.cpp)
target_link_libraries(folio-test-scheduler PRIVATE folio-core)
folio_enable_warnings(folio-test-scheduler)
add_test(NAME render.scheduler COMMAND folio-test-scheduler)

# --- tests that need a real PDFium -----------------------------------------
if(FOLIO_HAVE_PDFIUM)
    add_executable(folio-test-pdfium-export engine/test_pdfium_export.cpp)
    target_compile_definitions(folio-test-pdfium-export PRIVATE FOLIO_TEST_FONT_DIR="${PROJECT_SOURCE_DIR}/assets/fonts")
    target_link_libraries(folio-test-pdfium-export PRIVATE folio-engine-pdfium PDFium::PDFium)
    folio_enable_warnings(folio-test-pdfium-export)
    add_test(NAME engine.pdfium.export COMMAND folio-test-pdfium-export
             WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/data)
    set_tests_properties(engine.pdfium.export PROPERTIES
        ENVIRONMENT "LD_LIBRARY_PATH=${FOLIO_DEPS_DIR}/pdfium/lib:$ENV{LD_LIBRARY_PATH}")
    if(FOLIO_HAVE_PODOFO)
        add_executable(folio-test-podofo engine/test_podofo_roundtrip.cpp)
        target_link_libraries(folio-test-podofo
            PRIVATE folio-engine-podofo folio-engine-pdfium PDFium::PDFium)
        add_test(NAME engine.podofo.roundtrip COMMAND folio-test-podofo
                 WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/data)
    endif()

    add_executable(folio-test-pdfium-corpus engine/test_pdfium_corpus.cpp)
    target_link_libraries(folio-test-pdfium-corpus PRIVATE folio-engine-pdfium PDFium::PDFium)
    add_test(NAME engine.pdfium.corpus COMMAND folio-test-pdfium-corpus
             WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/data)

    add_executable(folio-test-pdfium engine/test_pdfium_rasterizer.cpp)
    target_link_libraries(folio-test-pdfium PRIVATE folio-engine-pdfium PDFium::PDFium)
    folio_enable_warnings(folio-test-pdfium)
    add_test(NAME engine.pdfium COMMAND folio-test-pdfium
             WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/data)
    # The imported PDFium is a shared library living outside any standard
    # search path, so the test needs it on the loader path at run time.
    set_tests_properties(engine.pdfium PROPERTIES
        ENVIRONMENT "LD_LIBRARY_PATH=${FOLIO_DEPS_DIR}/pdfium/lib:$ENV{LD_LIBRARY_PATH}")
endif()

# --- tests that need Skia ---------------------------------------------------
if(FOLIO_HAVE_SKIA)
    add_executable(folio-test-skia render/test_skia_compositor.cpp)
    target_link_libraries(folio-test-skia PRIVATE folio-render-skia)
    folio_enable_warnings(folio-test-skia)
    folio_relax_warnings(folio-test-skia)
    add_test(NAME render.skia COMMAND folio-test-skia)

    add_executable(folio-test-pagecache render/test_skia_page_cache.cpp)
    target_link_libraries(folio-test-pagecache PRIVATE folio-render-skia)
    folio_enable_warnings(folio-test-pagecache)
    folio_relax_warnings(folio-test-pagecache)
    add_test(NAME render.pagecache COMMAND folio-test-pagecache)
    set_tests_properties(render.skia PROPERTIES
        ENVIRONMENT "FOLIO_GOLDEN_DIR=${CMAKE_CURRENT_SOURCE_DIR}/golden")
endif()

# --- the full pipeline: PDFium raster -> cache -> Skia composite ------------
if(FOLIO_HAVE_PDFIUM AND FOLIO_HAVE_SKIA)
    add_executable(folio-test-pipeline integration/test_render_pipeline.cpp)
    target_link_libraries(folio-test-pipeline
        PRIVATE folio-engine-pdfium folio-render-skia PDFium::PDFium)
    folio_enable_warnings(folio-test-pipeline)
    folio_relax_warnings(folio-test-pipeline)
    add_test(NAME integration.pipeline COMMAND folio-test-pipeline
             WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/data)
    set_tests_properties(integration.pipeline PROPERTIES
        ENVIRONMENT "LD_LIBRARY_PATH=${FOLIO_DEPS_DIR}/pdfium/lib:$ENV{LD_LIBRARY_PATH}")
endif()

# --- UI tests (only when the UI is built) ----------------------------------
# folio-ui-core deliberately links only folio-render + folio-core, so this
# suite exercises the icon parser without dragging in Skia.
if(FOLIO_BUILD_UI)
    add_executable(folio-test-navigation-recents ui/test_navigation_recents.cpp)
    target_link_libraries(folio-test-navigation-recents PRIVATE folio-ui-app)
    target_compile_definitions(folio-test-navigation-recents PRIVATE
        FOLIO_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
    folio_enable_warnings(folio-test-navigation-recents)
    add_test(NAME ui.navigation-recents COMMAND folio-test-navigation-recents)
    if(FOLIO_HAVE_PDFIUM AND UNIX AND NOT APPLE)
        set_tests_properties(ui.navigation-recents PROPERTIES
            ENVIRONMENT "LD_LIBRARY_PATH=${FOLIO_DEPS_DIR}/pdfium/lib:$ENV{LD_LIBRARY_PATH}")
    endif()
    if(FOLIO_HAVE_SKIA)
        add_executable(folio-test-partial-redraw ui/test_partial_redraw.cpp)
        target_link_libraries(folio-test-partial-redraw PRIVATE folio-ui-app folio-render-skia)
        folio_enable_warnings(folio-test-partial-redraw)
        add_test(NAME ui.partial-redraw COMMAND folio-test-partial-redraw)
        if(FOLIO_HAVE_PDFIUM AND UNIX AND NOT APPLE)
            set_tests_properties(ui.partial-redraw PROPERTIES
                ENVIRONMENT "LD_LIBRARY_PATH=${FOLIO_DEPS_DIR}/pdfium/lib:$ENV{LD_LIBRARY_PATH}")
        endif()
    endif()
    if(FOLIO_HAVE_PDFIUM)
        add_executable(folio-test-recent-covers ui/test_recent_covers.cpp)
        target_link_libraries(folio-test-recent-covers PRIVATE folio-ui-app PDFium::PDFium)
        folio_enable_warnings(folio-test-recent-covers)
        add_test(NAME ui.recent-covers COMMAND folio-test-recent-covers
                 WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/data)
        if(UNIX AND NOT APPLE)
            set_tests_properties(ui.recent-covers PROPERTIES
                ENVIRONMENT "LD_LIBRARY_PATH=${FOLIO_DEPS_DIR}/pdfium/lib:$ENV{LD_LIBRARY_PATH}")
        endif()
        add_executable(folio-test-pdfium-page-source ui/test_pdfium_page_source.cpp)
        target_link_libraries(folio-test-pdfium-page-source
            PRIVATE folio-ui-pdfium folio-engine-pdfium PDFium::PDFium)
        folio_enable_warnings(folio-test-pdfium-page-source)
        add_test(NAME ui.pdfium.zoom-eviction COMMAND folio-test-pdfium-page-source
                 WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/data)
        add_executable(folio-test-pdfium-visible ui/test_pdfium_visible_tiles.cpp)
        target_link_libraries(folio-test-pdfium-visible
            PRIVATE folio-ui-pdfium folio-engine-pdfium PDFium::PDFium)
        folio_enable_warnings(folio-test-pdfium-visible)
        add_test(NAME ui.pdfium.visible-tiles COMMAND folio-test-pdfium-visible
                 WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/data)
        if(UNIX AND NOT APPLE)
            set_tests_properties(ui.pdfium.visible-tiles PROPERTIES
                ENVIRONMENT "LD_LIBRARY_PATH=${FOLIO_DEPS_DIR}/pdfium/lib:$ENV{LD_LIBRARY_PATH}")
        endif()
        if(UNIX AND NOT APPLE)
            set_tests_properties(ui.pdfium.zoom-eviction PROPERTIES
                ENVIRONMENT "LD_LIBRARY_PATH=${FOLIO_DEPS_DIR}/pdfium/lib:$ENV{LD_LIBRARY_PATH}")
        endif()
    endif()
    add_executable(folio-test-icons ui/test_icons.cpp)
    target_link_libraries(folio-test-icons PRIVATE folio-ui-core)
    folio_enable_warnings(folio-test-icons)
    add_test(NAME ui.icons COMMAND folio-test-icons)

    if(FOLIO_HAVE_PDFIUM AND NOT FOLIO_HAVE_PODOFO AND NOT ANDROID)
        add_executable(folio-test-saveas ui/test_pdfium_saveas.cpp)
        target_link_libraries(folio-test-saveas PRIVATE folio-ui-app folio-engine-pdfium)
        folio_enable_warnings(folio-test-saveas)
        add_test(NAME ui.pdfium.saveas COMMAND folio-test-saveas
                 WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/data)
        if(UNIX AND NOT APPLE)
            set_tests_properties(ui.pdfium.saveas PROPERTIES
                ENVIRONMENT "LD_LIBRARY_PATH=${FOLIO_DEPS_DIR}/pdfium/lib:$ENV{LD_LIBRARY_PATH}")
        endif()
    endif()

    add_executable(folio-test-mobile-input ui/test_mobile_input.cpp)
    target_link_libraries(folio-test-mobile-input PRIVATE folio-ui-core)
    folio_enable_warnings(folio-test-mobile-input)
    add_test(NAME ui.mobile.input COMMAND folio-test-mobile-input)
endif()
