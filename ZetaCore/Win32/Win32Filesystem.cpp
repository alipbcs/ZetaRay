#include "../App/Filesystem.h"
#include "../Utility/Error.h"
#include "../Support/MemoryArena.h"
#include "Win32.h"

using namespace ZetaRay::Util;
using namespace ZetaRay::App;

//--------------------------------------------------------------------------------------
// Path
//--------------------------------------------------------------------------------------

Filesystem::Path::Path(Util::StrView str) noexcept
{
    const size_t n = str.size();
    m_path.resize(n + 1);   // + 1 for '\0'

    if(n)
        memcpy(m_path.data(), str.data(), n);
    
    m_path[n] = '\0';
}

void Filesystem::Path::Reset(Util::StrView str) noexcept
{
    m_path.free_memory();

    if (!str.empty())
    {
        const size_t n = str.size();
        m_path.resize(n + 1);   // + 1 for '\0'
        memcpy(m_path.data(), str.data(), n);
        m_path[n] = '\0';
    }
}

Filesystem::Path& Filesystem::Path::Append(Util::StrView str) noexcept
{
    if (str.empty())
        return *this;

    // don't read uninitialized memory
    // (Note: underlying path storages's size and the actual string length may not match)
    const size_t curr = m_path.empty() ? 0 : strlen(m_path.data()); 
    char toAppend[MAX_PATH];

    size_t additionLen = str.size();

    if (curr)
    {
        toAppend[0] = '\\';
        memcpy(toAppend + 1, str.data(), additionLen);
        additionLen++;
    }
    else
        memcpy(toAppend, str.data(), additionLen);

    const size_t newSize = curr + additionLen + 1;
    m_path.resize(newSize);

    memcpy(m_path.begin() + curr, toAppend, additionLen);
    m_path[curr + additionLen] = '\0';
    
    return *this;
}

Filesystem::Path& Filesystem::Path::ToParent() noexcept
{
    const size_t len = strlen(m_path.data());

    char* beg = m_path.begin();
    char* curr = beg + len;
    
    while (curr >= beg && *curr != '\\')
        curr--;

    if(*curr == '\\')
        *curr = '\0';
    else
    {
        m_path.resize(3);

        m_path[0] = '.';
        m_path[1] = '.';
        m_path[2] = '\0';
    }

    return *this;
}

Filesystem::Path& Filesystem::Path::Directory() noexcept
{
    if (Filesystem::IsDirectory(m_path.data()))
        return *this;

    const size_t len = strlen(m_path.data());
    char* beg = m_path.begin();
    char* curr = beg + len;

    while (curr >= beg && *curr != '\\')
        curr--;

    if (*curr == '\\')
        *curr = '\0';
    else
    {
        m_path.resize(2);
        m_path[0] = '.';
        m_path[1] = '\0';
    }

    return *this;
}

void Filesystem::Path::Stem(Span<char> buff, size_t* outStrLen) const noexcept
{
    const size_t len = strlen(m_path.begin());

    char* beg = const_cast<char*>(m_path.begin());
    char* curr = beg + len - 1;

    size_t start = size_t(-1);
    size_t end = size_t(-1);
    char* firstDot = nullptr;

    // figure out the first ., e.g. a.b.c -> a
    while (curr >= beg && *curr != '\\')
    {
        if (*curr == '.')
            firstDot = curr;

        curr--;
    }

    firstDot = firstDot ? firstDot : beg + len;
    end = firstDot - beg;
    start = curr - beg + 1;

    size_t s = end - start + 1;
    Check(buff.size() >= s, "provided buffer is too small");

    if(s > 1)
        memcpy(buff.data(), beg + start, s - 1);
    
    buff.data()[s - 1] = '\0';

    if (outStrLen)
        *outStrLen = s - 1;
}

void Filesystem::Path::Extension(Util::Span<char> buff, size_t* outStrLen) const noexcept
{
    const size_t len = strlen(m_path.begin());

    char* beg = const_cast<char*>(m_path.begin());
    char* curr = beg + len - 1;

    size_t start = size_t(-1);
    size_t end = size_t(-1);

    // figure out the first ., e.g. a.b.c -> a
    while (curr >= beg && *curr != '.')
        curr--;

    if (curr < beg)
    {
        if (outStrLen)
            *outStrLen = 0;

        return;
    }

    size_t s = beg + len - 1 - curr;
    Check(buff.size() >= s + 1, "provided buffer is too small");

    if (s > 1)
        memcpy(buff.data(), curr + 1, s);

    buff.data()[s] = '\0';

    if (outStrLen)
        *outStrLen = s;
}

//--------------------------------------------------------------------------------------
// Functions
//--------------------------------------------------------------------------------------

