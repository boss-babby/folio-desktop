// Read-only, demand-paged PDFium input. Unlike copying every PDF into a
// DocumentSnapshot vector, a large file does not immediately consume its
// entire size in private RSS. The OS faults only the ranges PDFium reads.
// Lives on the PDFium worker thread and must outlive its FPDF_DOCUMENT.
#pragma once

#include <fpdfview.h>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <functional>
#include <mutex>
#include <limits>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace folio::engine {
// A host-provided durable locator may supply a fresh seekable descriptor on
// every open. The returned descriptor belongs to FileMappedAccess; it is never
// persisted, and is closed with the PDFium document on its worker thread.
using LinkedPdfOpener = std::function<int(const std::string&)>;
inline LinkedPdfOpener& linkedPdfOpener() { static LinkedPdfOpener fn; return fn; }
inline void setLinkedPdfOpener(LinkedPdfOpener fn) { linkedPdfOpener() = std::move(fn); }
class FileMappedAccess {
public:
    explicit FileMappedAccess(const std::string& utf8Path) {
#if defined(_WIN32)
        const std::filesystem::path path = std::filesystem::path(std::u8string(utf8Path.begin(), utf8Path.end()));
        file_ = CreateFileW(path.c_str(), GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file_ == INVALID_HANDLE_VALUE) return;
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file_, &size) || size.QuadPart <= 0 ||
            static_cast<unsigned long long>(size.QuadPart) >
                std::numeric_limits<unsigned long>::max()) return;
        length_ = static_cast<std::size_t>(size.QuadPart);
        mapping_ = CreateFileMappingW(file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (mapping_) data_ = MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, 0);
#else
        file_ = ::open(utf8Path.c_str(), O_RDONLY);
        // Only the Android bridge knows how to resolve this private stable
        // locator. Ordinary missing paths never ask the document provider.
        if (file_ < 0 && linkedPdfOpener() &&
            std::filesystem::exists(std::filesystem::path(utf8Path + ".folio-link")))
            file_ = linkedPdfOpener()(utf8Path);
        if (file_ < 0) return;
        mapFd();
#endif
#if defined(_WIN32)
        if (!data_) return;
        access_.m_FileLen = static_cast<unsigned long>(length_);
        access_.m_GetBlock = &FileMappedAccess::getBlock;
        access_.m_Param = this;
#endif
    }
#if !defined(_WIN32)
    explicit FileMappedAccess(int ownedFd) : file_(ownedFd) { mapFd(); }
#endif

private:
#if !defined(_WIN32)
    void mapFd() {
        struct stat st{};
        if (::fstat(file_, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
            static_cast<unsigned long long>(st.st_size) >
                std::numeric_limits<unsigned long>::max()) return;
        length_ = static_cast<std::size_t>(st.st_size);
        void* view = ::mmap(nullptr, length_, PROT_READ, MAP_PRIVATE, file_, 0);
        if (view != MAP_FAILED) data_ = view;
        if (!data_) return;
        access_.m_FileLen = static_cast<unsigned long>(length_);
        access_.m_GetBlock = &FileMappedAccess::getBlock;
        access_.m_Param = this;
    }
#else
    void mapFd() {}
#endif
public:
    ~FileMappedAccess() {
#if defined(_WIN32)
        if (data_) UnmapViewOfFile(data_);
        if (mapping_) CloseHandle(mapping_);
        if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
#else
        if (data_) ::munmap(const_cast<void*>(data_), length_);
        if (file_ >= 0) ::close(file_);
#endif
    }
    FileMappedAccess(const FileMappedAccess&) = delete;
    FileMappedAccess& operator=(const FileMappedAccess&) = delete;
    [[nodiscard]] bool valid() const noexcept { return data_ != nullptr; }
    [[nodiscard]] FPDF_FILEACCESS* handle() noexcept { return &access_; }

private:
    static int getBlock(void* param, unsigned long pos,
                        unsigned char* buf, unsigned long size) {
        auto* self = static_cast<FileMappedAccess*>(param);
        if (!self->data_ || pos > self->length_ ||
            size > self->length_ - pos) return 0;
        std::memcpy(buf, static_cast<const unsigned char*>(self->data_) + pos, size);
        return 1;
    }
#if defined(_WIN32)
    HANDLE file_ = INVALID_HANDLE_VALUE;
    HANDLE mapping_ = nullptr;
#else
    int file_ = -1;
#endif
    const void* data_ = nullptr;
    std::size_t length_ = 0;
    FPDF_FILEACCESS access_{};
};
} // namespace folio::engine
