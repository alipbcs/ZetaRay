#include "SharedShaderResources.h"
#include <xxHash/xxhash.h>

using namespace ZetaRay;
using namespace ZetaRay::Core;
using namespace ZetaRay::Core::GpuMemory;

//--------------------------------------------------------------------------------------
// SharedShaderResources
//--------------------------------------------------------------------------------------

const UploadHeapBuffer* SharedShaderResources::GetUploadHeapBuffer(uint64_t id)
{
    std::shared_lock<std::shared_mutex> lock(m_uploadHeapMtx);
    auto buff = m_uploadHeapBuffs.find(id);
    if(buff)
        return *buff.value();

    return nullptr;
}

const UploadHeapBuffer* SharedShaderResources::GetUploadHeapBuffer(std::string_view id)
{
    uint64_t h = XXH3_64bits(id.data(), id.size());
    return GetUploadHeapBuffer(h);
}

void SharedShaderResources::InsertOrAssignUploadHeapBuffer(std::string_view id, UploadHeapBuffer& buf)
{
    uint64_t h = XXH3_64bits(id.data(), id.size());
    InsertOrAssignUploadHeapBuffer(h, buf);
}

void SharedShaderResources::InsertOrAssignUploadHeapBuffer(uint64_t id, const UploadHeapBuffer& buf)
{
    std::unique_lock<std::shared_mutex> lock(m_uploadHeapMtx);
    m_uploadHeapBuffs[id] = &buf;
}

const Buffer* SharedShaderResources::GetDefaultHeapBuffer(uint64_t id)
{
    std::shared_lock<std::shared_mutex> lock(m_defaultHeapMtx);
    auto it = m_defaultHeapBuffs.find(id);
    if (!it)
        return nullptr;

    return *it.value();
}

const Buffer* SharedShaderResources::GetDefaultHeapBuffer(std::string_view id)
{
    uint64_t h = XXH3_64bits(id.data(), id.size());
    return GetDefaultHeapBuffer(h);
}

void SharedShaderResources::InsertOrAssignDefaultHeapBuffer(uint64_t id, const Buffer& buf)
{
    std::unique_lock<std::shared_mutex> lock(m_defaultHeapMtx);
    m_defaultHeapBuffs[id] = &buf;
}

void SharedShaderResources::InsertOrAssignDefaultHeapBuffer(std::string_view id, const Buffer& buf)
{
    uint64_t h = XXH3_64bits(id.data(), id.size());
    InsertOrAssignDefaultHeapBuffer(h, buf);
}

void SharedShaderResources::RemoveDefaultHeapBuffer(uint64_t id, const Buffer& buf)
{
    std::unique_lock<std::shared_mutex> lock(m_defaultHeapMtx);
    auto numDeleted = m_defaultHeapBuffs.erase(id);
    Assert(numDeleted == 1, "Buffer with ID %llu was not found.", id);
}

void SharedShaderResources::RemoveDefaultHeapBuffer(std::string_view id, const Buffer& buf)
{
    uint64_t h = XXH3_64bits(id.data(), id.size());
    auto numDeleted = m_defaultHeapBuffs.erase(h);
    Assert(numDeleted == 1, "Buffer with ID %llu was not found.", id);
}

const DescriptorTable* SharedShaderResources::GetDescriptorTable(uint64_t id)
{
    std::shared_lock<std::shared_mutex> lock(m_descTableMtx);
    if (auto it = m_descTables.find(id); it)
        return *it.value();

    return nullptr;
}

const DescriptorTable* SharedShaderResources::GetDescriptorTable(std::string_view id)
{
    uint64_t h = XXH3_64bits(id.data(), id.size());
    return GetDescriptorTable(h);
}

void SharedShaderResources::InsertOrAssignDescriptorTable(uint64_t id, const DescriptorTable& t)
{
    std::unique_lock<std::shared_mutex> lock(m_descTableMtx);
    m_descTables[id] = &t;
}

void SharedShaderResources::InsertOrAssignDescriptorTable(std::string_view id, const DescriptorTable& t)
{
    uint64_t h = XXH3_64bits(id.data(), id.size());
    InsertOrAssignDescriptorTable(h, t);
}