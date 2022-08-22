#pragma once

#include "../Utility/HashTable.h"
#include <shared_mutex>

namespace ZetaRay
{
	struct UploadHeapBuffer;
	struct DefaultHeapBuffer;
	struct DescriptorTable;

	// Allows sharing buffers (in upload and default heaps), descriptor tables and other reosurces 
	// that are shared between various shaders. Multiple threads can read and write to these, so access is 
	// synchronized.
	class SharedShaderResources
	{
	public:
		SharedShaderResources() noexcept = default;
		~SharedShaderResources() noexcept = default;

		SharedShaderResources(SharedShaderResources&&) = delete;
		SharedShaderResources& operator==(SharedShaderResources&&) = delete;
		
		// Upload heap buffers
		const UploadHeapBuffer* GetUploadHeapBuff(uint64_t id) noexcept;
		const UploadHeapBuffer* GetUploadHeapBuff(std::string_view id) noexcept;
		void InsertOrAssingUploadHeapBuffer(uint64_t, const UploadHeapBuffer& buf) noexcept;
		void InsertOrAssingUploadHeapBuffer(std::string_view id, UploadHeapBuffer& buf) noexcept;

		// Default heap buffers
		const DefaultHeapBuffer* GetDefaultHeapBuff(uint64_t id) noexcept;
		const DefaultHeapBuffer* GetDefaultHeapBuff(std::string_view id) noexcept;
		void InsertOrAssignDefaultHeapBuffer(uint64_t id, const DefaultHeapBuffer& buf) noexcept;
		void InsertOrAssignDefaultHeapBuffer(std::string_view id, const DefaultHeapBuffer& buf) noexcept;

		// Descriptor-tables
		const DescriptorTable* GetDescriptorTable(uint64_t id) noexcept;
		const DescriptorTable* GetDescriptorTable(std::string_view id) noexcept;
		void InsertOrAssingDescriptorTable(uint64_t id, const DescriptorTable& t) noexcept;
		void InsertOrAssingDescriptorTable(std::string_view id, const DescriptorTable& t) noexcept;

	private:
		HashTable<const DescriptorTable*> m_descTables;
		HashTable<const UploadHeapBuffer*> m_uploadHeapBuffs;
		HashTable<const DefaultHeapBuffer*> m_defaultHeapBuffs;

		std::shared_mutex m_descTableMtx;
		std::shared_mutex m_uploadHeapMtx;
		std::shared_mutex m_defaulHeapMtx;
	};
}