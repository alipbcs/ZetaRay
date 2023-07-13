#pragma once

#include "../Utility/Error.h"
#include <string.h>

namespace ZetaRay::Support
{
	template<size_t BlockSize>
	struct FrameMemory
	{
		FrameMemory() noexcept
		{
			memset(m_blocks, 0, sizeof(MemoryBlock) * NUM_BLOCKS);
		}
		~FrameMemory() noexcept
		{
			for (int i = 0; i < NUM_BLOCKS; i++)
			{
				if (m_blocks[i].Start)
					free(m_blocks[i].Start);
			}
		}

		FrameMemory(FrameMemory&&) = delete;
		FrameMemory& operator=(FrameMemory&&) = delete;

		struct MemoryBlock
		{
			void* Start;
			uintptr_t Offset;
			int UsageCounter;
		};

		ZetaInline MemoryBlock& GetAndInitIfEmpty(int i) noexcept
		{
			Assert(i >= 0 && i < NUM_BLOCKS, "invalid block index.");

			if (!m_blocks[i].Start)
			{
				m_blocks[i].Start = malloc(BLOCK_SIZE);
				m_blocks[i].Offset = 0;
			}

			m_blocks[i].UsageCounter = NUM_FRAMES_TO_FREE_DELAY;

			return m_blocks[i];
		}

		void Reset() noexcept
		{
			for (int i = 0; i < NUM_BLOCKS; i++)
			{
				m_blocks[i].Offset = 0;

				m_blocks[i].UsageCounter = (m_blocks[i].UsageCounter == NUM_FRAMES_TO_FREE_DELAY) ?
					NUM_FRAMES_TO_FREE_DELAY :
					m_blocks[i].UsageCounter - 1;

				if (m_blocks[i].UsageCounter == 0)
				{
					free(m_blocks[i].Start);
					m_blocks[i].Start = nullptr;
				}
			}
		}

		size_t TotalSize() noexcept
		{
			size_t sum = 0;

			for (int i = 0; i < NUM_BLOCKS; i++)
			{
				if (m_blocks[i].Start)
					sum += BLOCK_SIZE;
			}

			return sum;
		}

		static constexpr int NUM_BLOCKS = MAX_NUM_THREADS * 2;
		static constexpr int NUM_FRAMES_TO_FREE_DELAY = 10;
		static constexpr size_t BLOCK_SIZE = BlockSize;

		MemoryBlock m_blocks[NUM_BLOCKS];
	};
}