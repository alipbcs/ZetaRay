#pragma once

#include "../Utility/Span.h"
#include "App.h"

namespace ZetaRay::Win32::Filesystem
{
    void LoadFromFile(const char* filePath, Util::Vector<uint8_t, App::PoolAllocator>& fileData) noexcept;
    void WriteToFile(const char* filePath, uint8_t* data, uint32_t sizeInBytes) noexcept;
    void RemoveFile(const char* filePath) noexcept;
    bool Exists(const char* filePath) noexcept;
    size_t GetFileSize(const char* filePath) noexcept;

    struct Path
    {
        Path() noexcept = default;
        explicit Path(const char* p) noexcept;

        bool IsEmpty() const noexcept { return m_path.empty(); }
        void Reset(const char* p = nullptr) noexcept;
        Filesystem::Path& Append(const char* p) noexcept;
        Filesystem::Path& ToParent() noexcept;
        void Stem(Util::Span<char> buff) const noexcept;
        const char* Get() const noexcept;

    private:
        static constexpr size_t DEFAULT_PATH_LENGTH = 256;
        Util::SmallVector<char, App::PoolAllocator, DEFAULT_PATH_LENGTH> m_path;
    };
}