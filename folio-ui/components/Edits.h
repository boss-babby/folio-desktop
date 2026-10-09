// Undo/redo.
//
// Folio has two kinds of edit and they need very different treatment:
//
//   Annotations live in memory, are small, and are perfectly invertible.
//       Adding one can be undone by removing it; moving one can be undone by
//       putting the old geometry back. These are cheap enough that the stack
//       can hold hundreds.
//
//   Page operations rewrite the PDF on disk. Rotate is invertible in
//       principle, but *delete is not*: once PoDoFo has written a document
//       without page 7, nothing in the result can reconstruct page 7. The
//       only honest undo is to keep the bytes. So page ops snapshot the file
//       either side of the operation and undo by restoring a snapshot.
//
// Both go on one stack, because the user has one Ctrl+Z and does not care
// which subsystem owns their last action.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "components/Annotations.h"

namespace folio::ui {

class Command {
public:
    virtual ~Command() = default;
    virtual void undo() = 0;
    virtual void redo() = 0;
    /// Shown in the toast and (eventually) the Edit menu: "Undo <label>".
    [[nodiscard]] virtual std::string label() const = 0;
    /// Rough heap/disk cost, used to bound the stack.
    [[nodiscard]] virtual std::size_t weight() const { return 1; }
};

class UndoStack {
public:
    /// Push an already-performed command. Clears the redo branch, which is
    /// what every editor does: once you act after undoing, the branch you
    /// undid is gone.
    void push(std::unique_ptr<Command> c);

    bool undo();
    bool redo();

    [[nodiscard]] bool canUndo() const noexcept { return !done_.empty(); }
    [[nodiscard]] bool canRedo() const noexcept { return !undone_.empty(); }
    [[nodiscard]] std::string undoLabel() const {
        return done_.empty() ? std::string{} : done_.back()->label();
    }
    [[nodiscard]] std::string redoLabel() const {
        return undone_.empty() ? std::string{} : undone_.back()->label();
    }
    [[nodiscard]] std::size_t depth() const noexcept { return done_.size(); }
    void clear() {
        done_.clear();
        undone_.clear();
    }

private:
    void trim();
    std::vector<std::unique_ptr<Command>> done_, undone_;
    // Page-op snapshots dominate: a 153-page PDF is a few megabytes a step.
    static constexpr std::size_t kMaxWeight = 384;  // ~384 MB worst case
    static constexpr std::size_t kMaxDepth = 200;
};

// ---------------------------------------------------------------------------
// Annotation commands
// ---------------------------------------------------------------------------

/// An annotation was added. Undo removes it; redo puts it back *with the same
/// id*, so any later command that refers to that id still resolves.
class AddAnnotCommand final : public Command {
public:
    AddAnnotCommand(AnnotationStore& s, Annotation a)
        : store_(s), annot_(std::move(a)) {}
    void undo() override { store_.removeById(annot_.id); }
    void redo() override { store_.addExisting(annot_); }
    [[nodiscard]] std::string label() const override;

private:
    AnnotationStore& store_;
    Annotation annot_;
};

class RemoveAnnotCommand final : public Command {
public:
    RemoveAnnotCommand(AnnotationStore& s, Annotation a)
        : store_(s), annot_(std::move(a)) {}
    void undo() override { store_.addExisting(annot_); }
    void redo() override { store_.removeById(annot_.id); }
    [[nodiscard]] std::string label() const override;

private:
    AnnotationStore& store_;
    Annotation annot_;
};

/// Geometry or text changed on an existing annotation.
class ModifyAnnotCommand final : public Command {
public:
    ModifyAnnotCommand(AnnotationStore& s, Annotation before, Annotation after,
                       std::string what)
        : store_(s), before_(std::move(before)), after_(std::move(after)),
          what_(std::move(what)) {}
    void undo() override { store_.replace(before_.id, before_); }
    void redo() override { store_.replace(after_.id, after_); }
    [[nodiscard]] std::string label() const override { return what_; }

private:
    AnnotationStore& store_;
    Annotation before_, after_;
    std::string what_;
};

/// Several annotations changed as one user action. A multi-line highlight
/// is N annotations but one object to the person who drew it, so recolouring
/// it must be one entry on the undo stack, not one per line.
class ModifyManyCommand final : public Command {
public:
    ModifyManyCommand(AnnotationStore& s, std::vector<Annotation> before,
                      std::vector<Annotation> after, std::string what)
        : store_(s), before_(std::move(before)), after_(std::move(after)),
          what_(std::move(what)) {}
    void undo() override {
        for (const Annotation& a : before_) store_.replace(a.id, a);
    }
    void redo() override {
        for (const Annotation& a : after_) store_.replace(a.id, a);
    }
    [[nodiscard]] std::string label() const override { return what_; }

private:
    AnnotationStore& store_;
    std::vector<Annotation> before_, after_;
    std::string what_;
};

/// Several annotations added as one user action - the selection popup marks
/// up one annotation per line, and Ctrl+Z must take the whole markup back,
/// not peel off one line at a time.
class AddManyCommand final : public Command {
public:
    AddManyCommand(AnnotationStore& s, std::vector<Annotation> a, std::string what)
        : store_(s), annots_(std::move(a)), what_(std::move(what)) {}
    void undo() override {
        for (const auto& a : annots_) store_.removeById(a.id);
    }
    void redo() override {
        for (const auto& a : annots_) store_.addExisting(a);
    }
    [[nodiscard]] std::string label() const override { return what_; }

private:
    AnnotationStore& store_;
    std::vector<Annotation> annots_;
    std::string what_;
};

// ---------------------------------------------------------------------------
// Page operations
// ---------------------------------------------------------------------------

/// Undo by restoring bytes. Takes ownership of two temp files and deletes
/// them when it falls off the stack.
///
/// `reload` is called after either direction so the UI re-opens the document;
/// the page source holds an open handle and caches rasters, so swapping the
/// file underneath it without a reload shows stale pages.
class FileSnapshotCommand final : public Command {
public:
    FileSnapshotCommand(std::string what, std::string livePath,
                        std::filesystem::path before, std::filesystem::path after,
                        std::function<void(int)> reload, int pageAfter);
    ~FileSnapshotCommand() override;

    FileSnapshotCommand(const FileSnapshotCommand&) = delete;
    FileSnapshotCommand& operator=(const FileSnapshotCommand&) = delete;

    void undo() override;
    void redo() override;
    [[nodiscard]] std::string label() const override { return what_; }
    [[nodiscard]] std::size_t weight() const override { return weight_; }

    /// Copy `from` over `to` atomically (temp + rename), because a crash
    /// mid-restore must not leave a half-written PDF where the user's
    /// document was.
    static bool restore(const std::filesystem::path& from,
                        const std::string& to);

private:
    std::string what_, live_;
    std::filesystem::path before_, after_;
    std::function<void(int)> reload_;
    int pageBefore_ = 1, pageAfter_ = 1;
    std::size_t weight_ = 1;
};

/// Snapshot `path` into the session's undo scratch directory. Returns an
/// empty path on failure.
std::filesystem::path snapshotFile(const std::string& path);

}  // namespace folio::ui
