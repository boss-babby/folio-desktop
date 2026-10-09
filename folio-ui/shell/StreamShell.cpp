#include "shell/StreamShell.h"

#include "SkiaPaintTarget.h"
#include "SkiaTextSystem.h"
#include "ShadowCache.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkColorSpace.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace folio::shell {
namespace {

using Clock = std::chrono::steady_clock;

double nowSeconds() {
    static const auto epoch = Clock::now();
    return std::chrono::duration<double>(Clock::now() - epoch).count();
}

constexpr int kTargetFps = 60;
constexpr int kJpegQuality = 82;
/// Even when nothing animates, send a frame occasionally so a client that
/// connected during an idle period is not left staring at a blank <img>.
constexpr double kIdleKeepaliveSec = 1.0;

/// The browser client. Served inline so the binary has no asset dependency
/// beyond the font.
const char* kIndexHtml = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Folio - native C++/Skia UI</title>
<style>
  :root { color-scheme: light dark; }
  * { box-sizing: border-box; }
  html,body { margin:0; height:100%; background:#14151c;
              font:13px/1.4 ui-sans-serif,system-ui,-apple-system,"Segoe UI",sans-serif;
              color:#e7e9f2; overflow:hidden; }
  #wrap { position:fixed; inset:0; display:flex; align-items:center;
          justify-content:center; padding:10px; }
  #screen { max-width:100%; max-height:100%; display:block; cursor:default;
            border-radius:10px; box-shadow:0 18px 60px rgba(0,0,0,.6);
            image-rendering:-webkit-optimize-contrast; outline:none; }
  #hud { position:fixed; left:12px; bottom:10px; font:11px/1.5 ui-monospace,
         SFMono-Regular,Menlo,monospace; color:#9aa0b8; background:rgba(0,0,0,.45);
         padding:5px 9px; border-radius:7px; pointer-events:none;
         backdrop-filter:blur(6px); white-space:pre; }
  #hint { position:fixed; right:12px; bottom:10px; font-size:11px; color:#71768f;
          background:rgba(0,0,0,.45); padding:5px 9px; border-radius:7px;
          pointer-events:none; }
</style>
</head>
<body>
<div id="wrap"><img id="screen" src="/stream" alt="Folio UI" tabindex="0"></div>
<div id="hud">connecting...</div>
<div id="hint">click the window first, then use the keyboard</div>
<script>
const img = document.getElementById('screen');
const hud = document.getElementById('hud');
let queue = [], sending = false;

// Map a browser event to surface coordinates. The <img> is scaled to fit,
// so client pixels are not surface pixels.
function pt(e) {
  const r = img.getBoundingClientRect();
  const sx = (img.naturalWidth  || r.width)  / r.width;
  const sy = (img.naturalHeight || r.height) / r.height;
  return [ (e.clientX - r.left) * sx, (e.clientY - r.top) * sy ];
}
function mods(e) {
  return (e.shiftKey?1:0) | (e.ctrlKey?2:0) | (e.altKey?4:0) | (e.metaKey?8:0);
}
function push(rec) {
  queue.push(rec);
  if (!sending) flush();
}
async function flush() {
  if (!queue.length) { sending = false; return; }
  sending = true;
  const batch = queue.join('\n');
  queue = [];
  try { await fetch('/input', { method:'POST', body:batch }); } catch (_) {}
  flush();
}

// Coalesce pointer moves to one per animation frame: sending every
// mousemove floods the input queue and adds latency without adding fidelity.
let pendingMove = null;
img.addEventListener('mousemove', e => {
  if (window.PointerEvent) return;   // pointermove already covered it
  const [x,y] = pt(e);
  pendingMove = `m,${x.toFixed(1)},${y.toFixed(1)},${e.buttons},${mods(e)}`;
});
function pumpMove() {
  if (pendingMove) { push(pendingMove); pendingMove = null; }
  requestAnimationFrame(pumpMove);
}
requestAnimationFrame(pumpMove);

