#pragma once

#include "../RenderPass.h"
#include <Core/RootSignature.h>
#include <Core/GpuMemory.h>
#include <Core/DescriptorHeap.h>
#include "Sky_Common.h"

namespace ZetaRay::Core
{
	class CommandList;
}

namespace ZetaRay::Support
{
	struct ParamVariant;
}

namespace ZetaRay::RenderPass
{
	struct Sky final
	{
		enum class SHADER_OUT_RES
		{
			SKY_VIEW_LUT,
			INSCATTERING,
			COUNT
		};

		Sky();
		~Sky();

		void Init(int lutWidth, int lutHeight, bool doInscattering);
		bool IsInitialized() { return m_psos[(int)SHADERS::SKY_LUT] != nullptr; };
		bool IsInscatteringEnabled() { return m_doInscattering; }
		void Reset();
		void SetInscatteringEnablement(bool b);
		void GetVoxelGridDim(uint32_t& x, uint32_t& y, uint32_t& z) { x = m_localCB.NumVoxelsX; y = m_localCB.NumVoxelsY; z = INSCATTERING_THREAD_GROUP_SIZE_X; }
		void GetVoxelGridDepth(float& zNear, float& zFar) { zNear = m_localCB.VoxelGridNearZ, zFar = m_localCB.VoxelGridFarZ; }
		float GetVoxelGridMappingExp() { return m_localCB.DepthMappingExp; }
		Core::GpuMemory::Texture& GetOutput(SHADER_OUT_RES i)
		{
			Assert((int)i < (int)SHADER_OUT_RES::COUNT, "out-of-bound access.");

			if (i == SHADER_OUT_RES::SKY_VIEW_LUT)
				return m_lut;
			else
				return m_voxelGrid;
		}
		void Render(Core::CommandList& cmdList);

	private:
		void CreateSkyviewLUT();
		void CreateVoxelGrid();

		static constexpr int NUM_CBV = 1;
		static constexpr int NUM_SRV = 1;
		static constexpr int NUM_UAV = 0;
		static constexpr int NUM_GLOBS = 1;
		static constexpr int NUM_CONSTS = sizeof(cbSky) / sizeof(DWORD);

		struct ResourceFormats
		{
			static constexpr DXGI_FORMAT INSCATTERING_VOXEL_GRID = DXGI_FORMAT_R11G11B10_FLOAT;
			static constexpr DXGI_FORMAT SKY_VIEW_LUT = DXGI_FORMAT_R11G11B10_FLOAT;
		};

		RpObjects s_rpObjs;

		// both shaders use the same root signature
		Core::RootSignature m_rootSig;

		// sky look-up table
		Core::GpuMemory::Texture m_lut;

		struct DefaultParamVals
		{
			static constexpr int NUM_VOXELS_X = 192;
			static constexpr int NUM_VOXELS_Y = int(NUM_VOXELS_X / 1.77f);
			static constexpr float DEPTH_MAP_EXP = 2.0f;
			static constexpr float VOXEL_GRID_NEAR_Z = 0.5f;
			static constexpr float VOXEL_GRID_FAR_Z = 30.0f;
		};

		// voxel grid
		Core::GpuMemory::Texture m_voxelGrid;

		// root constants
		cbSky m_localCB;

		enum class DESC_TABLE
		{
			SKY_LUT_UAV,
			VOXEL_GRID_UAV,
			COUNT
		};

		Core::DescriptorTable m_descTable;

		bool m_doInscattering = false;

		enum class SHADERS
		{
			SKY_LUT,
			INSCATTERING,
			COUNT
		};

		ID3D12PipelineState* m_psos[(int)SHADERS::COUNT] = { nullptr };
		inline static constexpr const char* COMPILED_CS[(int)SHADERS::COUNT] = { "SkyViewLUT_cs.cso", "Inscattering_cs.cso" };

		// parameter callbacks
		void DepthMapExpCallback(const Support::ParamVariant& p);
		void VoxelGridNearZCallback(const Support::ParamVariant& p);
		void VoxelGridFarZCallback(const Support::ParamVariant& p);

		// shader reload
		void ReloadInscatteringShader();
		void ReloadSkyLUTShader();
	};
}