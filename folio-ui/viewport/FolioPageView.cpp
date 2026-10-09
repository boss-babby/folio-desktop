// folio-ui/viewport/FolioPageView.cpp  --  work package F1
//
// Scene-graph construction and gesture handling for the document viewport.

#include "FolioPageView.h"

#include "TexturePool.h"
#include "folio/core/session/DocumentSession.h"

#include <QElapsedTimer>
#include <QQuickWindow>
#include <QSGOpacityNode>
#include <QSGSimpleTextureNode>
#include <QSGTransformNode>
#include <QTimer>
#include <QWheelEvent>

namespace folio::ui {

using core::ColorScheme;
using core::TileGrid;

FolioPageView::FolioPageView(QQuickItem* parent) : QQuickItem(parent) {
    setFlag(ItemHasContents, true);
    setAcceptedMouseButtons(Qt::AllButtons);
    setAcceptTouchEvents(true);

    settleTimer_ = new QTimer(this);
    settleTimer_->setSingleShot(true);
    settleTimer_->setInterval(120);
    connect(settleTimer_, &QTimer::timeout, this, &FolioPageView::onScaleSettled);
}

FolioPageView::~FolioPageView() = default;

// ---------------------------------------------------------------------------
// Scene graph
// ---------------------------------------------------------------------------

QSGNode* FolioPageView::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) {
    auto* root = static_cast<QSGTransformNode*>(oldNode);
    if (!root) {
        root = new QSGTransformNode;
        textures_ = std::make_unique<TexturePool>(window());
    }

    // Scroll and live gesture scale live in ONE transform node. Updating a
    // matrix is free; rebuilding geometry is not. This is why a pinch costs
    // nothing until it settles.
    QMatrix4x4 m;
    m.translate(0.0f, static_cast<float>(-scrollY_));
    if (!qFuzzyCompare(gestureScale_, 1.0)) {
        const QPointF c = boundingRect().center();
        m.translate(static_cast<float>(c.x()), static_cast<float>(c.y()));
        m.scale(static_cast<float>(gestureScale_));
        m.translate(static_cast<float>(-c.x()), static_cast<float>(-c.y()));
    }
    root->setMatrix(m);

    if (!session_) return root;

    // Budgeted upload: hand the pool a deadline rather than letting it push
    // everything that is ready. A burst of finished tiles must not blow the
    // frame; the remainder lands next frame and nobody notices.
    QElapsedTimer clock;
    clock.start();
    textures_->uploadReady(kUploadBudgetNs, clock);

    for (int page : visiblePages()) {
        auto* pageNode = ensurePageNode(root, page);
        const QRectF rect = pageRectInView(page);

        // Proxy LOD first and always. It is cheap, permanently resident, and
        // it is the reason a fast fling never shows a blank page - sharp
        // tiles then cross-fade on top as they arrive.
        if (auto* proxy = textures_->proxyFor(page))
            updateProxyNode(pageNode, proxy, rect);

        const TileGrid grid = gridForPage(page);
        for (const auto& spec : grid.tilesForViewport(viewportInPageSpace(page))) {
            core::TileKey key{};
            key.pageId   = static_cast<core::PageId>(page);
            key.tileX    = spec.tx;
            key.tileY    = spec.ty;
            key.scaleQ   = core::quantiseScale(static_cast<float>(effectiveScale()));
            key.rotation = pageRotationQuarter(page);
            key.scheme   = theme_;

            if (QSGTexture* tex = textures_->lookup(key))
                updateTileNode(pageNode, key, tex, spec, rect);
        }

        applyPageThemeShader(pageNode, theme_);
        updateOverlayNode(pageNode, page);
    }

    pruneOffscreenPageNodes(root);
    if (textures_->hasPending()) update();   // keep draining the upload queue
    return root;
}

// ---------------------------------------------------------------------------
// Gestures: the 60 fps contract
// ---------------------------------------------------------------------------

void FolioPageView::beginGesture() {
    inGesture_ = true;
    settleTimer_->stop();
}

void FolioPageView::endGesture() {
    inGesture_ = false;
    settleTimer_->start();   // 120 ms of stability, then re-raster
}

void FolioPageView::onScaleSettled() {
    // Fold the GPU-only gesture scale into the real zoom, reset the transform,
    // and only now ask the engine for sharp tiles at the new scale.
    if (qFuzzyCompare(gestureScale_, 1.0)) { requestVisibleTiles(); return; }

    zoom_ *= gestureScale_;
    gestureScale_ = 1.0;
    emit zoomChanged();

    requestVisibleTiles();
    releaseOffscreenTiles();
    update();
}

void FolioPageView::wheelEvent(QWheelEvent* e) {
    if (e->modifiers() & Qt::ControlModifier) {
        beginGesture();
        const qreal step = e->angleDelta().y() > 0 ? 1.1 : 1.0 / 1.1;
        gestureScale_ = qBound(0.4 / zoom_, gestureScale_ * step, 3.0 / zoom_);
        update();          // transform-only repaint: no raster, no upload
        endGesture();
        e->accept();
        return;
    }
    scrollY_ -= e->angleDelta().y();
    clampScroll();
    // Velocity-predicted prefetch: ask for tiles where the user is GOING,
    // not where they are (plan §8.3).
    session_->scheduler().setScrollVelocity(-e->angleDelta().y());
    requestVisibleTiles();
    update();
    e->accept();
}

void FolioPageView::touchEvent(QTouchEvent* e) {
    const auto& pts = e->points();
    if (pts.size() == 2) {
        const qreal now = QLineF(pts[0].position(), pts[1].position()).length();
        const qreal start = QLineF(pts[0].pressPosition(), pts[1].pressPosition()).length();
        if (start > 1.0) {
            if (!inGesture_) beginGesture();
            gestureScale_ = qBound(0.4 / zoom_, now / start, 3.0 / zoom_);
            update();
        }
        if (e->touchPointStates() & QEventPoint::Released) endGesture();
        e->accept();
        return;
    }
    QQuickItem::touchEvent(e);
}

void FolioPageView::fitWidth() {
    if (!session_) return;
    const auto info = session_->pageInfo(currentPage_);
    if (info.widthPt <= 0.f) return;
    const qreal avail = width() - 64.0;        // matches the prototype's margin
    setZoom(avail / static_cast<qreal>(info.widthPt));
}

void FolioPageView::setZoom(qreal z) {
    const qreal clamped = qBound(0.4, z, 3.0);
    if (qFuzzyCompare(clamped, zoom_)) return;
    zoom_ = clamped;
    emit zoomChanged();
    requestVisibleTiles();
    update();
}

void FolioPageView::setPageTheme(int t) {
    const auto next = static_cast<ColorScheme>(t);
    if (next == theme_) return;
    theme_ = next;
    emit pageThemeChanged();
    // Theme is a shader uniform, NOT a re-render (plan §9.1). This is what
    // lets sepia/dark cross-fade over 400 ms exactly like the prototype's
    // `transition: background .4s`, which a re-raster approach cannot do.
    update();
}

}  // namespace folio::ui