// Pointer Events give us touch, pen and mouse through one API, with
// pointerType and pressure attached. Registered in preference to the mouse
// handlers below; browsers that support them never fire both for the same
// gesture once we preventDefault and capture.
const PT = { mouse:0, touch:1, pen:2 };
function ptype(e) { return PT[e.pointerType] !== undefined ? PT[e.pointerType] : 0; }
if (window.PointerEvent) {
  img.style.touchAction = 'none';   // we handle panning and pinching
  const send3 = (ch, e) => {
    const [x,y] = pt(e);
    push(`${ch},${x.toFixed(1)},${y.toFixed(1)},${e.button < 0 ? 0 : e.button},`
       + `${mods(e)},${ptype(e)},${e.pointerId},${(e.pressure||0).toFixed(3)}`);
  };
  img.addEventListener('pointerdown', e => {
    img.focus();
    img.setPointerCapture(e.pointerId);
    send3('p', e);
    e.preventDefault();
  });
  // Coalesced moves: a 120Hz digitiser emits far more points than we can
  // render, and getCoalescedEvents gives us the ones the browser merged -
  // which is what keeps a fast pen stroke smooth instead of polygonal.
  img.addEventListener('pointermove', e => {
    if (!e.buttons) { send3('q', e); return; }
    const list = e.getCoalescedEvents ? e.getCoalescedEvents() : [e];
    for (const c of list) send3('q', c);
    e.preventDefault();
  }, { passive:false });
  const up = e => { send3('s', e); e.preventDefault(); };
  img.addEventListener('pointerup', up);
  img.addEventListener('pointercancel', up);
}

img.addEventListener('mousedown', e => {
  if (window.PointerEvent) { e.preventDefault(); return; }
  img.focus();
  const [x,y] = pt(e);
  push(`d,${x.toFixed(1)},${y.toFixed(1)},${e.button},${mods(e)}`);
  e.preventDefault();
});
window.addEventListener('mouseup', e => {
  if (window.PointerEvent) return;
  const [x,y] = pt(e);
  push(`u,${x.toFixed(1)},${y.toFixed(1)},${e.button},${mods(e)}`);
});
img.addEventListener('mouseleave', () => push('x'));
img.addEventListener('wheel', e => {
  const [x,y] = pt(e);
  // deltaY is pixels (mode 0) or lines (mode 1); normalise to notches.
  const k = e.deltaMode === 1 ? 1/3 : 1/100;
  push(`w,${x.toFixed(1)},${y.toFixed(1)},${(-e.deltaX*k).toFixed(3)},${(-e.deltaY*k).toFixed(3)},${mods(e)}`);
  e.preventDefault();
}, { passive:false });
img.addEventListener('contextmenu', e => e.preventDefault());

// Opt-in live resize: load the page as /?fit and the stream tracks the
// browser viewport, so the responsive breakpoints can actually be seen.
// Off by default - the server's --size stays authoritative otherwise.
if (location.search.indexOf('fit') >= 0) {
  document.getElementById('wrap').style.cssText += ';width:100%;height:100%';
  img.style.cssText += ';width:100%;height:100%;object-fit:fill';
  let t = null;
  const fit = () => {
    const w = Math.round(document.documentElement.clientWidth);
    const h = Math.round(document.documentElement.clientHeight);
    push(`r,${w},${h}`);
  };
  // Debounced: a drag-resize otherwise reallocates the surface on every
  // frame the browser emits.
  window.addEventListener('resize', () => {
    clearTimeout(t);
    t = setTimeout(fit, 150);
  });
  fit();
}

