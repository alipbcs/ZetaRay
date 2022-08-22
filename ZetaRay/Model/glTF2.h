#pragma once

#include "../Core/ZetaRay.h"

namespace ZetaRay
{
	namespace glTF2
	{
		void Load(const char* modelRelPath, bool zUpToYupConversion = false) noexcept;
	};
}