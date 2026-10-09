// folio/core/engine/IPdfDocumentStore.h
//
// The authority half of the engine facade. Implemented by PodofoDocumentStore.
//
// This header encodes the single most important invariant in Folio
// (architecture plan §5.4): the two engines must never both believe they own
// the document. Rather than rely on developer discipline, the authority state
// machine is expressed in the type system - beginEdit() simply CANNOT return
// a usable guard while PDFium owns the form-fill session.

#pragma once

#include <expected>
#include <string>
#include <vector>

#include "folio/core/engine/DocumentSnapshot.h"

namespace folio::core {

/// Who is currently allowed to mutate the document.
enum class Authority {
    PodofoOwns,      // normal state: structural edits allowed
    PdfiumFormOwns,  // bounded form-fill session: structural edits rejected
    Saving,          // transient: serialising, nothing may mutate
};

enum class EditRejected {
    FormSessionActive,   // in PdfiumFormOwns - commit or leave Forms mode
    SaveInProgress,
    DocumentEncrypted,   // no write permission
    DocumentNotLoaded,
};

enum class SaveError {
    IoFailure,
    EncryptionFailure,
    SignatureFailure,
    NotPermitted,
};

class IPdfDocumentStore;

/// RAII proof that the caller holds write authority. Non-copyable, and only
/// obtainable from beginEdit(). Every mutating API requires one by reference,
/// so it is impossible to mutate without having passed the state check.
class EditGuard {
public:
    EditGuard(EditGuard&&) noexcept;
    EditGuard& operator=(EditGuard&&) noexcept;
    EditGuard(const EditGuard&) = delete;
    EditGuard& operator=(const EditGuard&) = delete;
    ~EditGuard();

    /// Pages touched during this edit. Fed into DocumentSnapshot so only these
    /// pages lose their cached tiles.
    void markDirty(PageId page);

private:
    friend class IPdfDocumentStore;
    explicit EditGuard(IPdfDocumentStore* owner) noexcept;
    IPdfDocumentStore* owner_;
};

struct Sink {
    virtual ~Sink() = default;
    virtual bool write(std::span<const std::byte> bytes) = 0;
};

class IPdfDocumentStore {
public:
    virtual ~IPdfDocumentStore() = default;

    [[nodiscard]] virtual Authority authority() const noexcept = 0;

    /// The ONLY way to obtain write access. Returns EditRejected rather than
    /// throwing, so callers must handle the form-session case explicitly.
    [[nodiscard]] virtual std::expected<EditGuard, EditRejected> beginEdit() = 0;

    /// Current snapshot. Safe to call from any thread.
    [[nodiscard]] virtual SnapshotPtr snapshot() const = 0;

    // --- structural operations: all require proof of authority ---
    virtual void rotatePage(EditGuard&, PageId, int degrees) = 0;
    virtual void deletePages(EditGuard&, std::span<const PageId>) = 0;
    virtual void movePage(EditGuard&, PageId, std::size_t toIndex) = 0;
    virtual PageId insertBlankPage(EditGuard&, std::size_t atIndex) = 0;
    virtual PageId duplicatePage(EditGuard&, PageId) = 0;

    // --- authority handoff for the form-fill session (plan 5.4) ---
    virtual void enterFormSession() = 0;
    /// PDFium saves incrementally into a buffer; PoDoFo reloads and resumes.
    virtual std::expected<void, SaveError> commitFormSession(
        std::vector<std::byte> pdfiumIncrementalSave) = 0;

    // --- persistence ---
    virtual std::expected<void, SaveError> saveIncremental(Sink&) = 0;
    virtual std::expected<void, SaveError> saveLinearized(Sink&) = 0;
};

}  // namespace folio::core