img.addEventListener('keydown', e => {
  const printable = e.key.length === 1 && !e.ctrlKey && !e.metaKey;
  push(`k,${encodeURIComponent(e.key)},1,${mods(e)},${printable?encodeURIComponent(e.key):''}`);
  if (['Tab',' ','ArrowUp','ArrowDown','ArrowLeft','ArrowRight','PageUp','PageDown','Home','End'].includes(e.key)
      || (e.ctrlKey && 'fpszno0+-='.includes(e.key.toLowerCase()))) {
    e.preventDefault();
  }
});
img.addEventListener('keyup', e => push(`k,${encodeURIComponent(e.key)},0,${mods(e)},`));

// Stats
let lastBytes = 0;
async function poll() {
  try {
    const r = await fetch('/stats');
    const s = await r.json();
    hud.textContent =
      `${s.w}x${s.h}  render ${s.renderMs.toFixed(1)}ms  encode ${s.encodeMs.toFixed(1)}ms  ` +
      `${s.fps.toFixed(0)} fps  ${(s.lastBytes/1024).toFixed(0)}KB/frame  ${s.widgets} widgets`;
    lastBytes = s.lastBytes;
  } catch (_) { hud.textContent = 'disconnected'; }
  setTimeout(poll, 500);
}
poll();
img.addEventListener('load', () => img.focus());
setTimeout(() => img.focus(), 300);
</script>
</body>
</html>
)HTML";

ui::Key mapKey(const std::string& k) {
    using K = ui::Key;
    if (k == "Escape")     return K::Escape;
    if (k == "Enter")      return K::Enter;
    if (k == "Tab")        return K::Tab;
    if (k == "Backspace")  return K::Backspace;
    if (k == "Delete")     return K::Delete;
    if (k == " ")          return K::Space;
    if (k == "ArrowLeft")  return K::Left;
    if (k == "ArrowRight") return K::Right;
    if (k == "ArrowUp")    return K::Up;
    if (k == "ArrowDown")  return K::Down;
    if (k == "Home")       return K::Home;
    if (k == "End")        return K::End;
    if (k == "PageUp")     return K::PageUp;
    if (k == "PageDown")   return K::PageDown;
    if (k == "+" || k == "=") return K::Plus;
    if (k == "-")          return K::Minus;
    if (k == "0")          return K::Digit0;
    if (k.size() == 1) {
        switch (std::tolower(static_cast<unsigned char>(k[0]))) {
            case 'a': return K::A;
            case 'c': return K::C;
            case 'f': return K::F;
            case 'n': return K::N;
            case 'o': return K::O;
            case 'p': return K::P;
            case 's': return K::S;
            case 't': return K::T;
            case 'w': return K::W;
            case 'y': return K::Y;
            case 'z': return K::Z;
            default: break;
        }
    }
    return K::Unknown;
}

/// Minimal percent-decoding for the key/text fields.
std::string urlDecode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            out.push_back(static_cast<char>(
                std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16)));
            i += 2;
        } else if (s[i] == '+') {
            out.push_back(' ');
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

std::vector<std::string> split(const std::string& s, char d) {
    std::vector<std::string> out;
    std::string cur;
    std::istringstream is(s);
    while (std::getline(is, cur, d)) out.push_back(cur);
    return out;
}

bool sendAll(int fd, const char* p, std::size_t n) {
    std::size_t sent = 0;
    while (sent < n) {
        const ssize_t k = ::send(fd, p + sent, n - sent, MSG_NOSIGNAL);
        if (k <= 0) return false;
        sent += static_cast<std::size_t>(k);
    }
    return true;
}

}  // namespace

StreamShell::StreamShell(int w, int h, float dpr)
    : width_(w), height_(h), dpr_(dpr),
      app_(static_cast<float>(w) / dpr, static_cast<float>(h) / dpr) {
    // BGRA8888 premultiplied matches the project-wide pixel format
    // (RasterTile.h), so a PDF tile can be blitted with no swizzle.
    const SkImageInfo info = SkImageInfo::Make(w, h, kBGRA_8888_SkColorType,
                                               kPremul_SkAlphaType,
                                               SkColorSpace::MakeSRGB());
    surface_ = SkSurfaces::Raster(info);
    render::skia::SkiaTextSystem::instance().initialise(
        std::string(FOLIO_ASSET_DIR) + "/fonts");
}

