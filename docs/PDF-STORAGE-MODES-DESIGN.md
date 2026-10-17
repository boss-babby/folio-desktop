# PDF storage modes — implementation and safety contract

## Modes

**Keep offline copy** is the default. SAF imports retain one durable private PDF per source URI, with a stable library path and a separate Folio annotation sidecar. The compact Home Settings popover can instead prefer **Reference source, keep annotations only** for *new* imports; changing the preference does not discard existing copies. On an unsupported provider the import falls back to Keep copy.

Each Home card has a 0.5-second touch-hold or desktop right-click menu: Open, Keep offline copy, Use source only, Relink source PDF, Remove from library. Storage actions are enabled only where meaningful. Removing a card removes its recent entry, **not** the source PDF, its private copy, or its annotations. Card metadata says whether the PDF is an offline copy or source-only. A linked file whose open fails stays in the library, displays unavailable status, and offers Relink.

## Android source-only validation and recovery

`FolioActivity` persists an individual SAF read grant only when the picker offers one. A source-only switch requires an explicit warning/confirmation, a still-persisted grant, a regular seekable provider FD, successful PDFium reopen, and an exact SHA-256 match to the existing private copy. Only then is the link marker atomically committed and the private copy removed. Importing with the source-only preference performs the same validation and otherwise keeps the newly imported copy. ACTION_VIEW and streaming/nonseekable sources cannot silently become linked items.

An entry's stable library path (and thus its historical sidecar key) never becomes `/proc/self/fd/N`. A separate URI locator and SHA-256 fingerprint live in private app storage. Each page/render/text/thumbnail/export open obtains a **fresh, owned FD** from the provider; PDFium uses bounded demand-paged mapping, and the FD closes with the document worker. Missing/revoked sources cannot erase the recent or annotation sidecar. Relink uses `ACTION_OPEN_DOCUMENT`, insists on a persisted read grant and exact PDF byte identity, validates a new PDFium reopen, then atomically updates the URI. A mismatched replacement is refused rather than attaching old edits to another PDF. Keep offline copy streams the original to a temporary file, verifies the digest and PDFium reopen, syncs and atomically commits it before removing the source-only marker.

Legacy path-only recents and sidecars keep their original path. A legacy private copy is hashed before any repeat import may replace it; a changed provider document is refused instead of silently reusing old annotations.

## Verification boundary

Native regression tests cover stable FD-backed opens and Save As, missing local PDF paths retained in recents, long-press/right-click suppression of accidental Open, imported-annotation source immutability, and export/reopen. The Android Java/C++ bridge is build/lint checked, but **no Android device/emulator is available here** to simulate grant revocation, cloud-provider pipes, lifecycle restart or picker UI. Those device/provider behaviours need acceptance testing on hardware before production rollout; do not treat build success as a phone test.
