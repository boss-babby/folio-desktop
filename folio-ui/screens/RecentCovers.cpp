#include "screens/RecentCovers.h"

#if FOLIO_HAVE_PDFIUM_SOURCE
#include "pdfium/PdfiumRasterizer.h"
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace folio::ui {
namespace {
namespace fs = std::filesystem;
constexpr int kMaxW = 256, kMaxH = 384;
constexpr std::uintmax_t kDiskBudget = 8u * 1024u * 1024u;
constexpr std::array<char, 8> kMagic{'F','O','L','C','O','V','2','\0'};

fs::path utf8Path(const std::string& s) {
    return fs::path(std::u8string(s.begin(), s.end()));
}

std::uint64_t identity(const std::string& path) {
    std::error_code ec;
    fs::path file = utf8Path(path);
    if (!fs::is_regular_file(file, ec) || ec) {
        // Stable locator for a provider-backed PDF. Identity follows the
        // immutable expected fingerprint, not an ephemeral /proc/self/fd/N.
        file = utf8Path(path + ".folio-link");
        if (!fs::is_regular_file(file, ec) || ec) return 0;
    }
    const auto size = fs::file_size(file, ec);
    if (ec) return 0;
    const auto timestamp = fs::last_write_time(file, ec);
    if (ec) return 0;
    // FNV-1a over the UTF-8 native path and the file's size/mtime. Unlike
    // std::hash this key is stable across launches; editing a PDF creates a
    // different cover, while a mere reordering of recents does not.
    std::uint64_t h = 1469598103934665603ull;
    for (unsigned char ch : path) { h ^= ch; h *= 1099511628211ull; }
    const auto mix = [&h](std::uint64_t value) {
        for (int i = 0; i < 8; ++i) {
            h ^= (value >> (i * 8)) & 255u;
            h *= 1099511628211ull;
        }
    };
    mix(size);
    mix(static_cast<std::uint64_t>(timestamp.time_since_epoch().count()));
    return h ? h : 1;
}

fs::path coverFile(const fs::path& directory, std::uint64_t key) {
    std::ostringstream name;
    name << std::hex << std::setw(16) << std::setfill('0') << key << ".bgra";
    return directory / name.str();
}

std::shared_ptr<const RecentCover> fromDisk(const fs::path& file,
                                              std::uint64_t key) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    std::array<char, 8> magic{};
    std::uint32_t w = 0, h = 0;
    in.read(magic.data(), magic.size());
    in.read(reinterpret_cast<char*>(&w), sizeof(w));
    in.read(reinterpret_cast<char*>(&h), sizeof(h));
    if (!in || magic != kMagic || !w || !h || w > kMaxW || h > kMaxH) return {};
    auto cover = std::make_shared<RecentCover>();
    cover->width = static_cast<int>(w);
    cover->height = static_cast<int>(h);
    cover->imageKey = key ^ 0xC0E750F108592D31ull;
    if (!cover->imageKey) cover->imageKey = 1;
    cover->pixels.resize(static_cast<std::size_t>(w) * h * 4u);
    in.read(reinterpret_cast<char*>(cover->pixels.data()),
            static_cast<std::streamsize>(cover->pixels.size()));
    if (!in || in.peek() != std::char_traits<char>::eof()) return {};
    return cover;
}

void saveDisk(const fs::path& file, const RecentCover& cover) {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    if (ec) return;
    fs::path tmp = file;
    tmp += ".tmp"; // only one cover writer per app
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return;
        const std::uint32_t w = static_cast<std::uint32_t>(cover.width);
        const std::uint32_t h = static_cast<std::uint32_t>(cover.height);
        out.write(kMagic.data(), kMagic.size());
        out.write(reinterpret_cast<const char*>(&w), sizeof(w));
        out.write(reinterpret_cast<const char*>(&h), sizeof(h));
        out.write(reinterpret_cast<const char*>(cover.pixels.data()),
                  static_cast<std::streamsize>(cover.pixels.size()));
        out.flush();
        if (!out) { out.close(); fs::remove(tmp, ec); return; }
    }
    fs::rename(tmp, file, ec);
    if (ec) {
        ec.clear();
        fs::copy_file(tmp, file, fs::copy_options::overwrite_existing, ec);
        fs::remove(tmp, ec);
    }
    // Changed PDFs can leave obsolete versions on disk. Keep this private
    // cache below 8 MiB without a periodic timer or indexing all user PDFs.
    std::vector<std::pair<fs::file_time_type, fs::path>> files;
    std::uintmax_t total = 0;
    for (fs::directory_iterator it(file.parent_path(), ec), end; !ec && it != end;
         it.increment(ec)) {
        if (it->path().extension() != ".bgra" || !it->is_regular_file(ec)) continue;
        total += it->file_size(ec);
        files.emplace_back(it->last_write_time(ec), it->path());
    }
    if (ec || total <= kDiskBudget) return;
    std::sort(files.begin(), files.end());
    for (const auto& [age, old] : files) {
        (void)age;
        if (total <= kDiskBudget) break;
        const auto bytes = fs::file_size(old, ec);
        fs::remove(old, ec);
        if (!ec) total -= bytes;
    }
}