StreamShell::~StreamShell() { stop(); }

ShellStats StreamShell::stats() const {
    std::lock_guard<std::mutex> lk(statsMu_);
    ShellStats s = stats_;
    s.clients = clients_.load();
    return s;
}

void StreamShell::queueInput(const std::string& record) {
    std::lock_guard<std::mutex> lk(inputMu_);
    if (inputQueue_.size() > 512) inputQueue_.pop_front();  // never unbounded
    inputQueue_.push_back(record);
}

void StreamShell::drainInput(double now) {
    std::deque<std::string> local;
    {
        std::lock_guard<std::mutex> lk(inputMu_);
        local.swap(inputQueue_);
    }
    for (const std::string& rec : local) {
        const auto f = split(rec, ',');
        if (f.empty()) continue;

        ui::PointerEvent pe;
        pe.timestamp = now;

        if (f[0] == "m" && f.size() >= 5) {
            pe.phase = ui::PointerPhase::Move;
            pe.windowPos = {std::strtof(f[1].c_str(), nullptr) / dpr_,
                            std::strtof(f[2].c_str(), nullptr) / dpr_};
            pe.buttonsDown = static_cast<std::uint8_t>(std::atoi(f[3].c_str()));
            pe.modifiers = static_cast<std::uint32_t>(std::atoi(f[4].c_str()));
            lastPointer_ = pe.windowPos;
            app_.pointer(pe);
        } else if ((f[0] == "d" || f[0] == "u") && f.size() >= 5) {
            pe.phase = (f[0] == "d") ? ui::PointerPhase::Down : ui::PointerPhase::Up;
            pe.windowPos = {std::strtof(f[1].c_str(), nullptr) / dpr_,
                            std::strtof(f[2].c_str(), nullptr) / dpr_};
            // Double-click detection. The browser sends raw presses, so the
            // shell has to pair them up the same way the X11 and Win32
            // backends do - 400 ms and 4 logical pixels.
            if (pe.phase == ui::PointerPhase::Down) {
                const bool near =
                    std::abs(pe.windowPos.x - lastClickPos_.x) < 4.f &&
                    std::abs(pe.windowPos.y - lastClickPos_.y) < 4.f;
                pe.clickCount = (near && now - lastClickTime_ < 0.4) ? 2 : 1;
                lastClickTime_ = now;
                lastClickPos_ = pe.windowPos;
            }
            const int btn = std::atoi(f[3].c_str());
            pe.button = (btn == 2) ? ui::MouseButton::Right
                                   : (btn == 1 ? ui::MouseButton::Middle
                                               : ui::MouseButton::Left);
            pe.modifiers = static_cast<std::uint32_t>(std::atoi(f[4].c_str()));
            lastPointer_ = pe.windowPos;
            app_.pointer(pe);
        } else if (f[0] == "w" && f.size() >= 6) {
            pe.phase = ui::PointerPhase::Wheel;
            pe.windowPos = {std::strtof(f[1].c_str(), nullptr) / dpr_,
                            std::strtof(f[2].c_str(), nullptr) / dpr_};
            pe.wheelX = std::strtof(f[3].c_str(), nullptr);
            pe.wheelY = std::strtof(f[4].c_str(), nullptr);
            pe.modifiers = static_cast<std::uint32_t>(std::atoi(f[5].c_str()));
            app_.pointer(pe);
        } else if ((f[0] == "p" || f[0] == "q" || f[0] == "s") &&
                   f.size() >= 8) {
            // Pointer Events transport: p=down q=move s=up/cancel.
            // Deliberately NOT 'r': that is the resize record, and telling
            // the two apart by field count alone means a short write turns
            // a finger lift into a window resize to 546x846.
            //   p,x,y,button,mods,type,id,pressure
            // Carries what the mouse records cannot - which device is
            // driving, which finger it is, and how hard it is pressed.
            pe.phase = f[0] == "p"   ? ui::PointerPhase::Down
                       : f[0] == "q" ? ui::PointerPhase::Move
                                     : ui::PointerPhase::Up;
            pe.windowPos = {std::strtof(f[1].c_str(), nullptr) / dpr_,
                            std::strtof(f[2].c_str(), nullptr) / dpr_};
            const int btn = std::atoi(f[3].c_str());
            pe.button = btn == 2   ? ui::MouseButton::Right
                        : btn == 1 ? ui::MouseButton::Middle
                                   : ui::MouseButton::Left;
            pe.modifiers = static_cast<std::uint32_t>(std::atoi(f[4].c_str()));
            const int ty = std::atoi(f[5].c_str());
            pe.pointerType = ty == 1   ? ui::PointerType::Touch
                             : ty == 2 ? ui::PointerType::Pen
                                       : ui::PointerType::Mouse;
            pe.pointerId =
                static_cast<ui::PointerId>(std::strtoul(f[6].c_str(), nullptr, 10));
            pe.pressure = std::strtof(f[7].c_str(), nullptr);
            // A mouse reports pressure 0 even while held; normalise so a
            // width curve can read `pressure` without asking what device
            // it came from.
            if (pe.pointerType != ui::PointerType::Pen)
                pe.pressure = (pe.phase == ui::PointerPhase::Up) ? 0.f : 1.f;

            if (pe.phase == ui::PointerPhase::Down) {
                const bool near =
                    std::abs(pe.windowPos.x - lastClickPos_.x) < 4.f &&
                    std::abs(pe.windowPos.y - lastClickPos_.y) < 4.f;
                pe.clickCount = (near && now - lastClickTime_ < 0.4) ? 2 : 1;
                lastClickTime_ = now;
                lastClickPos_ = pe.windowPos;
                pe.buttonsDown = 1;
            } else if (pe.phase == ui::PointerPhase::Move) {
                pe.buttonsDown = pe.pressure > 0.f ? 1 : 0;
            }
            lastPointer_ = pe.windowPos;
            app_.pointer(pe);
        } else if (f[0] == "x") {
            pe.phase = ui::PointerPhase::Cancel;
            app_.pointer(pe);
        } else if (f[0] == "r" && f.size() >= 3) {
            // Resize. There is no window manager behind the stream, so this
            // is the only way to cross a responsive breakpoint - whether
            // driven by a test script or by the page tracking its own
            // viewport. Runs on the render thread, so reallocating the
            // surface here races with nothing.
            const int nw = std::clamp(std::atoi(f[1].c_str()), 320, 4096);
            const int nh = std::clamp(std::atoi(f[2].c_str()), 240, 4096);
            if (nw != width_ || nh != height_) {
                width_ = nw;
                height_ = nh;
                const SkImageInfo info =
                    SkImageInfo::Make(nw, nh, kBGRA_8888_SkColorType,
                                      kPremul_SkAlphaType,
                                      SkColorSpace::MakeSRGB());
                surface_ = SkSurfaces::Raster(info);
                app_.resize(static_cast<float>(nw) / dpr_,
                            static_cast<float>(nh) / dpr_);
            }
        } else if (f[0] == "k" && f.size() >= 4) {
            ui::KeyEvent ke;
            const std::string keyName = urlDecode(f[1]);
            ke.key = mapKey(keyName);
            ke.pressed = f[2] == "1";
            ke.modifiers = static_cast<std::uint32_t>(std::atoi(f[3].c_str()));
            if (f.size() >= 5) ke.text = urlDecode(f[4]);
            if (ke.key != ui::Key::Unknown || !ke.text.empty()) app_.key(ke);
        }
    }
}

