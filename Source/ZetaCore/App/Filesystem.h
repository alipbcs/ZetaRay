#pragma once

#include "../Utility/Span.h"
#include "../Support/MemoryArena.h"

namespace ZetaRay::App::Filesystem
{
    // using a templated function for the allocator would be the obvious choice here, but unfortunately
    // that requires moving the implementation to the header file and exposing Windows.h to the whole codebase
    void LoadFromFile(const char* path, Util::Vector<uint8_t, Support::SystemAllocator>& fileData);
    void LoadFromFile(const char* path, Util::Vector<uint8_t, Support::ArenaAllocator>& fileData);
    void WriteToFile(const char* path, uint8_t* data, uint32_t sizeInBytes);
    void RemoveFile(const char* path);
    bool Exists(const char* path);
    size_t GetFileSize(const char* path);
    void CreateDirectoryIfNotExists(const char* path);
    bool Copy(const char* srcPath, const char* dstPath, bool overwrite = false);
    bool IsDirectory(const char* path);
}