void Filesystem::LoadFromFile(const char* path, Vector<uint8_t>& fileData) noexcept
{
    Assert(path, "given path was NULL");

    HANDLE h = CreateFileA(path,
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);

    Check(h, "CreateFile() for path %s failed with following error code: %d", path, GetLastError());

    LARGE_INTEGER s;
    bool success = GetFileSizeEx(h, &s);
    Check(success, "GetFileSizeEx() for path %s failed with following error code: %d", path, GetLastError());

    fileData.resize(s.QuadPart);
    DWORD numRead;
    success = ReadFile(h, fileData.data(), (DWORD)s.QuadPart, &numRead, nullptr);

    Check(success, "ReadFile() for path %s failed with following error code: %d", path, GetLastError());
    Check(numRead == (DWORD)s.QuadPart,
        "ReadFile(): read %u bytes, requested size: %u", numRead, (DWORD)s.QuadPart);

    CloseHandle(h);
}

void Filesystem::LoadFromFile(const char* path, Vector<uint8_t, Support::ArenaAllocator>& fileData) noexcept
{
    Assert(path, "given path was NULL");

    HANDLE h = CreateFileA(path,
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);

    Check(h, "CreateFile() for path %s failed with following error code: %d", path, GetLastError());

    LARGE_INTEGER s;
    bool success = GetFileSizeEx(h, &s);
    Check(success, "GetFileSizeEx() for path %s failed with following error code: %d", path, GetLastError());

    fileData.resize(s.QuadPart);
    DWORD numRead;
    success = ReadFile(h, fileData.data(), (DWORD)s.QuadPart, &numRead, nullptr);

    Check(success, "ReadFile() for path %s failed with following error code: %d", path, GetLastError());
    Check(numRead == (DWORD)s.QuadPart,
        "ReadFile(): read %u bytes, requested size: %u", numRead, (DWORD)s.QuadPart);

    CloseHandle(h);
}

void Filesystem::WriteToFile(const char* path, uint8_t* data, uint32_t sizeInBytes) noexcept
{
    Assert(path, "given path was NULL");

    HANDLE h = CreateFileA(path,
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
        Check(e == ERROR_ALREADY_EXISTS, "CreateFile() for path %s failed with following error code: %d", path, e);
    }

    DWORD numWritten;
    bool success = WriteFile(h, data, sizeInBytes, &numWritten, nullptr);

    Check(success, "WriteFile() for path %s failed with following error code: %d", path, GetLastError());
    Check(numWritten == (DWORD)sizeInBytes,
        "WriteFile(): wrote %u bytes, requested size: %llu", numWritten, sizeInBytes);

    CloseHandle(h);
}

void Filesystem::RemoveFile(const char* path) noexcept
{
    Assert(path, "given path was NULL");

    bool success = DeleteFileA(path);
    Check(success, "DeleteFile() for path %s failed with following error code: %d", path, GetLastError());
}

bool Filesystem::Exists(const char* path) noexcept
{
    Assert(path, "given path was NULL");

    WIN32_FIND_DATAA findData;
    HANDLE h = FindFirstFileA(path, &findData);

    if (h == INVALID_HANDLE_VALUE)
    {
        DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND)
            return false;

        Check(false, "Unexpected error in FindFirstFile() for path %s with the following error code: %d", path, e);
    }

    FindClose(h);

    return true;
}

size_t Filesystem::GetFileSize(const char* path) noexcept
{
    Assert(path, "given path was NULL");

    HANDLE h = CreateFileA(path,
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
            return size_t(-1);
        }

        Check(false, "CreateFile() for path %s failed with following error code: %d", path, e);
    }

    LARGE_INTEGER s;
    CheckWin32(GetFileSizeEx(h, &s));

    CloseHandle(h);

    return s.QuadPart;
}

void Filesystem::CreateDirectoryIfNotExists(const char* path) noexcept
{
    if (!CreateDirectoryA(path, nullptr))
    {
        auto err = GetLastError();
        if (err != ERROR_ALREADY_EXISTS)
        {
            if (err == ERROR_PATH_NOT_FOUND)
            {
                Check(false, "Failed to create directory in path %s: \
                    One or more intermediate directories do not exist.\n", path)
            }
            else
                Check(false, "Failed to create directory in path %s\n", path);
        }
    }
}

bool Filesystem::Copy(const char* path, const char* newPath, bool overwrite) noexcept
{
    bool ret = CopyFileA(path, newPath, !overwrite);
    if (!ret)
    {
        auto err = GetLastError();
        Check(err == ERROR_FILE_EXISTS, "CopyFile() failed with error code: %d\n", err);

        return false;
    }

    return true;
}

bool Filesystem::IsDirectory(const char* path) noexcept
{
    Assert(path, "given path was NULL");

    auto ret = GetFileAttributesA(path);

    if (ret == INVALID_FILE_ATTRIBUTES)
    {
        auto err = GetLastError();
        auto validCodes = ERROR_FILE_NOT_FOUND;
        Check(err & validCodes, "GetFileAttributesA() failed with error %d\n", err);

        return false;
    }

    return ret & FILE_ATTRIBUTE_DIRECTORY;
}


