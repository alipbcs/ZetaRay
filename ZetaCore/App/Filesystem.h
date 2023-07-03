#pragma once

#include "../Utility/Span.h"
#include "App.h"

namespace ZetaRay::Support
{
    struct ArenaAllocator;
}

namespace ZetaRay::App::Filesystem
{
    // using a templated function for the allocator would be the obvious choice here, but unfortunately
    // that requires moving the implementation to the header file and exposing Windows.h to the whole codebase
    void LoadFromFile(const char* path, Util::Vector<uint8_t, Support::SystemAllocator>& fileData) noexcept;
    void LoadFromFile(const char* path, Util::Vector<uint8_t, Support::ArenaAllocator>& fileData) noexcept;
    void WriteToFile(const char* path, uint8_t* data, uint32_t sizeInBytes) noexcept;
    void RemoveFile(const char* path) noexcept;
    bool Exists(const char* path) noexcept;
    size_t GetFileSize(const char* path) noexcept;
    void CreateDirectoryIfNotExists(const char* path) noexcept;
    bool Copy(const char* srcPath, const char* dstPath, bool overwrite = false) noexcept;
    bool IsDirectory(const char* path) noexcept;

    struct Path
    {
        Path() noexcept = default;
        explicit Path(Util::StrView str) noexcept;

        Path(const Path&) = delete;
        Path& operator=(const Path&) = delete;

        ZetaInline bool IsEmpty() const noexcept { return m_path.empty(); }
        void Reset(Util::StrView str) noexcept;
        ZetaInline void Resize(size_t n) noexcept { m_path.resize(n); };
        Filesystem::Path& Append(Util::StrView str) noexcept;
        Filesystem::Path& ToParent() noexcept;
        Filesystem::Path& Directory() noexcept;
        void Stem(Util::Span<char> buff, size_t* outStrLen = nullptr) const noexcept;
        void Extension(Util::Span<char> buff, size_t* outStrLen = nullptr) const noexcept;
        ZetaInline char* Get() noexcept { return m_path.begin(); }
        ZetaInline Util::StrView GetView() const noexcept { return Util::StrView(m_path.begin(), m_path.size()); }
        ZetaInline size_t Length() const noexcept { return m_path.size(); }
        void ConvertToBackslashes() noexcept;
        void ConvertToForwardSlashes() noexcept;

    private:
        static constexpr size_t DEFAULT_PATH_LENGTH = 260;
        Util::SmallVector<char, Support::SystemAllocator, DEFAULT_PATH_LENGTH> m_path;
    };
}