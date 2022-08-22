#ifndef SVGF_H
#define SVGF_H

namespace SVGF
{
	static const float INVALID_RAYT = -1;
	
	// TsppColorLumRayT
	uint3 PackHistoryData(in uint tspp, in float3 color, in float lum, in float lumSq)
	{
		uint4 ret;
	
		// tspp doesn't go above 64
		ret.x = (f32tof16(color.r) << 16) | f32tof16(lum);
		ret.y = (f32tof16(color.b) << 16) | f32tof16(color.g);
		ret.z = (f32tof16(lumSq) << 16) | tspp;
		ret.w = 0;
	
		return ret;
	}

	void UnpackHistoryData(in uint4 hist, out float tspp, out float3 color, out float lum, out float lumSq)
	{
		tspp = hist.x & 0xffff;
		
		color.r = f16tof32(hist.x >> 16);
		color.g = f16tof32(hist.y);
		color.b = f16tof32(hist.y >> 16);
		
		lum = f16tof32(hist.z);
		lumSq = f16tof32(hist.z >> 16);
		
//		rayT = asfloat(hist.w);
	}
}

#endif