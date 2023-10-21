#pragma once

#include "../Utility/Span.h"
#include "../Support/MemoryArena.h"
#include "App.h"

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

    struct Path
    {
        Path() = default;
        explicit Path(Util::StrView str);

        Path(const Path&) = delete;
        Path& operator=(const Path&) = delete;

        ZetaInline bool IsEmpty() const { return m_path.empty(); }
        void Reset(Util::StrView str);
        ZetaInline void Resize(size_t n) { m_path.resize(n); };
        Filesystem::Path& Append(Util::StrView str);
        Filesystem::Path& ToParent();
        Filesystem::Path& Directory();
        void Stem(Util::Span<char> buff, size_t* outStrLen = nullptr) const;
        void Extension(Util::Span<char> buff, size_t* outStrLen = nullptr) const;
        ZetaInline char* Get() { return m_path.begin(); }
        ZetaInline Util::StrView GetView() const { return Util::StrView(m_path.begin(), m_path.size()); }
        ZetaInline size_t Length() const { return m_path.size(); }
        void ConvertToBackslashes();
        void ConvertToForwardSlashes();

    private:
        static constexpr size_t DEFAULT_PATH_LENGTH = 260;
        Util::SmallVector<char, Support::SystemAllocator, DEFAULT_PATH_LENGTH> m_path;
    };
}