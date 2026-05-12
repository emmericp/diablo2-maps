#pragma once
#include <cstddef>
#include <string>

// Read-only memory-mapped file. Cross-platform — POSIX mmap() on
// Linux/macOS, CreateFileMapping/MapViewOfFile on Windows. Both branches
// expose the file's bytes through a single pointer.
//
// Caller owns lifetime: `open()` then read via `data()` until destruction
// (or `close()`). Concurrent reads of disjoint ranges are safe; mutation
// is not — this is strictly a read-only view.
class MmapFile {
public:
    MmapFile() = default;
    ~MmapFile();
    MmapFile(const MmapFile&) = delete;
    MmapFile& operator=(const MmapFile&) = delete;

    // Map the entire file read-only. Returns false on any failure (open,
    // size query, or mapping). Errors are reported to stderr.
    bool open(const std::string& path);
    void close();

    bool        valid() const { return data_ != nullptr; }
    const void* data()  const { return data_; }
    size_t      size()  const { return size_; }

    // Convenience: pointer arithmetic on a byte view.
    const unsigned char* bytes() const {
        return static_cast<const unsigned char*>(data_);
    }

private:
    void*  data_ = nullptr;
    size_t size_ = 0;
#if defined(_WIN32)
    void*  fileH_    = nullptr;   // HANDLE
    void*  mappingH_ = nullptr;   // HANDLE
#else
    int    fd_ = -1;
#endif
};
