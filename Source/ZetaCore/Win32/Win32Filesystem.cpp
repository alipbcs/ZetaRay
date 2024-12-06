#include "../App/Filesystem.h"
#include "../Support/MemoryArena.h"
#include "Win32.h"

using namespace ZetaRay::Util;
using namespace ZetaRay::App;

//--------------------------------------------------------------------------------------
// Functions
//--------------------------------------------------------------------------------------

void Filesystem::LoadFromFile(const char* path, Vector<uint8_t>& fileData)
{
    Assert(path, "path argument was NULL.");

    HANDLE h = CreateFileA(path,
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);

    Check(h, "CreateFile() for path %s failed with the following error code: %d.", path, 
        GetLastError());

    LARGE_INTEGER s;
    bool success = GetFileSizeEx(h, &s);
    Check(success, "GetFileSizeEx() for path %s failed with the following error code: %d.", 
        path, GetLastError());

    fileData.resize(s.QuadPart);
    DWORD numRead;
    success = ReadFile(h, fileData.data(), (DWORD)s.QuadPart, &numRead, nullptr);

    Check(success, "ReadFile() for path %s failed with the following error code: %d.", 
        path, GetLastError());
    Check(numRead == (DWORD)s.QuadPart,
        "ReadFile(): read %u bytes, requested size: %u", numRead, (DWORD)s.QuadPart);

    CloseHandle(h);
}

void Filesystem::LoadFromFile(const char* path, Vector<uint8_t, Support::ArenaAllocator>& fileData)
{
    Assert(path, "path argument was NULL.");

    HANDLE h = CreateFileA(path,
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);

    Check(h, "CreateFile() for path %s failed with the following error code: %d.", 
        path, GetLastError());

    LARGE_INTEGER s;
    bool success = GetFileSizeEx(h, &s);
    Check(success, "GetFileSizeEx() for path %s failed with the following error code: %d.", 
        path, GetLastError());

    fileData.resize(s.QuadPart);
    DWORD numRead;
    success = ReadFile(h, fileData.data(), (DWORD)s.QuadPart, &numRead, nullptr);

    Check(success, "ReadFile() for path %s failed with the following error code: %d.", 
        path, GetLastError());
    Check(numRead == (DWORD)s.QuadPart,
        "ReadFile(): read %u bytes, requested size: %u.", numRead, (DWORD)s.QuadPart);

    CloseHandle(h);
}

void Filesystem::WriteToFile(const char* path, uint8_t* data, uint32_t sizeInBytes)
{
    Assert(path, "path argument was NULL.");

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
        Check(e == ERROR_ALREADY_EXISTS, 
            "CreateFile() for path %s failed with the following error code: %d", path, e);
    }

    DWORD numWritten;
    bool success = WriteFile(h, data, sizeInBytes, &numWritten, nullptr);

    Check(success, "WriteFile() for path %s failed with the following error code: %d.", 
        path, GetLastError());
    Check(numWritten == (DWORD)sizeInBytes,
        "WriteFile(): wrote %u bytes, requested size: %llu.", numWritten, sizeInBytes);

    CloseHandle(h);
}

void Filesystem::RemoveFile(const char* path)
{
    Assert(path, "path argument was NULL.");

    bool success = DeleteFileA(path);
    Check(success, "DeleteFile() for path %s failed with the following error code: %d.", 
        path, GetLastError());
}

bool Filesystem::Exists(const char* path)
{
    Assert(path, "path argument was NULL.");
    auto attrib = GetFileAttributesA(path);

    return (attrib != INVALID_FILE_ATTRIBUTES) && !(attrib & FILE_ATTRIBUTE_DIRECTORY);
}

size_t Filesystem::GetFileSize(const char* path)
{
    Assert(path, "path argument was NULL.");

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

        Check(false, "CreateFile() for path %s failed with the following error code: %d.", 
            path, e);
    }

    LARGE_INTEGER s;
    CheckWin32(GetFileSizeEx(h, &s));

    CloseHandle(h);

    return s.QuadPart;
}

void Filesystem::CreateDirectoryIfNotExists(const char* path)
{
    Assert(path, "path argument was NULL.");
    auto attrib = GetFileAttributesA(path);
    if ((attrib != INVALID_FILE_ATTRIBUTES) && (attrib & FILE_ATTRIBUTE_DIRECTORY))
        return;

    CheckWin32(CreateDirectoryA(path, nullptr));
}

bool Filesystem::Copy(const char* path, const char* newPath, bool overwrite)
{
    bool ret = CopyFileA(path, newPath, !overwrite);
    if (!ret)
    {
        auto err = GetLastError();
        Check(err == ERROR_FILE_EXISTS, "CopyFile() failed with the error code: %d\n.", err);

        return false;
    }

    return true;
}

bool Filesystem::IsDirectory(const char* path)
{
    Assert(path, "path argument was NULL.");
    auto ret = GetFileAttributesA(path);

    if (ret == INVALID_FILE_ATTRIBUTES)
    {
        auto err = GetLastError();
        auto validCodes = ERROR_FILE_NOT_FOUND;
        Check(err & validCodes, "GetFileAttributesA() failed with the error code: %d\n.", err);

        return false;
    }

    return ret & FILE_ATTRIBUTE_DIRECTORY;
}
