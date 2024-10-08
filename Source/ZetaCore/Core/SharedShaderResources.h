#pragma once

#include "../Utility/HashTable.h"
#include <shared_mutex>

namespace ZetaRay::Core::GpuMemory
{
    struct UploadHeapBuffer;
    struct Buffer;
}

namespace ZetaRay::Core
{
    struct DescriptorTable;

    // Allows sharing buffers (in upload and default heaps), descriptor tables, and other resources 
    // that are shared between various shaders. Access is synchronized.
    class SharedShaderResources
    {
    public:
        SharedShaderResources() = default;
        ~SharedShaderResources() = default;

        SharedShaderResources(SharedShaderResources&&) = delete;
        SharedShaderResources& operator==(SharedShaderResources&&) = delete;
        
        // Upload heap buffers
        const GpuMemory::UploadHeapBuffer* GetUploadHeapBuffer(uint64_t id);
        const GpuMemory::UploadHeapBuffer* GetUploadHeapBuffer(std::string_view id);
        void InsertOrAssignUploadHeapBuffer(uint64_t, const GpuMemory::UploadHeapBuffer& buffer);
        void InsertOrAssignUploadHeapBuffer(std::string_view id, GpuMemory::UploadHeapBuffer& buffer);

        // Default heap buffers
        const GpuMemory::Buffer* GetDefaultHeapBuffer(uint64_t id);
        const GpuMemory::Buffer* GetDefaultHeapBuffer(std::string_view id);
        void InsertOrAssignDefaultHeapBuffer(uint64_t id, const GpuMemory::Buffer& buffer);
        void InsertOrAssignDefaultHeapBuffer(std::string_view id, const GpuMemory::Buffer& buffer);
        void RemoveDefaultHeapBuffer(uint64_t id, const GpuMemory::Buffer& buffer);
        void RemoveDefaultHeapBuffer(std::string_view id, const GpuMemory::Buffer& buffer);

        // Descriptor tables
        const DescriptorTable* GetDescriptorTable(uint64_t id);
        const DescriptorTable* GetDescriptorTable(std::string_view id);
        void InsertOrAssignDescriptorTable(uint64_t id, const DescriptorTable& table);
        void InsertOrAssignDescriptorTable(std::string_view id, const DescriptorTable& table);

    private:
        Util::HashTable<const DescriptorTable*> m_descTables;
        Util::HashTable<const GpuMemory::UploadHeapBuffer*> m_uploadHeapBuffs;
        Util::HashTable<const GpuMemory::Buffer*> m_defaultHeapBuffs;

        std::shared_mutex m_descTableMtx;
        std::shared_mutex m_uploadHeapMtx;
        std::shared_mutex m_defaultHeapMtx;
    };
}