void StreamShell::renderOnce(double now) {
    SkCanvas* canvas = surface_->getCanvas();
    render::skia::SkiaPaintTarget target(canvas);

    const auto t0 = Clock::now();
    canvas->save();
    if (dpr_ != 1.f) canvas->scale(dpr_, dpr_);
    app_.paint(target, now, dpr_);
    canvas->restore();
    const auto t1 = Clock::now();

    // Idle reclaim. Any cached page bitmap that has not been touched in the
    // last few hundred cache operations is off-screen and unlikely to come
    // back soon, so it is handed back to the allocator rather than sitting
    // on the byte budget. Visible pages are touched every frame and survive.
    // This is what keeps a long reading session from ratcheting upwards.
    {
        static auto lastTrim = Clock::now();
        if (std::chrono::duration<double>(t1 - lastTrim).count() > 2.0) {
            lastTrim = t1;
            render::skia::SkiaPaintTarget::trimRasterCache(400);
        }
    }

    auto jpeg = FrameEncoder::encode(*surface_, Codec::Jpeg, kJpegQuality);
    const auto t2 = Clock::now();

    {
        std::lock_guard<std::mutex> lk(statsMu_);
        stats_.renderMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
        stats_.encodeMs = std::chrono::duration<double, std::milli>(t2 - t1).count();
        stats_.lastBytes = jpeg.size();
        ++stats_.frames;
    }
    publish(std::move(jpeg));
}

