#include "screens/FolioApp.h"
#include "screens/ReaderScreen.h"
#if FOLIO_HAVE_PDFIUM_SOURCE
#include "pdfium/PdfiumPageSource.h"
#endif
#include "../Harness.h"

#include <filesystem>
#include <fstream>
#include <chrono>
#include <cmath>
#include <thread>

#ifndef FOLIO_TEST_DATA_DIR
#define FOLIO_TEST_DATA_DIR "tests/data"
#endif

using namespace folio::ui;
using namespace folio::test;

FOLIO_TEST_MAIN("navigation and persistent recents") {
    section("destroyed hover/capture targets never receive a later PDF click");
    {
        Widget root;
        root.setFrame(0.f, 0.f, 800.f, 600.f);
        Widget* rows = root.emplace<Widget>();
        rows->setFrame(0.f, 0.f, 800.f, 600.f);
        Button* card = rows->emplace<Button>("recent PDF", "", ButtonStyle::Card);
        card->setFrame(20.f, 60.f, 200.f, 180.f);
        EventRouter events;
        PointerEvent mouse{};
        mouse.pos = mouse.windowPos = {40.f, 80.f};
        mouse.phase = PointerPhase::Move;
        events.dispatchPointer(root, mouse);
        check(events.hovered() == card, "recent card owns mouse hover");
        mouse.phase = PointerPhase::Down;
        mouse.button = MouseButton::Left;
        events.dispatchPointer(root, mouse);
        check(events.focused() == card, "recent card owns pointer capture/focus");
        // A recents refresh after opening a PDF destroys its card BEFORE
        // the next native pointer event arrives. This used to leave a raw
        // pointer in hover/capture and AV on the first click anywhere.
        rows->removeAllChildren();
        check(!events.hovered() && !events.focused(),
              "row destruction invalidates hover and keyboard focus");
        mouse.phase = PointerPhase::Up;
        events.dispatchPointer(root, mouse);
        auto* next = rows->emplace<Button>("reader control", "", ButtonStyle::Icon);
        next->setFrame(20.f, 60.f, 200.f, 180.f);
        mouse.phase = PointerPhase::Down;
        events.dispatchPointer(root, mouse);
        mouse.phase = PointerPhase::Up;
        events.dispatchPointer(root, mouse);
        check(events.hovered() == next, "subsequent reader click hits only the live control");
        next->onClick = [&] { rows->removeAllChildren(); };
        mouse.phase = PointerPhase::Down;
        events.dispatchPointer(root, mouse);
        mouse.phase = PointerPhase::Up;
        events.dispatchPointer(root, mouse);
        check(!events.hovered() && !events.focused(),
              "a button may destroy itself on mouse release without leaving a dangling capture");
    }

    section("integrated desktop title bar moves only from empty chrome");
    {
        FolioApp dragApp(1024.f, 720.f);
        int moves = 0, toggles = 0;
        FolioApp::HostServices titleHost;
        titleHost.beginDragMove = [&] { ++moves; };
        titleHost.toggleMaximise = [&] { ++toggles; };
        titleHost.isMaximised = [&] { return toggles % 2 != 0; };
        dragApp.setHostServices(std::move(titleHost));
        PointerEvent title{};
        title.phase = PointerPhase::Down;
        title.button = MouseButton::Left;
        title.pos = title.windowPos = {200.f, 20.f};
        title.timestamp = 1.0;
        dragApp.pointer(title);
        check(moves == 1, "empty library title bar starts a native window drag");
        title.clickCount = 2;
        title.timestamp = 1.1;
        dragApp.pointer(title);
        check(toggles == 1, "double-click on empty title bar toggles maximise");
        title.clickCount = 1;
        title.windowPos = title.pos = {1024.f - 21.f, 20.f};
        dragApp.pointer(title);
        check(moves == 1, "close button never initiates a window drag");
    }

    section("expanded Pages and Outline change actual left-panel content");
    FolioApp app(1440.f, 900.f);
    app.showScreen(Screen::Reader, 1.0);
    ThumbnailList* thumbs = nullptr;
    OutlineList* outline = nullptr;
    SegmentedControl* tabs = nullptr;
    app.root().forEach([&](Widget& w) {
        if (auto* p = dynamic_cast<ThumbnailList*>(&w)) thumbs = p;
        if (auto* o = dynamic_cast<OutlineList*>(&w);
            o && o->debugName == "OutlineList") outline = o;
    });
    check(thumbs && outline, "reader has both navigation views");
    if (thumbs && outline) {
        check(thumbs->visible && !outline->visible, "Pages shows thumbnails on open");
        auto* panel = thumbs->parent();
        for (const auto& child : panel->children()) {
            if (auto* s = dynamic_cast<SegmentedControl*>(child.get())) {
                tabs = s;
                break;
            }
        }
        check(tabs != nullptr, "left panel has a tab switcher");
        if (tabs) {
            const Point target = tabs->toWindow({tabs->frame.w * 1.5f / 4.f,
                                                  tabs->frame.h * 0.5f});
            PointerEvent click{};
            click.button = MouseButton::Left;
            click.pos = click.windowPos = target;
            click.timestamp = 2.0;
            click.phase = PointerPhase::Down;
            app.pointer(click);
            click.phase = PointerPhase::Up;
            app.pointer(click);
            check(app.state().leftTab == 1, "clicking Outline updates navigation state");
            check(!thumbs->visible && outline->visible,
                  "Outline tab swaps the expanded content without resize");
            tabs->setSelected(0, 3.0);
            check(thumbs->visible && !outline->visible,
                  "Pages tab restores thumbnails without resize");
        }
    }
    check(app.state().rightPanelCollapsed, "inspector is collapsed by default");
    SidePanel* inspector = nullptr;
    int closeButtons = 0;
    app.root().forEach([&](Widget& w) {
        if (auto* p = dynamic_cast<SidePanel*>(&w);
            p && p->debugName == "SidePanel(R)") inspector = p;
    });
    check(inspector && inspector->collapsed() &&
              inspector->currentWidth(0.0) <= tokens::size::panelRailCollapsed + 1.f,
          "the actual command panel starts collapsed, without an opening animation");
    FolioApp::HostServices host;
    host.close = [&] { ++closeButtons; };
    app.setHostServices(std::move(host));
    app.root().forEach([&](Widget& w) {
        if (auto* b = dynamic_cast<Button*>(&w);
            b && b->debugName == "win.close" && b->onClick) b->onClick();
    });
    check(closeButtons == 2, "both welcome and reader close buttons request host exit");

    section("right-panel view rotation is distinct from edit history");
    ReaderScreen* rotateReader = nullptr;
    Button* viewLeft = nullptr;
    Button* viewRight = nullptr;
    Button* undoIcon = nullptr;
    app.root().forEach([&](Widget& w) {
        if (auto* r = dynamic_cast<ReaderScreen*>(&w)) rotateReader = r;
        if (auto* b = dynamic_cast<Button*>(&w)) {
            if (b->tooltip.find("Rotate view left") == 0) viewLeft = b;
            if (b->tooltip.find("Rotate view right") == 0) viewRight = b;
            if (b->tooltip.find("Undo") == 0 && b->icon == "undo") undoIcon = b;
        }
    });
    check(rotateReader && viewLeft && viewRight && undoIcon &&
              viewLeft->icon == "view-rotate-ccw" &&
              viewRight->icon == "view-rotate-cw" &&
              viewLeft->icon != undoIcon->icon,
          "view rotation uses page-orientation glyphs unlike Undo/Redo");
    if (rotateReader && viewLeft && viewRight) {
        const int before = rotateReader->stage()->rotationQuarterTurns();
        viewRight->onClick();
        check(rotateReader->stage()->rotationQuarterTurns() == (before + 1) % 4,
              "right-panel clockwise control rotates view by one quarter turn");
        viewLeft->onClick();
        check(rotateReader->stage()->rotationQuarterTurns() == before,
              "opposite view turn restores orientation without page edit");
    }

    section("compact Home keeps a functional Settings entry");
    FolioApp settingsPhone(390.f, 800.f);
    Button* settingsGear = nullptr;
    Button* darkSetting = nullptr;
    Popover* settingsMenu = nullptr;
    settingsPhone.root().forEach([&](Widget& w) {
        if (auto* b = dynamic_cast<Button*>(&w)) {
            if (b->tooltip == "Library settings") settingsGear = b;
            if (b->label == "Dark appearance") darkSetting = b;
        }
        if (auto* p = dynamic_cast<Popover*>(&w)) {
            if (p->frame.w == 310.f) settingsMenu = p;
        }
    });
    check(settingsGear && settingsGear->visible && darkSetting && settingsMenu,
          "phone Home exposes the same Settings surface as desktop");
    if (settingsGear && darkSetting && settingsMenu) {
        settingsGear->onClick();
        check(settingsMenu->isOpen(), "Home settings opens a real popover");
        darkSetting->onClick();
        check(!settingsMenu->isOpen() &&
                  settingsPhone.state().theme.scheme() == ColorScheme::Dark,
              "Home settings changes appearance and dismisses the popover");
    }

    section("stationary hover does not demand another full GPU frame");
    Widget hoverRoot;
    hoverRoot.setFrame(0.f, 0.f, 200.f, 100.f);
    auto* hoverButton = hoverRoot.emplace<Button>("Hover", "", ButtonStyle::Ghost);
    hoverButton->setFrame(0.f, 0.f, 100.f, 50.f);
    EventRouter hoverRouter;
    PointerEvent move{};
    move.phase = PointerPhase::Move;
    move.pos = move.windowPos = {30.f, 20.f};
    check(hoverRouter.dispatchPointer(hoverRoot, move), "entry invalidates hover");
    check(!hoverRouter.dispatchPointer(hoverRoot, move),
          "moves inside unchanged button do not repaint the full frame");
    ThumbnailList hoverThumbs(app.state());
    hoverThumbs.setFrame(0.f, 0.f, 300.f, 300.f);
    hoverThumbs.setPageCount(2);
    move.pos = {52.f, 30.f};
    check(hoverThumbs.onPointer(move), "entering the first thumbnail repaints");
    check(!hoverThumbs.onPointer(move), "stationary thumbnail hover does not repaint");
    move.pos = {200.f, 30.f};
    check(hoverThumbs.onPointer(move), "crossing to the second column repaints");
    check(!hoverThumbs.onPointer(move), "stationary second-column hover does not repaint");
    move.phase = PointerPhase::Leave;
    check(hoverThumbs.onPointer(move) && !hoverThumbs.onPointer(move),
          "thumbnail hover repaints only on the first leave");
    OutlineList hoverOutline(app.state());
    hoverOutline.setFrame(0.f, 0.f, 300.f, 300.f);
    hoverOutline.setEntries({{"First", 0, 0}, {"Second", 1, 0}});
    move.phase = PointerPhase::Move;
    move.pos = {20.f, 43.f};
    check(hoverOutline.onPointer(move), "entering an outline row repaints");
    check(!hoverOutline.onPointer(move), "stationary outline hover does not repaint");
    move.pos.y += 34.f;
    check(hoverOutline.onPointer(move), "crossing outline rows repaints");
    check(!hoverOutline.onPointer(move), "stationary second outline row does not repaint");
    move.phase = PointerPhase::Leave;
    check(hoverOutline.onPointer(move) && !hoverOutline.onPointer(move),
          "outline hover repaints only on the first leave");

    section("recent cards are durable filesystem PDFs, never demo entries");
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "folio-history-test";
    fs::create_directories(root);
    const fs::path pdf = root / "a real PDF.pdf";
    { std::ofstream output(pdf); output << "%PDF-1.7\n"; }
    const fs::path stateFile = root / "recents.txt";
    AppState state;
    state.loadRecents(stateFile.string());
    check(state.recents.empty(), "fresh library has no synthetic demo documents");
    state.rememberDocument(pdf.string(), 17, 9);
    state.autoHideFocusControls = false;
    state.selectionMagnifier = true;
    state.saveRecents();
    AppState reloaded;
    reloaded.loadRecents(stateFile.string());
    check(!reloaded.autoHideFocusControls,
          "Focus auto-hide preference persists alongside V1 recents");
    check(reloaded.selectionMagnifier,
          "reader text magnifier preference persists beside recent PDFs");
    FolioApp restoredPhone(390.f, 800.f);
    restoredPhone.loadRecents(stateFile.string());
    Button* restoredAutoHide = nullptr;
    Button* restoredMagnifier = nullptr;
    ReaderScreen* restoredReader = nullptr;
    restoredPhone.root().forEach([&](Widget& w) {
        if (auto* r = dynamic_cast<ReaderScreen*>(&w)) restoredReader = r;
        if (auto* b = dynamic_cast<Button*>(&w);
            b && b->tooltip == "Hide focus controls after 3 seconds")
            restoredAutoHide = b;
        if (auto* b = dynamic_cast<Button*>(&w);
            b && b->tooltip == "Magnify text selection and annotation highlighter")
            restoredMagnifier = b;
    });
    check(restoredAutoHide && !restoredAutoHide->toggled,
          "loaded preference updates the inspector toggle as well as the model");
    check(restoredMagnifier && restoredMagnifier->toggled &&
              restoredReader && restoredReader->stage()->selectionMagnifierEnabled(),
          "saved magnifier choice restores the Reader toggle and the live stage");
    check(reloaded.recents.size() == 1 && reloaded.recents[0].path == pdf.string(),
          "opened PDF path survives restart");
    check(reloaded.recents.front().progress > 0.4f,
          "real last-page progress survives restart");
#if !defined(__ANDROID__)
    check(reloaded.recents.front().meta.find("Original file") != std::string::npos &&
          reloaded.recents.front().meta.find("Offline copy") == std::string::npos,
          "desktop never claims an automatic offline PDF copy");
#endif
    Button* manageStorage = nullptr;
    Button* documentStorage = nullptr;
    Popover* storageChooser = nullptr;
#if defined(__ANDROID__)
    const std::string manageLabel = "Manage each PDF's storage...";
    const std::string documentLabel = "a real PDF.pdf · Keep copy";
#else
    const std::string manageLabel = "Manage library files...";
    const std::string documentLabel = "a real PDF.pdf · Original file";
#endif
    restoredPhone.root().forEach([&](Widget& w) {
        if (auto* b = dynamic_cast<Button*>(&w)) {
            if (b->label == manageLabel) manageStorage = b;
            if (b->label.find(documentLabel) == 0) documentStorage = b;
        }
        if (auto* p = dynamic_cast<Popover*>(&w); p && p->frame.w == 312.f &&
            p->frame.h == 360.f) storageChooser = p;
    });
    check(manageStorage && storageChooser,
          "Settings exposes a per-document storage picker on compact Home");
    if (manageStorage && storageChooser) {
        manageStorage->onClick();
        restoredPhone.root().forEach([&](Widget& w) {
            if (auto* b = dynamic_cast<Button*>(&w);
                b && b->label.find(documentLabel) == 0)
                documentStorage = b;
        });
        check(storageChooser->isOpen() && documentStorage,
              "Settings lists the named PDF and its current storage mode");
    }

    fs::remove(pdf);
    const fs::path linked = pdf.string() + ".folio-link";
    { std::ofstream marker(linked); marker << "source-only\n"; }
    reloaded.loadRecents(stateFile.string());
    check(reloaded.recents.size() == 1 && reloaded.recents[0].linked,
          "source-only PDF remains in library after its copy is removed");
    fs::remove(linked);
    reloaded.loadRecents(stateFile.string());
    check(reloaded.recents.empty(), "deleted non-linked PDF is not shown as a broken card");
    section("imported annotation sidecar keeps tombstones and stable identity");
    AnnotationStore sourceMarks;
    Annotation importedMark;
    importedMark.kind = AnnotKind::Highlight;
    importedMark.page = 0;
    importedMark.box = {.1f, .2f, .3f, .04f};
    importedMark.sourceIndex = 7;
    importedMark.sourceSubtype = 9;
    importedMark.sourceFingerprint = 0xA104BEEFULL;
    importedMark.comment = "original";
    const auto importedId = sourceMarks.add(importedMark);
    const auto sourceBefore = *sourceMarks.get(importedId);
    sourceMarks.removeById(importedId);
    const auto sidecar = (root / "source-edits.txt").string();
    check(sourceMarks.saveSidecar(sidecar, "stable-path"), "imported deletion persists");
    AnnotationStore restoredSource;
    check(restoredSource.loadSidecar(sidecar, "stable-path") &&
          restoredSource.get(importedId) && restoredSource.get(importedId)->deleted &&
          restoredSource.get(importedId)->sourceFingerprint == importedMark.sourceFingerprint &&
          restoredSource.forPage(0).empty(),
          "sidecar restores imported tombstone without painting a duplicate mark");
    restoredSource.addExisting(sourceBefore);
    check(restoredSource.forPage(0).size() == 1,
          "undo restores the same imported id rather than appending another object");
    fs::remove(sidecar);

    { std::ofstream legacy(stateFile); legacy << "FOLIO_RECENTS_V1\n"; }
    reloaded.loadRecents(stateFile.string());
    check(reloaded.autoHideFocusControls,
          "legacy recents without a preference keep auto-hide enabled");
#if defined(__ANDROID__)
    check(reloaded.selectionMagnifier, "legacy mobile state defaults magnifier on");
#else
    check(!reloaded.selectionMagnifier, "legacy desktop state defaults magnifier off");
#endif
    fs::remove_all(root);

    section("phone keeps title theme, removes command Display; Focus remains in every mode's right panel");
    FolioApp phone(390.f, 800.f);
    phone.showScreen(Screen::Reader, 1.0);
    Button* focus = nullptr;
    Button* focusPanel = nullptr;
    Button* focusAutoHide = nullptr;
    Button* magnifier = nullptr;
    ReaderScreen* phoneReader = nullptr;
    Button* display = nullptr;
    Button* openPdf = nullptr;
    Button* themeShortcut = nullptr;
    Button* exitFocus = nullptr;
    Button* annotate = nullptr;
    Button* edit = nullptr;
    Button* read = nullptr;
    Button* inspectFab = nullptr;
    phone.root().forEach([&](Widget& w) {
        if (auto* r = dynamic_cast<ReaderScreen*>(&w)) phoneReader = r;
        if (auto* b = dynamic_cast<Button*>(&w)) {
            if (b->tooltip == "Focus mode  (F)") focus = b;
            if (b->tooltip == "Focus mode (right panel)") focusPanel = b;
            if (b->tooltip == "Hide focus controls after 3 seconds") focusAutoHide = b;
            if (b->tooltip == "Magnify text selection and annotation highlighter") magnifier = b;
            if (b->tooltip == "Display options") display = b;
            if (b->tooltip == "Open  (Ctrl+O)") openPdf = b;
            if (b->tooltip == "Day / night") themeShortcut = b;
            if (b->tooltip == "Leave focus mode  (Esc)") exitFocus = b;
            if (b->tooltip == "Tool options") inspectFab = b;
            if (b->trailing == "Ctrl 1") read = b;
            if (b->trailing == "Ctrl 2") annotate = b;
            if (b->trailing == "Ctrl 3") edit = b;
        }
    });
    check(focus && focus->visible && display && !display->visible &&
              openPdf && openPdf->visible,
          "Read toolbar keeps Focus and Open but hides command-bar Display");
    check(themeShortcut && themeShortcut->visible && themeShortcut->frame.w == 34.f,
          "mobile title-bar sun/moon shortcut is retained");
    Button* desktopDisplay = nullptr;
    app.root().forEach([&](Widget& w) {
        if (auto* b = dynamic_cast<Button*>(&w);
            b && b->tooltip == "Display options") desktopDisplay = b;
    });
    check(desktopDisplay && desktopDisplay->visible,
          "Display options remain in the wide desktop command bar");
    Button* zoomPct = nullptr;
    Button* fitPreset = nullptr;
    Button* fixedPreset = nullptr;
    phone.root().forEach([&](Widget& w) {
        if (auto* b = dynamic_cast<Button*>(&w)) {
            if (b->tooltip == "Zoom presets, fit width or fit page") zoomPct = b;
            if (b->label == "Fit page" && dynamic_cast<Popover*>(b->parent()))
                fitPreset = b;
            if (b->label == "150%" && dynamic_cast<Popover*>(b->parent()))
                fixedPreset = b;
        }
    });
    auto* zoomMenu = fitPreset ? dynamic_cast<Popover*>(fitPreset->parent()) : nullptr;
    check(zoomPct && zoomPct->visible && zoomMenu && fitPreset && fixedPreset,
          "mobile percentage has a native popover with fit and fixed zoom presets");
    if (zoomPct && zoomMenu && fitPreset && fixedPreset) {
        zoomPct->onClick();
        check(zoomMenu->isOpen() && zoomMenu->preferAbove,
              "percentage opens the normal upward anchor-reveal popover");
        fitPreset->onClick();
        ReaderScreen* screen = nullptr;
        phone.root().forEach([&](Widget& w) {
            if (auto* r = dynamic_cast<ReaderScreen*>(&w)) screen = r;
        });
        check(screen && screen->stage()->zoomMode() == ZoomMode::FitPage &&
                  !zoomMenu->isOpen(),
              "Fit page chooses viewport-height fit and closes the popup");
        zoomPct->onClick();
        fixedPreset->onClick();
        check(screen && std::fabs(screen->stage()->zoom() - 1.5f) < .001f &&
                  screen->stage()->zoomMode() == ZoomMode::Free,
              "150 percent applies a fixed zoom without adding pill buttons");
    }
    if (themeShortcut) {
        const auto previous = phone.state().theme.scheme();
        themeShortcut->onClick();
        check(phone.state().theme.scheme() != previous &&
                  themeShortcut->icon == "moon",
              "top shortcut still switches theme and updates its glyph");
    }
    check(focusPanel && focusAutoHide && focusAutoHide->toggled &&
              inspectFab,
          "right panel has a handle, Focus and an auto-hide option");
    if (inspectFab) {
        inspectFab->onClick();
        check(!phone.state().rightPanelCollapsed,
              "phone inspector handle opens the Focus entry panel");
    }
    Button* rightUndo = nullptr;
    Button* rightRedo = nullptr;
    phone.root().forEach([&](Widget& w) {
        if (auto* b = dynamic_cast<Button*>(&w);
            b && focusPanel && b->parent() == focusPanel->parent()) {
            if (b->tooltip.rfind("Undo", 0) == 0) rightUndo = b;
            if (b->tooltip.rfind("Redo", 0) == 0) rightRedo = b;
        }
    });
    // A real phone advances the sheet animation before it can be tapped.
    // Lay out its finished state rather than asserting the still-collapsed
    // first frame (whose Focus button is legitimately offscreen).
    if (focusPanel) {
        if (auto* panel = dynamic_cast<SidePanel*>(focusPanel->parent())) {
            panel->setCollapsedImmediately(false);
            phone.root().layoutTree();
        }
    }
    check(rightUndo && rightRedo && rightUndo->visible && rightRedo->visible &&
              !rightUndo->enabled && !rightRedo->enabled &&
              rightUndo->frame.right() < focusPanel->frame.x,
          "right-panel Undo and Redo fit beside Focus, disabled until edits exist");
    if (focusPanel && focus && exitFocus && annotate && edit && read) {
        annotate->onClick();
        check(phone.state().mode == Mode::Annotate && !focus->visible &&
                  openPdf && !openPdf->visible,
              "Annotate frees the compact Focus and Open slots for tools");
        Button *colour = nullptr, *size = nullptr, *opacity = nullptr;
        Button *find = nullptr, *overflow = nullptr, *remove = nullptr;
        phone.root().forEach([&](Widget& w) {
            if (auto* b = dynamic_cast<Button*>(&w)) {
                if (b->tooltip == "Colour") colour = b;
                if (b->tooltip == "Thickness") size = b;
                if (b->tooltip == "Opacity") opacity = b;
                if (b->tooltip == "Find in document  (Ctrl+F)") find = b;
                if (b->tooltip == "More tools") overflow = b;
                if (b->tooltip == "Delete selected annotation") remove = b;
            }
        });
        check(colour && size && opacity && find && overflow && remove &&
                  colour->visible && size->visible && opacity->visible &&
                  !find->visible && !remove->visible,
              "Annotate keeps three stable property slots, hides search, and reserves Delete for selection");
        phone.resize(320.f, 800.f);
        bool fits = colour && size && opacity && overflow;
        if (fits) {
            const Point first = opacity->toWindow({0.f, 0.f});
            fits = first.x >= 0.f && first.x + opacity->frame.w <= 320.f;
            // Every visible tool and the More button must end BEFORE the
            // first options slot; no invisible buttons can steal taps.
            phone.root().forEach([&](Widget& w) {
                if (auto* b = dynamic_cast<Button*>(&w);
                    b && b->visible && b->parent() &&
                    (b->tooltip == "Select" || b->tooltip == "Highlight" ||
                     b->tooltip == "Pen" || b->tooltip == "Note" ||
                     b->tooltip == "Eraser" || b->tooltip == "More tools")) {
                    const Point pt = b->toWindow({0.f, 0.f});
                    fits = fits && pt.x + b->frame.w <= first.x - 2.f;
                }
            });
        }
        check(fits && overflow && overflow->visible,
              "320dp phone uses overflow rather than overlapping property controls");
        phone.resize(390.f, 800.f);
        Button* marker = nullptr;
        phone.root().forEach([&](Widget& w) {
            if (auto* b = dynamic_cast<Button*>(&w);
                b && b->tooltip == "Highlight" && b->visible &&
                !dynamic_cast<Popover*>(b->parent())) marker = b;
        });
        if (marker && size) {
            marker->onClick();
            check(phone.state().tool == Tool::Highlight && size->visible &&
                      size->acceptsPointer && size->opacity > .9f,
                  "Highlight enables the top-bar Thickness control");
            size->onClick();
            std::vector<Button*> weights;
            phone.root().forEach([&](Widget& w) {
                if (auto* b = dynamic_cast<Button*>(&w);
                    b && b->isWidthSample &&
                    dynamic_cast<Popover*>(b->parent()) && b->parent()->visible)
                    weights.push_back(b);
            });
            check(weights.size() == 6,
                  "Highlight's top-bar Thickness opens six live presets");
            if (weights.size() == 6) {
                weights.back()->onClick();
                check(size->widthSample == 11.f,
                      "choosing a width updates the top-bar sample and marker tool");
            }
        } else {
            check(false, "Highlight and Thickness controls exist on the command bar");
        }
        Button* pen = nullptr;
        Button* arrowShape = nullptr;
        ReaderScreen* phoneReader = nullptr;
        phone.root().forEach([&](Widget& w) {
            if (auto* r = dynamic_cast<ReaderScreen*>(&w)) phoneReader = r;
            if (auto* b = dynamic_cast<Button*>(&w)) {
                if (b->tooltip == "Draw" && !pen) pen = b;
                if (b->tooltip == "Arrow") arrowShape = b;
            }
        });
        if (pen && arrowShape && phoneReader) {
            pen->onClick();
            check(arrowShape->visible,
                  "Pen icon opens five-choice compact drawing menu");
            arrowShape->onClick();
            check(phoneReader->stage()->editTool() == PageStage::EditTool::Arrow,
                  "Arrow choice arms a genuine arrow annotation tool");
        }
        focusPanel->onClick();
        check(phone.state().focusMode && exitFocus->visible &&
                  exitFocus->label.empty() && exitFocus->frame.w == 44.f &&
                  phoneReader->stage()->focusScrollbarsVisible(),
              "Focus begins with exit, bottom controls and PDF scrollbars visible");
        exitFocus->onClick();
        check(phoneReader->stage()->focusScrollbarsVisible(),
              "leaving Focus always restores page scrollbar touch targets");
        edit->onClick();
        check(phone.state().mode == Mode::Edit && !focus->visible &&
                  openPdf && !openPdf->visible,
              "Edit frees the compact Focus and Open slots for tools");
        focusPanel->onClick();
        check(phone.state().focusMode && exitFocus->visible,
              "right-panel Focus also opens in Edit mode");
        exitFocus->onClick();
        read->onClick();
        check(focus->visible && openPdf && openPdf->visible,
              "Read restores compact toolbar Focus and Open");
    }
    if (magnifier) {
        magnifier->onClick();
        check(phone.state().selectionMagnifier && magnifier->toggled &&
                  phoneReader && phoneReader->stage()->selectionMagnifierEnabled(),
              "right-hand Display toggle enables the touch-only magnifier");
        magnifier->onClick();
        check(!phone.state().selectionMagnifier && !magnifier->toggled &&
                  phoneReader && !phoneReader->stage()->selectionMagnifierEnabled(),
              "reader toggle immediately disables and persists the magnifier");
    } else {
        check(false, "Reader inspector contains the text magnifier setting");
    }
    if (focusAutoHide) {
        focusAutoHide->onClick();
        check(!phone.state().autoHideFocusControls && !focusAutoHide->toggled,
              "inspector can keep controls on screen");
        focusAutoHide->onClick();
        check(phone.state().autoHideFocusControls && focusAutoHide->toggled,
              "inspector can restore idle auto-hide");
    }

    section("landscape phone stays phone-sized, without clipped sheet content");
    check(LayoutMetrics::forViewport(850.f, 390.f).panelsOverlay &&
          LayoutMetrics::forViewport(1000.f, 720.f).bp == Breakpoint::Medium,
          "short rotated phone remains compact; full-height tablet remains medium");
    FolioApp landscape(850.f, 390.f);
    WelcomeScreen* landHome = nullptr;
    Label* landGreeting = nullptr;
    Button* landSettings = nullptr;
    landscape.root().forEach([&](Widget& w) {
        if (auto* h = dynamic_cast<WelcomeScreen*>(&w)) landHome = h;
        if (auto* l = dynamic_cast<Label*>(&w);
            l && l->text.rfind("Good ", 0) == 0) landGreeting = l;
        if (auto* b = dynamic_cast<Button*>(&w);
            b && b->tooltip == "Library settings") landSettings = b;
    });
    bool noLandscapeHero = landHome != nullptr;
    if (landHome) {
        for (const auto& child : landHome->children()) {
            if (dynamic_cast<CardView*>(child.get()) && child->visible)
                noLandscapeHero = false;
        }
    }
    check(noLandscapeHero && landGreeting && landSettings &&
          landSettings->visible && landGreeting->frame.right() <=
              landSettings->frame.x - 5.f,
          "landscape Home hides the oversized hero and keeps greeting/gear separate");
    landscape.showScreen(Screen::Reader, 2.0);
    SidePanel* landLeft = nullptr;
    ThumbnailList* landThumbs = nullptr;
    PageStage* landStage = nullptr;
    landscape.root().forEach([&](Widget& w) {
        if (auto* p = dynamic_cast<SidePanel*>(&w);
            p && p->debugName == "SidePanel(L)") landLeft = p;
        if (auto* t = dynamic_cast<ThumbnailList*>(&w)) landThumbs = t;
        if (auto* stage = dynamic_cast<PageStage*>(&w)) landStage = stage;
    });
    if (landLeft) {
        landLeft->setCollapsedImmediately(false);
        landscape.root().layoutTree();
    }
    check(landLeft && landThumbs && landStage &&
          landLeft->presentation() == SidePanel::Presentation::Sheet &&
          landStage->frame.w > 750.f &&
          landThumbs->frame.bottom() <= landLeft->frame.h - 20.f,
          "landscape reader uses a full-width page and scroll sheet above nav strip");
    FolioApp portraitSheet(390.f, 800.f);
    SidePanel* portraitLeft = nullptr;
    ThumbnailList* portraitThumbs = nullptr;
    portraitSheet.root().forEach([&](Widget& w) {
        if (auto* p = dynamic_cast<SidePanel*>(&w);
            p && p->debugName == "SidePanel(L)") portraitLeft = p;
        if (auto* t = dynamic_cast<ThumbnailList*>(&w)) portraitThumbs = t;
    });
    if (portraitLeft) {
        portraitLeft->setCollapsedImmediately(false);
        portraitSheet.root().layoutTree();
    }
    check(portraitLeft && portraitThumbs &&
          portraitThumbs->frame.bottom() <= portraitLeft->frame.h - 20.f,
          "portrait thumbnail list stops before the sheet footer and gesture bar");

#if FOLIO_HAVE_PDFIUM_SOURCE
    section("first click after opening a real PDF survives recents rebuild");
    {
        const fs::path actualPdf = fs::path(FOLIO_TEST_DATA_DIR) / "folio-test.pdf";
        const fs::path recents = fs::temp_directory_path() / "folio-first-click-recents.txt";
        fs::remove(recents);
        {
            FolioApp clicked(1200.f, 800.f);
            clicked.loadRecents(recents.string());
            clicked.state().rememberDocument(actualPdf.string(), 1, 1);
            WelcomeScreen* welcome = nullptr;
            ReaderScreen* reader = nullptr;
            clicked.root().forEach([&](Widget& w) {
                if (auto* s = dynamic_cast<WelcomeScreen*>(&w)) welcome = s;
                if (auto* s = dynamic_cast<ReaderScreen*>(&w)) reader = s;
            });
            check(welcome && reader, "both screens exist before PDF open");
            if (welcome && reader) {
                welcome->refreshRecents(1.0);
                RecentCard* card = nullptr;
                clicked.root().forEach([&](Widget& w) {
                    if (auto* c = dynamic_cast<RecentCard*>(&w)) card = c;
                });
                check(card != nullptr, "real PDF appears in recent cards");
                if (card) {
                    PointerEvent mouse{};
                    mouse.phase = PointerPhase::Move;
                    mouse.pos = mouse.windowPos = card->toWindow({40.f, 40.f});
                    mouse.timestamp = 1.0;
                    clicked.pointer(mouse); // hover a soon-to-be-destroyed card
                    check(reader->openFile(actualPdf.string(), 2.0),
                          "real PDF prepares for reader view");
                    clicked.showScreen(Screen::Reader, 2.0);
                    // This is what FolioApp::paint does on recentsDirty_ after
                    // opening the file, while the old card is still hovered.
                    welcome->refreshRecents(2.0);
                    mouse.pos = mouse.windowPos = {600.f, 480.f};
                    mouse.timestamp = 2.1;
                    mouse.phase = PointerPhase::Down;
                    mouse.button = MouseButton::Left;
                    clicked.pointer(mouse);
                    mouse.phase = PointerPhase::Up;
                    mouse.timestamp = 2.2;
                    clicked.pointer(mouse);
                    check(reader->stage()->source() != nullptr,
                          "PDF remains open after first reader click");
                }
            }
        }
        fs::remove(recents);
    }

    section("selected-text popup stays up while choosing a markup colour");
    app.openFileFromHost(std::string(FOLIO_TEST_DATA_DIR) + "/folio-test.pdf");
    for (int i = 0; i < 500 && app.loadingDocument(); ++i) {
        app.pollOpen();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ReaderScreen* reader = nullptr;
    app.root().forEach([&](Widget& w) {
        if (auto* r = dynamic_cast<ReaderScreen*>(&w)) reader = r;
    });
    check(reader != nullptr, "real PDF opens in reader");
    if (reader && reader->stage()->source()) {
        auto* src = reader->stage()->source();
        for (int i = 0; i < 300 && !src->glyphs(0); ++i) {
            src->tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        reader->stage()->selectAll();
        check(reader->stage()->hasSelection(), "text is selected on the real PDF");
        if (reader->stage()->hasSelection()) {
            reader->stage()->onSelectionCommitted({700.f, 400.f});
            Popover* selection = nullptr;
            Popover* colours = nullptr;
            Button* chip = nullptr;
            app.root().forEach([&](Widget& w) {
                auto* b = dynamic_cast<Button*>(&w);
                if (b && b->tooltip == "Markup colour") {
                    chip = b;
                    selection = dynamic_cast<Popover*>(b->parent());
                }
            });
            check(chip && selection && selection->isOpen(),
                  "selection actions appear next to text");
            if (chip && selection) {
                const Point p = chip->toWindow({chip->frame.w / 2, chip->frame.h / 2});
                PointerEvent pick{};
                pick.button = MouseButton::Left;
                pick.pos = pick.windowPos = p;
                pick.timestamp = 3.0;
                pick.phase = PointerPhase::Down;
                app.pointer(pick);
                pick.phase = PointerPhase::Up;
                app.pointer(pick);
                app.root().forEach([&](Widget& w) {
                    if (auto* pop = dynamic_cast<Popover*>(&w))
                        if (pop != selection && pop->isOpen()) colours = pop;
                });
                check(selection->isOpen() && colours && colours->isOpen(),
                      "colour picker and selection actions coexist");
                if (colours && !colours->children().empty()) {
                    auto* swatch = dynamic_cast<Button*>(colours->children().front().get());
                    if (swatch) {
                        const Point c = swatch->toWindow({swatch->frame.w / 2,
                                                         swatch->frame.h / 2});
                        pick.pos = pick.windowPos = c;
                        pick.timestamp = 4.0;
                        pick.phase = PointerPhase::Down;
                        app.pointer(pick);
                        pick.phase = PointerPhase::Up;
                        app.pointer(pick);
                        check(selection->isOpen(),
                              "choosing colour dismisses only colour picker");
                    }
                }
                reader->handleOutsideClick({0.f, 200.f}, 5.0);
                check(!selection->isOpen(), "outside click dismisses actions");
                reader->stage()->onSelectionCommitted({700.f, 400.f});
                KeyEvent esc{};
                esc.key = Key::Escape;
                esc.pressed = true;
                reader->onKey(esc);
                check(!selection->isOpen(), "Escape deselects text and dismisses actions");
            }
        }
        TabBar* docTabs = nullptr;
        app.root().forEach([&](Widget& w) {
            if (auto* t = dynamic_cast<TabBar*>(&w)) docTabs = t;
        });
        if (docTabs && docTabs->onClose) {
            const auto* firstSource = reader->stage()->source();
            Button* darkChip = nullptr;
            Button* lightChip = nullptr;
            app.root().forEach([&](Widget& w) {
                if (auto* b = dynamic_cast<Button*>(&w)) {
                    if (b->tooltip == "Dark page") darkChip = b;
                    if (b->tooltip == "Light page") lightChip = b;
                }
            });
            check(darkChip && lightChip, "inspector exposes page theme buttons");
            if (darkChip && inspector) {
                app.state().rightPanelCollapsed = false;
                inspector->setCollapsedImmediately(false);
                app.root().layoutTree();
                const Point p = darkChip->toWindow({22.f, 22.f});
                PointerEvent pick{};
                pick.button = MouseButton::Left;
                pick.pos = pick.windowPos = p;
                pick.timestamp = 5.0;
                pick.phase = PointerPhase::Down;
                app.pointer(pick);
                pick.phase = PointerPhase::Up;
                app.pointer(pick);
                check(reader->stage()->pageTheme() == PageTheme::Dark &&
                      app.state().theme.pageTheme() == PageTheme::Dark,
                      "right-panel Dark chip click updates actual PDF rendering state");
            }
            reader->stage()->setZoom(1.65f, 0.0);
            reader->stage()->relayout(1.0);
            KeyEvent scroll{};
            scroll.pressed = true;
            scroll.key = Key::Right;
            reader->stage()->onKey(scroll);
            scroll.key = Key::Down;
            reader->stage()->onKey(scroll);
            const auto firstView = reader->stage()->captureViewState(5.0);
            reader->stage()->selectAll();
            check(reader->stage()->hasSelection(), "first document has a selection");
            const fs::path second = fs::temp_directory_path() / "folio-second-tab.pdf";
            fs::copy_file(std::string(FOLIO_TEST_DATA_DIR) + "/folio-test.pdf",
                          second, fs::copy_options::overwrite_existing);
            auto prepared = std::make_shared<folio::engine::PdfiumPageSource>();
            check(prepared->open(second.string()), "second PDF prepares successfully");
            std::weak_ptr<folio::engine::PdfiumPageSource> closedWorker = prepared;
            check(reader->openPreparedFile(second.string(), prepared, 6.0),
                  "second PDF opens as a prepared tab");
            check(!reader->stage()->hasSelection(),
                  "switching PDFs clears stale glyph selection overlays");
            prepared.reset();
            if (lightChip) lightChip->onClick();
            check(reader->stage()->pageTheme() == PageTheme::Light,
                  "other PDF can choose independent page appearance");
            docTabs->selectTab(0, 7.0);
            check(reader->stage()->source() == firstSource,
                  "switching tabs reuses the parsed worker without opening PDF again");
            const auto returned = reader->stage()->captureViewState(7.0);
            check(std::fabs(returned.zoom - firstView.zoom) < 0.001f &&
                  std::fabs(returned.scrollX - firstView.scrollX) < 0.01f &&
                  std::fabs(returned.scrollY - firstView.scrollY) < 0.01f &&
                  returned.page == firstView.page &&
                  reader->stage()->pageTheme() == PageTheme::Dark,
                  "tab switch restores zoom, XY scroll, page and page theme");
            docTabs->onClose(1);
            for (int i = 0; i < 200 && !closedWorker.expired(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            check(closedWorker.expired(), "closed tab releases its worker off the UI thread");
            fs::remove(second);
            docTabs->onClose(0);
            check(reader->stage()->source() == nullptr,
                  "closing last tab frees the mapped PDF and decoded pages");
            check(!app.state().recents.empty(),
                  "closed real document remains in recent history");
        }
    }
#endif
}
