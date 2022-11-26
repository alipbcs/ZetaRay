#pragma once

#include "../Core/ZetaRay.h"

namespace ZetaRay::Model::glTF
{
	void Load(const char* modelRelPath, bool blenderToYupConversion = false) noexcept;
}