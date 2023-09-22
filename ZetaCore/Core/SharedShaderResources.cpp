#include "SharedShaderResources.h"
#include <xxHash/xxhash.h>

using namespace ZetaRay;
using namespace ZetaRay::Core;
using namespace ZetaRay::Core::GpuMemory;

//--------------------------------------------------------------------------------------
// SharedShaderResources
//--------------------------------------------------------------------------------------

const UploadHeapBuffer* SharedShaderResources::GetUploadHeapBuff(uint64_t id) noexcept
{
	std::shared_lock<std::shared_mutex> lock(m_uploadHeapMtx);
	auto buff = m_uploadHeapBuffs.find(id);
	if(buff)
		return *buff;

	return nullptr;
}

const UploadHeapBuffer* SharedShaderResources::GetUploadHeapBuff(std::string_view id) noexcept
{
	uint64_t h = XXH3_64bits(id.data(), id.size());
	return GetUploadHeapBuff(h);
}

void SharedShaderResources::InsertOrAssingUploadHeapBuffer(std::string_view id, UploadHeapBuffer& buf) noexcept
{
	uint64_t h = XXH3_64bits(id.data(), id.size());
	InsertOrAssingUploadHeapBuffer(h, buf);
}

void SharedShaderResources::InsertOrAssingUploadHeapBuffer(uint64_t id, const UploadHeapBuffer& buf) noexcept
{
	std::unique_lock<std::shared_mutex> lock(m_uploadHeapMtx);
	m_uploadHeapBuffs[id] = &buf;
}

//void SharedShaderResources::RemoveUploadHeapBuffer(uint64_t id) noexcept
//{
//	std::unique_lock<std::shared_mutex> lock(m_uploadHeapMtx);
//	m_uploadHeapBuffs.erase(id);
//}

const DefaultHeapBuffer* SharedShaderResources::GetDefaultHeapBuff(uint64_t id) noexcept
{
	std::shared_lock<std::shared_mutex> lock(m_defaulHeapMtx);
	auto* it = m_defaultHeapBuffs.find(id);
	if (!it)
		return nullptr;

	return *it;
}

const DefaultHeapBuffer* SharedShaderResources::GetDefaultHeapBuff(std::string_view id) noexcept
{
	uint64_t h = XXH3_64bits(id.data(), id.size());
	return GetDefaultHeapBuff(h);
}

void SharedShaderResources::InsertOrAssignDefaultHeapBuffer(uint64_t id, const DefaultHeapBuffer& buf) noexcept
{
	std::unique_lock<std::shared_mutex> lock(m_defaulHeapMtx);
	m_defaultHeapBuffs[id] = &buf;
}

void SharedShaderResources::InsertOrAssignDefaultHeapBuffer(std::string_view id, const DefaultHeapBuffer& buf) noexcept
{
	uint64_t h = XXH3_64bits(id.data(), id.size());
	InsertOrAssignDefaultHeapBuffer(h, buf);
}

//void SharedShaderResources::RemoveDefaultHeapBuffer(uint64_t id) noexcept
//{
//	std::unique_lock<std::shared_mutex> lock(m_defaulHeapMtx);
//	m_defaultHeapBuffs.erase(id);
//}

const DescriptorTable* SharedShaderResources::GetDescriptorTable(uint64_t id) noexcept
{
	std::shared_lock<std::shared_mutex> lock(m_descTableMtx);

	if (auto it = m_descTables.find(id); it != nullptr)
		return *it;

	return nullptr;
}

const DescriptorTable* SharedShaderResources::GetDescriptorTable(std::string_view id) noexcept
{
	uint64_t h = XXH3_64bits(id.data(), id.size());
	return GetDescriptorTable(h);
}

void SharedShaderResources::InsertOrAssingDescriptorTable(uint64_t id, const DescriptorTable& t) noexcept
{
	std::unique_lock<std::shared_mutex> lock(m_descTableMtx);
	m_descTables[id] = &t;
}

void SharedShaderResources::InsertOrAssingDescriptorTable(std::string_view id, const DescriptorTable& t) noexcept
{
	uint64_t h = XXH3_64bits(id.data(), id.size());
	InsertOrAssingDescriptorTable(h, t);
}