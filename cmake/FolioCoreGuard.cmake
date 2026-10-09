# FolioCoreGuard.cmake  --  work package A8
#
# Mechanically enforces the boundary rule from architecture plan §4.1:
#
#     "No Qt type crosses into folio-core."
#
# This single rule is what keeps the UI toolkit swappable. Given that Qt
# requires a commercial licence for iOS and WebAssembly (plan §2.2), the
# ability to move the view layer to Slint without touching the application
# is worth real money. It costs an hour to add on day one and is effectively
# impossible to retrofit once 200 files have crept across the line.
#
# Also enforces §11: no hard-coded colours or durations in folio-ui.

set(FOLIO_FORBIDDEN_IN_CORE
    "#include *<Q"            # <QString>, <QObject>, ...
    "#include *\"Q"           # "QQuickItem.h"
    "Q_OBJECT"
    "Q_PROPERTY"
    "QT_BEGIN_NAMESPACE"
)

function(folio_guard_core_purity target)
    add_custom_target(${target}-purity-check
        COMMAND ${CMAKE_COMMAND}
                -DFOLIO_SCAN_DIR=${CMAKE_SOURCE_DIR}/folio-core
                -P ${CMAKE_SOURCE_DIR}/cmake/ScanCorePurity.cmake
        COMMENT "Checking folio-core contains no toolkit types"
        VERBATIM)
    add_dependencies(${target} ${target}-purity-check)
endfunction()

# Fails if folio-ui contains a literal hex colour or a bare millisecond
# duration that should have come from design-tokens.json.
function(folio_guard_design_tokens)
    add_custom_target(design-token-check ALL
        COMMAND ${CMAKE_COMMAND}
                -DFOLIO_SCAN_DIR=${CMAKE_SOURCE_DIR}/folio-ui
                -P ${CMAKE_SOURCE_DIR}/cmake/ScanHardcodedTokens.cmake
        COMMENT "Checking folio-ui has no hard-coded colours or durations"
        VERBATIM)
endfunction()