void StreamShell::publish(std::vector<std::uint8_t> jpeg) {
    if (jpeg.empty()) return;
    {
        std::lock_guard<std::mutex> lk(frameMu_);
        frame_ = std::move(jpeg);
        ++frameSeq_;
    }
    frameCv_.notify_all();
}

void StreamShell::installRoutes() {
    http_.route("/", [](const HttpRequest&, HttpResponse& res) {
        res.contentType = "text/html; charset=utf-8";
        res.body = kIndexHtml;
    });

    http_.route("/input", [this](const HttpRequest& req, HttpResponse& res) {
        std::istringstream is(req.body);
        std::string line;
        while (std::getline(is, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) queueInput(line);
        }
        res.status = 204;
        res.contentType = "text/plain";
    });

    http_.route("/stats", [this](const HttpRequest&, HttpResponse& res) {
        const ShellStats s = stats();
        const auto st = app_.stats(nowSeconds());
        const auto sh = render::skia::ShadowCache::instance().stats();
        render::skia::SkiaPaintTarget probe(nullptr);
        const auto rc = probe.rasterCacheStats();
        char buf[1200];
        std::snprintf(buf, sizeof(buf),
                      "{\"w\":%d,\"h\":%d,\"renderMs\":%.2f,\"encodeMs\":%.2f,"
                      "\"fps\":%.1f,\"frames\":%llu,\"lastBytes\":%zu,"
                      "\"widgets\":%d,\"clients\":%d,\"animating\":%s,"
                      "\"shadowTiles\":%zu,\"shadowHits\":%zu,\"shadowMisses\":%zu,"
                      "\"shadowKB\":%zu,"
                      "\"pageTiles\":%zu,\"pageMB\":%.1f,\"pageHits\":%zu,"
                      "\"pageMisses\":%zu,\"pageEvict\":%zu,\"pageHitRate\":%.3f,"
                      "\"imgCount\":%zu,\"imgMB\":%.1f,"
                      "\"partial\":%s,\"dmgW\":%.0f,\"dmgH\":%.0f,\"driver\":\"%s\"}",
                      width_, height_, s.renderMs, s.encodeMs,
                      s.frameMs > 0.0 ? 1000.0 / s.frameMs : 0.0,
                      static_cast<unsigned long long>(s.frames), s.lastBytes,
                      st.widgets, s.clients, st.animating ? "true" : "false",
                      sh.entries, sh.hits, sh.misses, sh.bytes / 1024,
                      rc.entries, static_cast<double>(rc.bytes) / (1024.0 * 1024.0),
                      rc.hits, rc.misses, rc.evictions,
                      (rc.hits + rc.misses) ? static_cast<double>(rc.hits) /
                                                  static_cast<double>(rc.hits + rc.misses)
                                            : 0.0,
                      render::skia::SkiaPaintTarget::imageCacheCount(),
                      static_cast<double>(
                          render::skia::SkiaPaintTarget::imageCacheBytes()) /
                          (1024.0 * 1024.0),
                      st.partial ? "true" : "false", st.damage.w, st.damage.h,
                      st.driver.c_str());
        res.contentType = "application/json";
        res.body = buf;
    });

    // MJPEG: one multipart part per frame, forever.
    http_.streamRoute("/stream", [this](int fd, std::atomic<bool>& serverRunning) {
        clients_.fetch_add(1);
        static constexpr const char* kBoundary = "folioframe";
        std::string head =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: multipart/x-mixed-replace; boundary=";
        head += kBoundary;
        head +=
            "\r\n"
            "Cache-Control: no-store, no-cache, must-revalidate\r\n"
            "Pragma: no-cache\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Connection: close\r\n\r\n";
        if (!sendAll(fd, head.data(), head.size())) {
            clients_.fetch_sub(1);
            return;
        }

        std::uint64_t seen = 0;
        while (serverRunning.load() && running_.load()) {
            std::vector<std::uint8_t> copy;
            {
                std::unique_lock<std::mutex> lk(frameMu_);
                frameCv_.wait_for(lk, std::chrono::milliseconds(500),
                                  [&] { return frameSeq_ != seen || !running_.load(); });
                if (!running_.load()) break;
                if (frameSeq_ == seen || frame_.empty()) continue;
                seen = frameSeq_;
                copy = frame_;
            }
            std::string part = "--";
            part += kBoundary;
            part += "\r\nContent-Type: image/jpeg\r\nContent-Length: " +
                    std::to_string(copy.size()) + "\r\n\r\n";
            if (!sendAll(fd, part.data(), part.size())) break;
            if (!sendAll(fd, reinterpret_cast<const char*>(copy.data()), copy.size())) break;
            if (!sendAll(fd, "\r\n", 2)) break;
        }
        clients_.fetch_sub(1);
    });
}

