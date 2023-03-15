#ifndef RTCOMMON_H
#define RTCOMMON_H

// Meshes present in an Acceleration Structure, can be subdivided into groups
// based on a specified 8-bit mask value. During ray travesal, instance mask from 
// the ray and corresponding mask from each mesh are ANDed together. Mesh is skipped
// if the result is zero.
namespace RT_AS_SUBGROUP
{
	static const uint32_t EMISSIVE = 0x1;
	static const uint32_t NON_EMISSIVE = 0x2;
	static const uint32_t ALL = EMISSIVE | NON_EMISSIVE;
}

#ifdef __cplusplus
namespace ZetaRay
{
#endif
	namespace RT
	{
		struct MeshInstance
		{
			uint32_t BaseVtxOffset;
			uint32_t BaseIdxOffset;
			float4_ Rotation;
			uint16_t MatID;
			uint16_t pad[3];
		};
	}
#ifdef __cplusplus
}
#endif

#endif
