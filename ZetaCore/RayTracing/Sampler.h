#pragma once

#include "../Core/GpuMemory.h"

namespace ZetaRay::RT
{
	class Sampler
	{
	public:
		// 32 spp
		inline static constexpr const char* SOBOL_SEQ_32 = "SobolSeq_32";
		inline static constexpr const char* SCRAMBLING_TILE_32 = "ScramblingTile_32";
		inline static constexpr const char* RANKING_TILE_32 = "RankingTile_32";

		// Ref: E. Heitz, L. Belcour, V. Ostromoukhov, D. Coeurjolly and J. Iehl, "A Low-Discrepancy 
		// Sampler that Distributes Monte Carlo Errors as a Blue Noise in Screen Space," in SIGGRAPH, 2019.
		void InitLowDiscrepancyBlueNoise32() noexcept;
		void Clear() noexcept;

	private:
		// "An Owen-scrambled Sobol sequence of 256 samples of 256 dimensions"
		// "The keys are optimized for 32spp in 8d."
		Core::DefaultHeapBuffer m_sobolSeq32;

		// "The scrambling tile of 128x128 pixels."
		// "Each pixel contains an optimized 8d key used to scramble the sequence."
		Core::DefaultHeapBuffer m_scramblingTile32;

		// "The ranking tile of 128x128 pixels."
		// "Each pixel contains an optimized 8d key used to scramble the sequence."
		// "The keys are optimized for all the powers of two spp below 32 in 8d."
		Core::DefaultHeapBuffer m_rankingTile32;

		inline static constexpr const char* SobolSeqPath32 = "Samplers\\Low_Discrepancy_Blue_Noise\\sobol32.bin";
		inline static constexpr const char* ScramblingTilePath32 = "Samplers\\Low_Discrepancy_Blue_Noise\\scramblingTile32.bin";
		inline static constexpr const char* RankingTilePath32 = "Samplers\\Low_Discrepancy_Blue_Noise\\rankingTile32.bin";
	};
}