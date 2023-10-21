#pragma once

#include "../Utility/HashTable.h"
#include <shared_mutex>

namespace ZetaRay::Core::GpuMemory
{
	struct UploadHeapBuffer;
	struct DefaultHeapBuffer;
}

namespace ZetaRay::Core
{
	struct DescriptorTable;

	// Allows sharing buffers (in upload and default heaps), descriptor tables and other reosurces 
	// that are shared between various shaders. Multiple threads can read and write to these, so access is 
	// synchronized.
	class SharedShaderResources
	{
	public:
		SharedShaderResources() = default;
		~SharedShaderResources() = default;

		SharedShaderResources(SharedShaderResources&&) = delete;
		SharedShaderResources& operator==(SharedShaderResources&&) = delete;
		
		// Upload heap buffers
		const GpuMemory::UploadHeapBuffer* GetUploadHeapBuff(uint64_t id);
		const GpuMemory::UploadHeapBuffer* GetUploadHeapBuff(std::string_view id);
		void InsertOrAssingUploadHeapBuffer(uint64_t, const GpuMemory::UploadHeapBuffer& buf);
		void InsertOrAssingUploadHeapBuffer(std::string_view id, GpuMemory::UploadHeapBuffer& buf);

		// Default heap buffers
		const GpuMemory::DefaultHeapBuffer* GetDefaultHeapBuff(uint64_t id);
		const GpuMemory::DefaultHeapBuffer* GetDefaultHeapBuff(std::string_view id);
		void InsertOrAssignDefaultHeapBuffer(uint64_t id, const GpuMemory::DefaultHeapBuffer& buf);
		void InsertOrAssignDefaultHeapBuffer(std::string_view id, const GpuMemory::DefaultHeapBuffer& buf);

		// Descriptor-tables
		const DescriptorTable* GetDescriptorTable(uint64_t id);
		const DescriptorTable* GetDescriptorTable(std::string_view id);
		void InsertOrAssingDescriptorTable(uint64_t id, const DescriptorTable& t);
		void InsertOrAssingDescriptorTable(std::string_view id, const DescriptorTable& t);

	private:
		Util::HashTable<const DescriptorTable*> m_descTables;
		Util::HashTable<const GpuMemory::UploadHeapBuffer*> m_uploadHeapBuffs;
		Util::HashTable<const GpuMemory::DefaultHeapBuffer*> m_defaultHeapBuffs;

		std::shared_mutex m_descTableMtx;
		std::shared_mutex m_uploadHeapMtx;
		std::shared_mutex m_defaulHeapMtx;
	};
}