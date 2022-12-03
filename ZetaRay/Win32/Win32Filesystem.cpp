#include "../App/Filesystem.h"
#include "../Utility/Error.h"
#include "Win32.h"

using namespace ZetaRay::Util;
using namespace ZetaRay::App;

//--------------------------------------------------------------------------------------
// Path
//--------------------------------------------------------------------------------------

Filesystem::Path::Path(const char* p) noexcept
{
    const size_t n = Math::min(strlen(p), DEFAULT_PATH_LENGTH);
    m_path.resize(n + 1);
    memcpy(m_path.data(), p, n);
    m_path[n] = '\0';
}

void Filesystem::Path::Reset(const char* p) noexcept
{
    m_path.free_memory();

    if (p)
    {
        const size_t n = Math::min(strlen(p), DEFAULT_PATH_LENGTH);
        m_path.resize(n + 1);
        memcpy(m_path.data(), p, n);
        m_path[n] = '\0';
    }
}

Filesystem::Path& Filesystem::Path::Append(const char* pa) noexcept
{
    if (!pa)
        return *this;

    char toAppend[MAX_PATH];

    {
        const size_t len = strlen(pa);
        toAppend[0] = '\\';
        memcpy(toAppend + 1, pa, len);
        toAppend[1 + len] = '\0';
    }

    const size_t addition = strlen(toAppend);

    const size_t curr = m_path.size();
    m_path.resize(curr + addition);      // '\0' is already included in curr
    Assert(m_path[curr - 1] == '\0', "bug");

    memcpy(m_path.begin() + curr - 1, toAppend, addition);
    m_path[curr + addition - 1] = '\0';
    
    return *this;
}

Filesystem::Path& Filesystem::Path::ToParent() noexcept
{
    size_t len = m_path.size();

    char* beg = m_path.begin();
    char* curr = beg + len;
    while (curr >= beg && *curr != '\\')
        curr--;

    *curr = '\0';

    return *this;
}

void Filesystem::Path::Stem(Span<char> buff) const noexcept
{
    size_t len = m_path.size();

    char* beg = const_cast<char*>(m_path.begin());
    char* curr = beg + len;

    size_t start = -1;
    size_t end = -1;

    while (curr >= beg && *curr != '.')
        curr--;

    end = curr - beg;

    while (curr >= beg && *curr != '\\')
        curr--;

    start = curr - beg + 1;

    size_t s = end - start + 1;
    Check(buff.size() >= s, "provided buffer is too small");

    memcpy(buff.data(), beg + start, s - 1);
    buff.data()[s - 1] = '\0';
}

const char* Filesystem::Path::Get() const noexcept
{
    return m_path.begin();
}

//--------------------------------------------------------------------------------------
// Functions
//--------------------------------------------------------------------------------------

void Filesystem::LoadFromFile(const char* filePath, Vector<uint8_t, App::PoolAllocator>& fileData) noexcept
{
    Assert(filePath, "filePath was NULL");

    HANDLE h = CreateFileA(filePath,
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);

    Check(h, "CreateFile() for path %s failed with following error code: %d", filePath, GetLastError());

    LARGE_INTEGER s;
    bool success = GetFileSizeEx(h, &s);
    Check(success, "GetFileSizeEx() for path %s failed with following error code: %d", filePath, GetLastError());

    fileData.resize(s.QuadPart);
    DWORD numRead;
    success = ReadFile(h, fileData.data(), (DWORD)s.QuadPart, &numRead, nullptr);

    Check(success, "ReadFile() for path %s failed with following error code: %d", filePath, GetLastError());
    Check(numRead == (DWORD)s.QuadPart,
        "ReadFile(): read %u bytes, requested size: %u", numRead, (DWORD)s.QuadPart);

    CloseHandle(h);
}

void Filesystem::WriteToFile(const char* filePath, uint8_t* data, uint32_t sizeInBytes) noexcept
{
    Assert(filePath, "filePath was NULL");

    HANDLE h = CreateFileA(filePath,
        GENERIC_WRITE,
        FILE_SHARE_WRITE,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (h == INVALID_HANDLE_VALUE)
    {
        auto e = GetLastError();

        // overwrite is fine
        Check(e == ERROR_ALREADY_EXISTS, "CreateFile() for path %s failed with following error code: %d", filePath, e);
    }

    DWORD numWritten;
    bool success = WriteFile(h, data, sizeInBytes, &numWritten, nullptr);

    Check(success, "WriteFile() for path %s failed with following error code: %d", filePath, GetLastError());
    Check(numWritten == (DWORD)sizeInBytes,
        "WriteFile(): wrote %u bytes, requested size: %llu", numWritten, sizeInBytes);

    CloseHandle(h);
}

void Filesystem::RemoveFile(const char* filePath) noexcept
{
    Assert(filePath, "filePath was NULL");

    bool success = DeleteFileA(filePath);
    Check(success, "DeleteFile() for path %s failed with following error code: %d", filePath, GetLastError());
}

bool Filesystem::Exists(const char* filePath) noexcept
{
    Assert(filePath, "filePath was NULL");

    WIN32_FIND_DATAA findData;
    HANDLE h = FindFirstFileA(filePath, &findData);

    if (h == INVALID_HANDLE_VALUE)
    {
        DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND)
            return false;

        Check(false, "FindFirstFile() for path %s failed with following error code: %d", filePath, e);
    }

    FindClose(h);

    return true;
}

size_t Filesystem::GetFileSize(const char* filePath) noexcept
{
    Assert(filePath, "filePath was NULL");

    HANDLE h = CreateFileA(filePath,
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);

    if (h == INVALID_HANDLE_VALUE)
    {
        auto e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND)
        {
            CloseHandle(h);
            return -1;
        }

        Check(false, "CreateFile() for path %s failed with following error code: %d", filePath, e);
    }

    LARGE_INTEGER s;
    CheckWin32(GetFileSizeEx(h, &s));

    CloseHandle(h);

    return s.QuadPart;
}


