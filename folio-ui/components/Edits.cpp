#include "components/Edits.h"

#include <atomic>
#include <system_error>

#if defined(_WIN32)
#  include <process.h>
#else
#  include <unistd.h>
#endif

namespace folio::ui {

namespace fs = std::filesystem;

namespace {

const char* kindName(AnnotKind k) {
    switch (k) {
        case AnnotKind::Highlight:     return "highlight";
        case AnnotKind::Underline:     return "underline";
        case AnnotKind::Strikethrough: return "strikethrough";
        case AnnotKind::Ink:           return "drawing";
        case AnnotKind::Note:          return "note";
        case AnnotKind::Box:           return "box";
        case AnnotKind::Ellipse:       return "ellipse";
        case AnnotKind::Text:          return "text";
        case AnnotKind::Stamp:         return "stamp";
        case AnnotKind::Signature:     return "signature";
    }
    return "annotation";
}

/// One scratch directory per process, created lazily and removed at exit.
/// Keyed on pid so two Folio windows cannot tread on each other.
const fs::path& scratchDir() {
    static const fs::path dir = [] {
        std::error_code ec;
        fs::path d = fs::temp_directory_path(ec) /
                     ("folio-undo-" + std::to_string(
#if defined(_WIN32)
                                          static_cast<long>(::_getpid())
#else
                                          static_cast<long>(::getpid())
#endif
                                              ));
        fs::create_directories(d, ec);
        return d;
    }();
    return dir;
}

struct ScratchCleanup {
    ~ScratchCleanup() {
        std::error_code ec;
        fs::remove_all(scratchDir(), ec);
    }
};
ScratchCleanup gScratchCleanup;

std::atomic<std::uint64_t> gSnapshotSeq{0};

}  // namespace

std::string AddAnnotCommand::label() const {
    return std::string("add ") + kindName(annot_.kind);
}

std::string RemoveAnnotCommand::label() const {
    return std::string("delete ") + kindName(annot_.kind);
}

// ---------------------------------------------------------------------------
// UndoStack
// ---------------------------------------------------------------------------

void UndoStack::push(std::unique_ptr<Command> c) {
    if (!c) return;
    done_.push_back(std::move(c));
    undone_.clear();
    trim();
}

void UndoStack::trim() {
    std::size_t total = 0;
    for (const auto& c : done_) total += c->weight();
    // Drop from the bottom: the oldest edits are the ones the user is least
    // likely to walk back to, and page-op snapshots are what blow the budget.
    while (!done_.empty() && (total > kMaxWeight || done_.size() > kMaxDepth)) {
        total -= done_.front()->weight();
        done_.erase(done_.begin());
    }
}

bool UndoStack::undo() {
    if (done_.empty()) return false;
    auto c = std::move(done_.back());
    done_.pop_back();
    c->undo();
    undone_.push_back(std::move(c));
    return true;
}

bool UndoStack::redo() {
    if (undone_.empty()) return false;
    auto c = std::move(undone_.back());
    undone_.pop_back();
    c->redo();
    done_.push_back(std::move(c));
    return true;
}

// ---------------------------------------------------------------------------
// FileSnapshotCommand
// ---------------------------------------------------------------------------

fs::path snapshotFile(const std::string& path) {
    std::error_code ec;
    if (!fs::exists(path, ec)) return {};
    const fs::path dst =
        scratchDir() / ("snap-" + std::to_string(++gSnapshotSeq) + ".pdf");
    fs::copy_file(path, dst, fs::copy_options::overwrite_existing, ec);
    if (ec) return {};
    return dst;
}

bool FileSnapshotCommand::restore(const fs::path& from, const std::string& to) {
    std::error_code ec;
    if (from.empty() || !fs::exists(from, ec)) return false;
    // Temp + rename. Copying straight over the live path would leave the
    // user's document truncated if anything went wrong halfway.
    const fs::path tmp = fs::path(to).concat(".folio-restore");
    fs::copy_file(from, tmp, fs::copy_options::overwrite_existing, ec);
    if (ec) return false;
    fs::rename(tmp, to, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

FileSnapshotCommand::FileSnapshotCommand(std::string what, std::string livePath,
                                         fs::path before, fs::path after,
                                         std::function<void(int)> reload,
                                         int pageAfter)
    : what_(std::move(what)), live_(std::move(livePath)), before_(std::move(before)),
      after_(std::move(after)), reload_(std::move(reload)), pageAfter_(pageAfter) {
    std::error_code ec;
    std::size_t bytes = 0;
    if (!before_.empty()) bytes += fs::file_size(before_, ec);
    if (!after_.empty()) bytes += fs::file_size(after_, ec);
    weight_ = 1 + bytes / (1024 * 1024);  // in megabytes, floor 1
}

FileSnapshotCommand::~FileSnapshotCommand() {
    std::error_code ec;
    if (!before_.empty()) fs::remove(before_, ec);
    if (!after_.empty()) fs::remove(after_, ec);
}

void FileSnapshotCommand::undo() {
    if (restore(before_, live_) && reload_) reload_(pageBefore_);
}

void FileSnapshotCommand::redo() {
    if (restore(after_, live_) && reload_) reload_(pageAfter_);
}

}  // namespace folio::ui