bool StreamShell::start(std::uint16_t port) {
    installRoutes();
    if (!http_.start(port)) return false;
    running_ = true;
    // Render one frame immediately so the first client sees something
    // without waiting for an animation to start.
    renderOnce(nowSeconds());
    return true;
}

void StreamShell::run() {
    const double frameBudget = 1.0 / kTargetFps;
    double lastPresent = nowSeconds();
    double lastAny = lastPresent;

    while (running_.load()) {
        const double t0 = nowSeconds();
        drainInput(t0);

        const bool need = app_.needsFrame(t0);
        const bool keepalive = (t0 - lastAny) > kIdleKeepaliveSec;

        if (need || keepalive) {
            renderOnce(t0);
            const double t1 = nowSeconds();
            {
                std::lock_guard<std::mutex> lk(statsMu_);
                stats_.frameMs = (t1 - lastPresent) * 1000.0;
            }
            lastPresent = t1;
            lastAny = t1;
        }

        // Sleep out the rest of the frame. When idle this still wakes 60
        // times a second to check the input queue, which costs microseconds;
        // the expensive part (paint + encode) is what is actually skipped.
        const double spent = nowSeconds() - t0;
        const double remain = frameBudget - spent;
        if (remain > 0.0) {
            std::this_thread::sleep_for(std::chrono::duration<double>(remain));
        }
    }
}

void StreamShell::stop() {
    if (!running_.exchange(false)) return;
    frameCv_.notify_all();
    http_.stop();
}

}  // namespace folio::shell
