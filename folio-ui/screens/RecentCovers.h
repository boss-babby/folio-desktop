// Sequential, bounded first-page thumbnails for the library. The UI never
// parses a PDF or touches the filesystem from its paint path.
#pragma once

#include "components/RecentCover.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace folio::ui {

class RecentCovers {
public:
    RecentCovers();
    ~RecentCovers();
    RecentCovers(const RecentCovers&) = delete;
    RecentCovers& operator=(const RecentCovers&) = delete;

    void setStateFile(const std::string& recentsFile);
    void setWake(std::function<void()> wake);
    void request(const std::string& pdfPath); // main thread; visible cards only
    void cancelQueued();                      // leaving the home screen
    void keepMemoryFor(const std::unordered_set<std::string>& visible);
    void releaseMemory(); // minimise: disk files survive, pixels do not
    [[nodiscard]] bool hasResults() const noexcept {
        return resultsWaiting_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool busy() const;
    bool drain(); // main thread; true if a card can now be repainted
    [[nodiscard]] std::shared_ptr<const RecentCover> get(const std::string& path) const;

private:
    void run();
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::thread worker_;
    bool stopping_ = false;
    std::uint64_t generation_ = 0; // cancel in-flight cover on minimise/reset
    std::filesystem::path directory_; // alongside recents, never in the PDF's directory
    std::function<void()> wake_;
    std::deque<std::string> queue_;
    std::unordered_set<std::string> pending_;
    std::vector<std::pair<std::string, std::shared_ptr<const RecentCover>>> results_;
    std::atomic<bool> resultsWaiting_{false};
    std::map<std::string, std::shared_ptr<const RecentCover>> ready_; // UI only
};

} // namespace folio::ui