std::shared_ptr<const RecentCover> loadCover(const std::string& path,
                                               const fs::path& directory) {
#if FOLIO_HAVE_PDFIUM_SOURCE
    const std::uint64_t key = identity(path);
    if (!key) return {};
    const fs::path file = directory.empty() ? fs::path{} : coverFile(directory, key);
    if (!directory.empty()) {
        if (auto hit = fromDisk(file, key)) return hit;
    }
    folio::engine::PdfiumRasterizer raster;
    if (!raster.openFile(path) || raster.pageCount() == 0) return {};
    const auto info = raster.pageInfo(folio::core::PageId{0});
    if (!std::isfinite(info.widthPt) || !std::isfinite(info.heightPt) ||
        info.widthPt <= 0.f || info.heightPt <= 0.f) return {};
    const float scale = static_cast<float>(std::min({1.0,
        static_cast<double>(kMaxW) / info.widthPt,
        static_cast<double>(kMaxH) / info.heightPt}));
    auto pixels = raster.renderFullPage(folio::core::PageId{0}, scale, 0);
    if (!pixels || pixels->width <= 0 || pixels->height <= 0 ||
        pixels->width > kMaxW || pixels->height > kMaxH) return {};
    auto cover = std::make_shared<RecentCover>();
    cover->width = pixels->width;
    cover->height = pixels->height;
    cover->imageKey = key ^ 0xC0E750F108592D31ull;
    if (!cover->imageKey) cover->imageKey = 1;
    cover->pixels = std::move(pixels->pixels);
    if (!directory.empty()) saveDisk(file, *cover);
    return cover;
#else
    (void)path; (void)directory;
    return {};
#endif
}
} // namespace

RecentCovers::RecentCovers() : worker_([this] { run(); }) {}
RecentCovers::~RecentCovers() {
    {
        std::lock_guard lk(mu_);
        stopping_ = true;
        wake_ = {};
        queue_.clear();
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void RecentCovers::setStateFile(const std::string& recentsFile) {
    std::lock_guard lk(mu_);
    directory_ = recentsFile.empty() ? fs::path{} :
        utf8Path(recentsFile).parent_path() / "covers";
    ++generation_;
    queue_.clear();
    pending_.clear();
    results_.clear();
    resultsWaiting_.store(false, std::memory_order_release);
    ready_.clear();
}

void RecentCovers::setWake(std::function<void()> wake) {
    std::lock_guard lk(mu_);
    wake_ = std::move(wake);
}

void RecentCovers::request(const std::string& pdfPath) {
    if (pdfPath.empty() || ready_.count(pdfPath)) return;
    {
        std::lock_guard lk(mu_);
        if (!pending_.insert(pdfPath).second) return;
        queue_.push_back(pdfPath);
    }
    cv_.notify_one();
}

void RecentCovers::cancelQueued() {
    std::lock_guard lk(mu_);
    for (const auto& path : queue_) pending_.erase(path);
    queue_.clear();
}

void RecentCovers::keepMemoryFor(
    const std::unordered_set<std::string>& visible) {
    // UI thread. Remove offscreen cards from the CPU cache; their tiny disk
    // covers remain available when the user scrolls back.
    for (auto it = ready_.begin(); it != ready_.end();)
        it = visible.count(it->first) ? std::next(it) : ready_.erase(it);
    std::lock_guard lk(mu_);
    for (auto it = queue_.begin(); it != queue_.end();)
        if (!visible.count(*it)) {
            pending_.erase(*it);
            it = queue_.erase(it);
        } else ++it;
    for (auto it = pending_.begin(); it != pending_.end();)
        it = visible.count(*it) ? std::next(it) : pending_.erase(it);
}

void RecentCovers::releaseMemory() {
    std::lock_guard lk(mu_);
    ++generation_;
    queue_.clear();
    pending_.clear();
    results_.clear();
    resultsWaiting_.store(false, std::memory_order_release);
    ready_.clear(); // UI thread, after cards release their shared references
}

bool RecentCovers::busy() const {
    std::lock_guard lk(mu_);
    return !pending_.empty() || !results_.empty();
}

bool RecentCovers::drain() {
    std::vector<std::pair<std::string, std::shared_ptr<const RecentCover>>> results;
    {
        std::lock_guard lk(mu_);
        results.swap(results_);
        resultsWaiting_.store(false, std::memory_order_release);
    }
    for (auto& [path, cover] : results)
        ready_[path] = std::move(cover); // nullptr remembers invalid PDFs
    return !results.empty();
}

std::shared_ptr<const RecentCover> RecentCovers::get(const std::string& path) const {
    auto it = ready_.find(path);
    return it == ready_.end() ? nullptr : it->second;
}

void RecentCovers::run() {
    for (;;) {
        std::string path;
        fs::path directory;
        std::uint64_t generation;
        {
            std::unique_lock lk(mu_);
            cv_.wait(lk, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_) return;
            path = std::move(queue_.front());
            queue_.pop_front();
            directory = directory_;
            generation = generation_;
        }
        auto cover = loadCover(path, directory);
        std::function<void()> wake;
        {
            std::lock_guard lk(mu_);
            if (generation != generation_ || !pending_.erase(path)) continue;
            results_.emplace_back(path, std::move(cover));
            resultsWaiting_.store(true, std::memory_order_release);
            wake = wake_;
        }
        if (wake) wake(); // only a thread-safe platform wake, never UI work
    }
}

} // namespace folio::ui
