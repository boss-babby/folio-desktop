#include "podofo/PodofoDocument.h"

#include <podofo/podofo.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <set>

namespace folio::engine {

using namespace PoDoFo;

struct PodofoDocument::Impl {
    PdfMemDocument doc;
};

namespace {

/// Marks an annotation as ours, so a re-save replaces Folio's work and
/// leaves anything another application wrote alone. /T (title) is the
/// conventional place for an author string and is what viewers display.
constexpr const char* kFolioTag = "Folio";

/// Page geometry needed to map normalised UI coordinates into PDF user
/// space: the MediaBox origin, its size, and the page rotation.
struct PageFrame {
    double x0 = 0, y0 = 0, w = 612, h = 792;
    int    rotation = 0;
};

PageFrame frameOf(PdfPage* page) {
    PageFrame f;
    const PdfRect mb = page->GetMediaBox();
    f.x0 = mb.GetLeft();
    f.y0 = mb.GetBottom();
    f.w = mb.GetWidth();
    f.h = mb.GetHeight();
    f.rotation = ((page->GetRotation() % 360) + 360) % 360;
    return f;
}

/// Normalised, top-left-origin point -> PDF user space.
///
/// The UI works in the *displayed* page, so when /Rotate is non-zero the
/// normalised axes are not the page's axes. Undoing that here is the only
/// reason this is not two lines; getting it wrong puts annotations on the
/// right page in the wrong place, which looks like a coordinate bug
/// everywhere except the one file that has /Rotate set.
void toUser(const PageFrame& f, float nx, float ny, double& ux, double& uy) {
    double rx = nx, ry = ny;  // 0..1 in displayed space
    switch (f.rotation) {
        case 90:  { const double t = rx; rx = ry;       ry = 1.0 - t; break; }
        case 180: { rx = 1.0 - rx;       ry = 1.0 - ry;               break; }
        case 270: { const double t = rx; rx = 1.0 - ry; ry = t;       break; }
        default: break;
    }
    ux = f.x0 + rx * f.w;
    uy = f.y0 + (1.0 - ry) * f.h;   // PDF y grows upwards
}

PdfRect rectOf(const PageFrame& f, const AnnotSpec& a) {
    double x1, y1, x2, y2;
    toUser(f, a.l, a.t, x1, y1);
    toUser(f, a.r, a.b, x2, y2);
    const double left = std::min(x1, x2), bottom = std::min(y1, y2);
    return PdfRect(left, bottom, std::fabs(x2 - x1), std::fabs(y2 - y1));
}

/// A transparency ExtGState. Highlights also multiply, so text underneath
/// stays readable instead of being painted over.
PdfExtGState* makeGState(PdfMemDocument& doc, double alpha, bool multiply) {
    auto* gs = new PdfExtGState(&doc);
    gs->SetFillOpacity(alpha);
    gs->SetStrokeOpacity(alpha);
    if (multiply) gs->SetBlendMode("Multiply");
    return gs;
}

/// Build the /AP form XObject for an annotation and attach it.
///
/// The XObject's coordinate system is the annotation rectangle translated to
/// the origin, so everything inside is drawn relative to (0,0).
void buildAppearance(PdfMemDocument& doc, PdfAnnotation* annot,
                     const PdfRect& box, const AnnotSpec& a,
                     const PageFrame& f) {
    if (box.GetWidth() <= 0.0 || box.GetHeight() <= 0.0) return;

    PdfRect local(0.0, 0.0, box.GetWidth(), box.GetHeight());
    auto* xobj = new PdfXObject(local, &doc);

    PdfPainter p;
    p.SetPage(xobj);

    const double cr = a.cr / 255.0, cg = a.cg / 255.0, cb = a.cb / 255.0;
    const bool multiply = a.kind == AnnotSpec::Kind::Highlight;
    PdfExtGState* gs = makeGState(doc, a.opacity, multiply);
    p.SetExtGState(gs);

    switch (a.kind) {
        case AnnotSpec::Kind::Highlight:
            p.SetColor(cr, cg, cb);
            p.Rectangle(0.0, 0.0, local.GetWidth(), local.GetHeight());
            p.Fill();
            break;

        case AnnotSpec::Kind::FreeText: {
            // A FreeText annotation without an appearance stream is drawn
            // by the viewer from /DA, and viewers disagree wildly about
            // what that means. Baking the glyphs into /AP is the only way
            // the saved file looks like what the editor showed.
            if (a.contents.empty()) break;
            const double h = local.GetHeight();
            const double size = a.fontSize > 0.f ? a.fontSize : h * 0.74;
            PdfFont* font =
                doc.CreateFont(a.fontName.empty() ? "Helvetica"
                                                  : a.fontName.c_str());
            if (!font) break;
            font->SetFontSize(static_cast<float>(size));
            p.SetFont(font);
            p.SetColor(cr, cg, cb);
            // PDF text sits on its baseline; the UI positions by the top of
            // the box, so drop by the ascender to line the two up.
            p.DrawText(0.0, h - size * 0.82, PdfString(a.contents.c_str()));
            break;
        }

        case AnnotSpec::Kind::Underline: {
            // Below the glyph box, clear of descenders. PDF 32000 leaves the
            // exact offset to the producer; 8% of the line box matches what
            // Acrobat draws closely enough to be indistinguishable.
            const double w = std::max(0.4, local.GetHeight() * 0.055);
            const double y = local.GetHeight() * 0.08;
            p.SetStrokingColor(cr, cg, cb);
            p.SetStrokeWidth(w);
            p.MoveTo(0.0, y);
            p.LineTo(local.GetWidth(), y);
            p.Stroke();
            break;
        }

        case AnnotSpec::Kind::StrikeOut: {
            const double w = std::max(0.4, local.GetHeight() * 0.055);
            const double y = local.GetHeight() * 0.42;  // bottom-up: x-height centre
            p.SetStrokingColor(cr, cg, cb);
            p.SetStrokeWidth(w);
            p.MoveTo(0.0, y);
            p.LineTo(local.GetWidth(), y);
            p.Stroke();
            break;
        }

        case AnnotSpec::Kind::Box:
            p.SetStrokingColor(cr, cg, cb);
            p.SetStrokeWidth(a.width);
            p.Rectangle(a.width * 0.5, a.width * 0.5,
                        std::max(0.1, local.GetWidth() - a.width),
                        std::max(0.1, local.GetHeight() - a.width));
            p.Stroke();
            break;

        case AnnotSpec::Kind::Ellipse:
            p.SetStrokingColor(cr, cg, cb);
            p.SetStrokeWidth(a.width);
            p.Ellipse(a.width * 0.5, a.width * 0.5,
                      std::max(0.1, local.GetWidth() - a.width),
                      std::max(0.1, local.GetHeight() - a.width));
            p.Stroke();
            break;

        case AnnotSpec::Kind::Ink: {
            if (a.points.size() < 2) break;
            p.SetStrokingColor(cr, cg, cb);
            p.SetStrokeWidth(a.width);
            p.SetLineCapStyle(ePdfLineCapStyle_Round);
            p.SetLineJoinStyle(ePdfLineJoinStyle_Round);
            bool started = false;
            for (const auto& pt : a.points) {
                double ux = 0, uy = 0;
                toUser(f, pt[0], pt[1], ux, uy);
                const double lx = ux - box.GetLeft();
                const double ly = uy - box.GetBottom();
                if (!started) { p.MoveTo(lx, ly); started = true; }
                else          { p.LineTo(lx, ly); }
            }
            p.Stroke();
            break;
        }

        case AnnotSpec::Kind::Note: {
            // A filled rounded pin. Drawing our own rather than relying on
            // the viewer's /Name icon, which differs between readers and is
            // frequently not drawn at all.
            const double w = local.GetWidth(), h = local.GetHeight();
            p.SetColor(cr, cg, cb);
            p.Rectangle(0.0, h * 0.18, w, h * 0.72);
            p.Fill();
            p.SetColor(1.0, 1.0, 1.0);
            p.Rectangle(w * 0.22, h * 0.42, w * 0.56, h * 0.08);
            p.Fill();
            p.Rectangle(w * 0.22, h * 0.60, w * 0.40, h * 0.08);
            p.Fill();
            break;
        }
    }

    p.FinishPage();
    annot->SetAppearanceStream(xobj);
}

}  // namespace

PodofoDocument::PodofoDocument() : impl_(std::make_unique<Impl>()) {}
PodofoDocument::~PodofoDocument() = default;

bool PodofoDocument::open(const std::string& path) {
    error_.clear();
    dirty_ = false;
    try {
        impl_ = std::make_unique<Impl>();
        impl_->doc.Load(path.c_str(), /*bForUpdate*/ true);
    } catch (const PdfError& e) {
        error_ = e.what() ? e.what() : "PoDoFo could not open the file";
        impl_.reset(new Impl());
        return false;
    }
    path_ = path;
    return pageCount() > 0;
}

bool PodofoDocument::isOpen() const noexcept {
    return impl_ && !path_.empty();
}

int PodofoDocument::pageCount() const {
    if (!impl_) return 0;
    try {
        return impl_->doc.GetPageCount();
    } catch (const PdfError&) {
        return 0;
    }
}

std::array<double, 2> PodofoDocument::pageSize(int index) const {
    if (!impl_ || index < 0 || index >= pageCount()) return {0.0, 0.0};
    try {
        PdfPage* page = impl_->doc.GetPage(index);
        const PageFrame f = frameOf(page);
        if (f.rotation == 90 || f.rotation == 270) return {f.h, f.w};
        return {f.w, f.h};
    } catch (const PdfError&) {
        return {0.0, 0.0};
    }
}

int PodofoDocument::pageRotation(int index) const {
    if (!impl_ || index < 0 || index >= pageCount()) return 0;
    try {
        return frameOf(impl_->doc.GetPage(index)).rotation;
    } catch (const PdfError&) {
        return 0;
    }
}

bool PodofoDocument::rotatePage(int index, int deltaDegrees) {
    if (!impl_ || index < 0 || index >= pageCount()) return false;
    try {
        PdfPage* page = impl_->doc.GetPage(index);
        const int next = (((page->GetRotation() + deltaDegrees) % 360) + 360) % 360;
        page->SetRotation(next);
        dirty_ = true;
        return true;
    } catch (const PdfError& e) {
        error_ = e.what() ? e.what() : "rotate failed";
        return false;
    }
}

bool PodofoDocument::deletePage(int index) {
    if (!impl_ || index < 0 || index >= pageCount()) return false;
    if (pageCount() <= 1) {
        error_ = "a document must keep at least one page";
        return false;
    }
    try {
        impl_->doc.DeletePages(index, 1);
        dirty_ = true;
        return true;
    } catch (const PdfError& e) {
        error_ = e.what() ? e.what() : "delete failed";
        return false;
    }
}

bool PodofoDocument::insertBlankPage(int atIndex) {
    if (!impl_) return false;
    const int n = pageCount();
    atIndex = std::clamp(atIndex, 0, n);
    try {
        // Match the neighbour's size; a letter-sized page dropped into an A4
        // document is the kind of detail that makes a tool feel careless.
        PdfRect size(0.0, 0.0, 595.0, 842.0);
        if (n > 0) {
            PdfPage* ref = impl_->doc.GetPage(std::min(atIndex, n - 1));
            size = ref->GetMediaBox();
        }
        // InsertPage takes the index to insert *after*, hence the -1.
        impl_->doc.InsertPage(size, atIndex - 1);
        dirty_ = true;
        return true;
    } catch (const PdfError& e) {
        error_ = e.what() ? e.what() : "insert failed";
        return false;
    }
}

// ---------------------------------------------------------------------------
// Orphan sweep
//
// PoDoFo's InsertPages() is implemented on top of Append(), which copies
// EVERY object out of the source document and then links only the requested
// pages into the new page tree. Everything else - the source's own
// catalogue, its full page tree, and the content streams of every page we
// did not ask for - stays in the object list and is written to disk.
//
// That is not just wasted space. Extracting page 3 of a contract would ship
// a file that still contains the text of pages 1-11: unreachable from the
// root, invisible in every viewer, and recoverable in seconds by anyone who
// parses the objects directly. For an extract feature that is a
// confidentiality bug, so this sweep is mandatory rather than an
// optimisation.
//
// PoDoFo 0.9.8 appears to offer exactly this as PdfVecObjects::CollectGarbage,
// but on a freshly built extract document it is a no-op - measured at 33
// objects in, 33 objects out, with the duplicate catalogue still present.
// So we do the mark-and-sweep ourselves: walk every reference reachable from
// the trailer, then delete whatever we never reached.
// ---------------------------------------------------------------------------
namespace {

void markReachable(const PdfObject* o, const PdfVecObjects& objs,
                   std::set<PdfReference>& seen) {
    if (!o) return;
    if (o->IsReference()) {
        const PdfReference r = o->GetReference();
        if (!seen.insert(r).second) return;   // already walked; also breaks cycles
        markReachable(objs.GetObject(r), objs, seen);
        return;
    }
    if (o->IsDictionary())
        for (const auto& kv : o->GetDictionary().GetKeys())
            markReachable(kv.second, objs, seen);
    if (o->IsArray())
        for (const auto& e : o->GetArray()) markReachable(&e, objs, seen);
}

/// Deletes every object in `doc` not reachable from its trailer.
void sweepUnreachable(PdfMemDocument& doc) {
    PdfVecObjects& objs = doc.GetObjects();
    std::set<PdfReference> seen;
    markReachable(doc.GetTrailer(), objs, seen);

    std::vector<PdfReference> dead;
    for (auto it = objs.begin(); it != objs.end(); ++it)
        if (*it && !seen.count((*it)->Reference())) dead.push_back((*it)->Reference());

    // Collected first, then removed: RemoveObject mutates the vector we are
    // iterating. RemoveObject hands back ownership, so each one is deleted.
    for (const PdfReference& r : dead) delete objs.RemoveObject(r);
}

}  // namespace

bool PodofoDocument::extractPages(const std::vector<int>& pages,
                                  const std::string& outPath) {
    if (!impl_ || pages.empty()) return false;
    try {
        PdfMemDocument out;
        for (int p : pages) {
            if (p < 0 || p >= pageCount()) continue;
            out.InsertPages(impl_->doc, p, 1);
        }
        if (out.GetPageCount() == 0) {
            error_ = "no valid pages to extract";
            return false;
        }

        sweepUnreachable(out);
        out.Write(outPath.c_str());
        return true;
    } catch (const PdfError& e) {
        error_ = e.what() ? e.what() : "extract failed";
        return false;
    }
}

bool PodofoDocument::writeAnnotations(const std::vector<AnnotSpec>& annots) {
    if (!impl_) return false;
    try {
        // Every page we are about to write to gets its existing Folio
        // annotations removed first, so saving twice does not duplicate
        // them. Annotations from other applications are left alone - they
        // are not ours to manage.
        std::set<int> touched;
        for (const auto& a : annots) touched.insert(a.page);

        for (int p : touched) {
            if (p < 0 || p >= pageCount()) continue;
            PdfPage* page = impl_->doc.GetPage(p);
            for (int i = page->GetNumAnnots() - 1; i >= 0; --i) {
                PdfAnnotation* existing = page->GetAnnotation(i);
                if (!existing) continue;
                const PdfObject* title =
                    existing->GetObject()->GetDictionary().GetKey(PdfName("T"));
                if (title && title->IsString() &&
                    title->GetString().GetStringUtf8() == kFolioTag) {
                    page->DeleteAnnotation(i);
                }
            }
        }

        for (const auto& a : annots) {
            if (a.page < 0 || a.page >= pageCount()) continue;
            PdfPage* page = impl_->doc.GetPage(a.page);
            const PageFrame f = frameOf(page);
            const PdfRect box = rectOf(f, a);

            EPdfAnnotation type = ePdfAnnotation_Highlight;
            switch (a.kind) {
                case AnnotSpec::Kind::Highlight: type = ePdfAnnotation_Highlight; break;
                case AnnotSpec::Kind::Underline: type = ePdfAnnotation_Underline; break;
                case AnnotSpec::Kind::StrikeOut: type = ePdfAnnotation_StrikeOut; break;
                case AnnotSpec::Kind::Ink:       type = ePdfAnnotation_Ink;       break;
                case AnnotSpec::Kind::Note:      type = ePdfAnnotation_Text;      break;
                case AnnotSpec::Kind::Box:       type = ePdfAnnotation_Square;    break;
                case AnnotSpec::Kind::Ellipse:   type = ePdfAnnotation_Circle;    break;
                case AnnotSpec::Kind::FreeText:  type = ePdfAnnotation_FreeText;  break;
            }

            PdfAnnotation* annot = page->CreateAnnotation(type, box);
            annot->SetColor(a.cr / 255.0, a.cg / 255.0, a.cb / 255.0);
            annot->SetTitle(PdfString(kFolioTag));
            if (!a.contents.empty())
                annot->SetContents(PdfString(a.contents.c_str()));
            annot->SetFlags(4);  // Print

            PdfDictionary& dict = annot->GetObject()->GetDictionary();
            dict.AddKey(PdfName("CA"), PdfObject(static_cast<double>(a.opacity)));

            const bool textMarkup = a.kind == AnnotSpec::Kind::Highlight ||
                                    a.kind == AnnotSpec::Kind::Underline ||
                                    a.kind == AnnotSpec::Kind::StrikeOut;
            if (textMarkup) {
                // /QuadPoints is what a conforming viewer uses to lay a text
                // markup annotation over the glyphs. Order is upper-left,
                // upper-right, lower-left, lower-right - not a rectangle
                // walk, which is a classic way to produce an invisible
                // highlight.
                PdfArray q;
                const double x1 = box.GetLeft(), x2 = box.GetLeft() + box.GetWidth();
                const double y1 = box.GetBottom() + box.GetHeight();
                const double y2 = box.GetBottom();
                for (double v : {x1, y1, x2, y1, x1, y2, x2, y2})
                    q.push_back(PdfObject(v));
                annot->SetQuadPoints(q);
            } else if (a.kind == AnnotSpec::Kind::Ink && a.points.size() >= 2) {
                PdfArray stroke;
                for (const auto& pt : a.points) {
                    double ux = 0, uy = 0;
                    toUser(f, pt[0], pt[1], ux, uy);
                    stroke.push_back(PdfObject(ux));
                    stroke.push_back(PdfObject(uy));
                }
                PdfArray inkList;
                inkList.push_back(PdfObject(stroke));
                dict.AddKey(PdfName("InkList"), inkList);
                PdfArray bs;
                dict.AddKey(PdfName("BS"), PdfObject(PdfDictionary()));
                (void)bs;
            }

            buildAppearance(impl_->doc, annot, box, a, f);
        }
        dirty_ = true;
        return true;
    } catch (const PdfError& e) {
        error_ = e.what() ? e.what() : "writing annotations failed";
        return false;
    }
}

bool PodofoDocument::saveIncremental() {
    if (!impl_ || path_.empty()) return false;
    try {
        impl_->doc.WriteUpdate(path_.c_str());
        dirty_ = false;
        return true;
    } catch (const PdfError& e) {
        error_ = e.what() ? e.what() : "incremental save failed";
        return false;
    }
}

bool PodofoDocument::saveAs(const std::string& outPath) {
    if (!impl_) return false;
    // Always write to a sibling temporary and rename over the target.
    //
    // Two reasons. The document was loaded for update, so PoDoFo still holds
    // the source file; writing a full save straight back over it truncates
    // the bytes it is reading from and produces a corrupt result - which is
    // exactly what a save-in-place looked like before this. And a rename is
    // atomic, so an interrupted save leaves the previous file intact rather
    // than a half-written one.
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path target(outPath);
    const fs::path tmp = target.parent_path() /
                         (target.filename().string() + ".folio-tmp");
    try {
        impl_->doc.Write(tmp.string().c_str());
    } catch (const PdfError& e) {
        error_ = e.what() ? e.what() : "save failed";
        fs::remove(tmp, ec);
        return false;
    }

    fs::rename(tmp, target, ec);
    if (ec) {
        // Different filesystems: fall back to copy and unlink.
        fs::copy_file(tmp, target, fs::copy_options::overwrite_existing, ec);
        fs::remove(tmp, ec);
        if (ec) { error_ = "could not replace " + outPath; return false; }
    }

    dirty_ = false;
    // The in-memory document still refers to the bytes we just replaced, so
    // anything further would be built on a stale view. Reopen.
    if (target == fs::path(path_)) return open(path_);
    return true;
}

PodofoDocument::Metadata PodofoDocument::metadata() const {
    Metadata m;
    if (!impl_) return m;
    try {
        PdfInfo* info = impl_->doc.GetInfo();
        if (!info) return m;
        // GetTitle() and friends return an invalid PdfString when the key is
        // absent, and GetLength() on one of those logs an error to stderr.
        auto get = [](const PdfString& s) {
            return s.IsValid() && s.GetLength() ? s.GetStringUtf8() : std::string{};
        };
        m.title = get(info->GetTitle());
        m.author = get(info->GetAuthor());
        m.subject = get(info->GetSubject());
        m.creator = get(info->GetCreator());
        m.producer = get(info->GetProducer());
    } catch (const PdfError&) {
    }
    return m;
}

bool PodofoDocument::setMetadata(const Metadata& m) {
    if (!impl_) return false;
    try {
        PdfInfo* info = impl_->doc.GetInfo();
        if (!info) return false;
        if (!m.title.empty())   info->SetTitle(PdfString(m.title.c_str()));
        if (!m.author.empty())  info->SetAuthor(PdfString(m.author.c_str()));
        if (!m.subject.empty()) info->SetSubject(PdfString(m.subject.c_str()));
        dirty_ = true;
        return true;
    } catch (const PdfError& e) {
        error_ = e.what() ? e.what() : "metadata failed";
        return false;
    }
}

}  // namespace folio::engine
