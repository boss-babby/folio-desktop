// folio-ui/viewport/FolioPageView.h  --  work package F1
//
// The document viewport. A custom QQuickItem that builds QSGNodes directly.
//
// Deliberately NOT QQuickPaintedItem: that routes through a software raster
// surface plus an extra texture upload every repaint, which forfeits the
// frame budget before we have drawn anything (plan §14.2 rule 8).
//
// Node tree per visible page:
//
//   QSGTransformNode              scroll + live gesture scale
//    └─ QSGOpacityNode            page cross-fade on layout change
//        ├─ PageShadowNode        drop shadow, one 9-slice draw
//        ├─ QSGSimpleTextureNode  proxy LOD - always resident, never blank
//        ├─ TileNode[]            sharp tiles, cross-faded in over 120 ms
//        ├─ PageThemeNode         sepia/dark shader (plan §9.1)
//        └─ OverlayNode           vector annotations, GPU-drawn

#pragma once

#include <QQuickItem>
#include <QSGTexture>
#include <memory>

#include "folio/core/render/TileGrid.h"
#include "folio/core/render/TileKey.h"

namespace folio::core {
class DocumentSession;
class TileScheduler;
}

namespace folio::ui {

class TexturePool;

class FolioPageView : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(int    currentPage READ currentPage WRITE setCurrentPage NOTIFY currentPageChanged)
    Q_PROPERTY(qreal  zoom        READ zoom        WRITE setZoom        NOTIFY zoomChanged)
    Q_PROPERTY(int    layout      READ layout      WRITE setLayout      NOTIFY layoutChanged)
    Q_PROPERTY(int    pageTheme   READ pageTheme   WRITE setPageTheme   NOTIFY pageThemeChanged)

public:
    /// Mirrors the prototype's Scroll / Single / Two-up segmented control.
    enum Layout { Continuous = 0, SinglePage = 1, TwoUp = 2 };
    Q_ENUM(Layout)

    explicit FolioPageView(QQuickItem* parent = nullptr);
    ~FolioPageView() override;

    void setSession(core::DocumentSession* session);

    [[nodiscard]] int   currentPage() const noexcept { return currentPage_; }
    [[nodiscard]] qreal zoom()        const noexcept { return zoom_; }
    [[nodiscard]] int   layout()      const noexcept { return static_cast<int>(layout_); }
    [[nodiscard]] int   pageTheme()   const noexcept { return static_cast<int>(theme_); }

    void setCurrentPage(int page);
    void setZoom(qreal z);
    void setLayout(int l);
    void setPageTheme(int t);

    Q_INVOKABLE void fitWidth();
    Q_INVOKABLE void scrollToPage(int page, bool animated = true);

signals:
    void currentPageChanged();
    void zoomChanged();
    void layoutChanged();
    void pageThemeChanged();
    void tileStatsChanged(int resident, int pending);

protected:
    QSGNode* updatePaintNode(QSGNode* old, UpdatePaintNodeData*) override;
    void geometryChange(const QRectF& newGeom, const QRectF& oldGeom) override;
    void wheelEvent(QWheelEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void touchEvent(QTouchEvent*) override;

private:
    // ---- the rule that protects the frame budget --------------------------
    //
    // During an active gesture we NEVER re-rasterise. The existing textures
    // are scaled on the GPU and the settled scale is scheduled only after the
    // gesture ends or 120 ms of scale stability (plan §8.3). Re-rastering
    // mid-pinch is the single most common way to lose 60 fps in a PDF viewer.
    void beginGesture();
    void endGesture();
    void onScaleSettled();

    void requestVisibleTiles();
    void releaseOffscreenTiles();

    [[nodiscard]] core::TileGrid gridForPage(int page) const;
    [[nodiscard]] QRectF         pageRectInView(int page) const;
    [[nodiscard]] core::DeviceRect viewportInPageSpace(int page) const;

    core::DocumentSession* session_ = nullptr;
    std::unique_ptr<TexturePool> textures_;

    int    currentPage_ = 0;
    qreal  zoom_        = 1.25;   // prototype default: 125%
    qreal  gestureScale_ = 1.0;   // GPU-only scale while pinching
    qreal  scrollY_     = 0.0;
    Layout layout_      = Continuous;
    core::ColorScheme theme_ = core::ColorScheme::Light;

    bool   inGesture_   = false;
    QTimer* settleTimer_ = nullptr;   // 120 ms

    // Upload budget: at most 2 ms of texture upload per frame, remainder
    // deferred. One 4K tile must never cost a frame (plan §7.3).
    static constexpr qint64 kUploadBudgetNs = 2'000'000;
};

}  // namespace folio::ui
