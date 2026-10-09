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
    add_executable(folio-test-icons ui/test_icons.cpp)
    target_link_libraries(folio-test-icons PRIVATE folio-ui-core)
    folio_enable_warnings(folio-test-icons)
    add_test(NAME ui.icons COMMAND folio-test-icons)
endif()
