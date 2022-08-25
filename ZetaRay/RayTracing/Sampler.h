#pragma once

#include "../Core/ZetaRay.h"
#include "../Core/GpuMemory.h"

namespace ZetaRay::RT
{
	class Sampler
	{
	public:
		inline static const char* SOBOL_SEQ = "SobolSeq";
		inline static const char* SCRAMBLING_TILE = "ScramblingTile";
		inline static const char* RANKING_TILE = "RankingTile";

		// Ref: "A Low-Discrepancy Sampler that Distributes Monte Carlo Errors as a Blue Noise in Screen Space", Heitz et al.
		void InitLowDiscrepancyBlueNoise() noexcept;
		void Clear() noexcept;

	private:
		// "An Owen-scrambled Sobol sequence of 256 samples of 256 dimensions"
		// "The keys are optimized for 32spp in 8d."
		Core::DefaultHeapBuffer m_sobolSeq;

		// "The scrambling tile of 128x128 pixels."
		// "Each pixel contains an optimized 8d key used to scramble the sequence."
		Core::DefaultHeapBuffer m_scramblingTile;

		// "The ranking tile of 128x128 pixels."
		// "Each pixel contains an optimized 8d key used to scramble the sequence."
		// "The keys are optimized for all the powers of two spp below 32 in 8d."
		Core::DefaultHeapBuffer m_rankingTile;

		inline static const char* SobolSeqPath = "Samplers\\Low_Discrepancy_Blue_Noise\\sobol.bin";
		inline static const char* ScramblingTilePath = "Samplers\\Low_Discrepancy_Blue_Noise\\scramblingTile.bin";
		inline static const char* RankingTilePath = "Samplers\\Low_Discrepancy_Blue_Noise\\rankingTile.bin";
	};
}