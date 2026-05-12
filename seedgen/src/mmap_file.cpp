#include "mmap_file.h"
#include <cstdio>

#if defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #include <Windows.h>
#else
  #include <fcntl.h>
  #include <sys/mman.h>
  #include <sys/stat.h>
  #include <unistd.h>
#endif

MmapFile::~MmapFile() { close(); }

bool MmapFile::open(const std::string& path) {
    close();

#if defined(_WIN32)
    HANDLE file = CreateFileA(path.c_str(),
                              GENERIC_READ,
                              FILE_SHARE_READ,
                              nullptr,
                              OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "MmapFile: CreateFileA failed for %s\n", path.c_str());
        return false;
    }
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(file, &sz)) {
        fprintf(stderr, "MmapFile: GetFileSizeEx failed for %s\n", path.c_str());
        CloseHandle(file);
        return false;
    }
    size_ = static_cast<size_t>(sz.QuadPart);
    if (size_ == 0) {
        // Empty file is valid; just leave data_ null.
        CloseHandle(file);
        return true;
    }
    HANDLE mapping = CreateFileMappingA(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!mapping) {
        fprintf(stderr, "MmapFile: CreateFileMappingA failed for %s\n", path.c_str());
        CloseHandle(file);
        size_ = 0;
        return false;
    }
    void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    if (!view) {
        fprintf(stderr, "MmapFile: MapViewOfFile failed for %s\n", path.c_str());
        CloseHandle(mapping);
        CloseHandle(file);
        size_ = 0;
        return false;
    }
    fileH_    = file;
    mappingH_ = mapping;
    data_     = view;
    return true;

#else
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "MmapFile: open failed for %s\n", path.c_str());
        return false;
    }
    struct stat st;
    if (fstat(fd, &st) != 0) {
        fprintf(stderr, "MmapFile: fstat failed for %s\n", path.c_str());
        ::close(fd);
        return false;
    }
    size_ = static_cast<size_t>(st.st_size);
    if (size_ == 0) {
        ::close(fd);
        return true;
    }
    void* view = mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0);
    if (view == MAP_FAILED) {
        fprintf(stderr, "MmapFile: mmap failed for %s\n", path.c_str());
        ::close(fd);
        size_ = 0;
        return false;
    }
    fd_   = fd;
    data_ = view;
    return true;
#endif
}

void MmapFile::close() {
#if defined(_WIN32)
    if (data_)     { UnmapViewOfFile(data_); data_ = nullptr; }
    if (mappingH_) { CloseHandle(static_cast<HANDLE>(mappingH_)); mappingH_ = nullptr; }
    if (fileH_)    { CloseHandle(static_cast<HANDLE>(fileH_));    fileH_    = nullptr; }
#else
    if (data_) {
        munmap(data_, size_);
        data_ = nullptr;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
#endif
    size_ = 0;
}
