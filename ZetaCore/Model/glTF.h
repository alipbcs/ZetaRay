#pragma once

#include "../App/ZetaRay.h"

namespace ZetaRay::App::Filesystem
{
	struct Path;
}

namespace ZetaRay::Model::glTF
{
	void Load(const App::Filesystem::Path& p);
}