//
// Main.cpp
//

#include <Win32/Log.h>
#include <Win32/Timer.h>
#include <Scene/Scene.h>
#include <RenderPass/Common/RtCommon.h>
#include <Math/MatrixFuncs.h>
#include <Model/Mesh.h>
#include <fcntl.h>
#include <io.h>

using namespace ZetaRay;
using namespace ZetaRay::Math;

// Indicates to hybrid graphics systems to prefer the discrete part by default
extern "C"
{
    __declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
    __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;

    _declspec(dllexport) extern const UINT D3D12SDKVersion = 602;
    _declspec(dllexport) extern const char8_t* D3D12SDKPath = u8".\\D3D12\\";
}

// Entry point
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow)
{
	AllocConsole();
	HANDLE stdHandle = GetStdHandle(STD_OUTPUT_HANDLE);
	int hConsole = _open_osfhandle(reinterpret_cast<intptr_t>(stdHandle), _O_TEXT);
	FILE* fp = _fdopen(hConsole, "w");
	freopen_s(&fp, "CONOUT$", "w", stdout);

	App::Init();
	Scene& scene = App::GetScene();

	const uint64_t SCENE_ID = 0;

	// grid
	{
		Asset::MaterialDesc desc;
		desc.Reset();
		//desc.BaseColorFactor = float4(0.03f, 0.32f, 0.56f, 1.0f);
		desc.BaseColorFactor = float4(0.0f);
		desc.Index = 0;

		// matID = Hash(index, sceneID)
		scene.AddMaterial(SCENE_ID, ZetaMove(desc));

		Asset::MeshSubset subset;
		subset.MeshIdx = 0;
		subset.MeshPrimIdx = 0;
		subset.MaterialIdx = 0;

		SmallVector<VertexPosNormalTexTangent> vertices;
		SmallVector<uint32_t> indices;
		PrimitiveMesh::ComputeGrid(vertices, indices, 1000.0f, 1000.0f, 4, 4);
		subset.Vertices = ZetaMove(vertices);
		subset.Indices = ZetaMove(indices);

		scene.AddMesh(SCENE_ID, ZetaMove(subset));

		float4x3 I(store(identity()));

		Asset::InstanceDesc insdesc{
			.LocalTransform = I,
			.MeshIdx = 0,
			.Name = "Grid",
			.ParentID = (uint64_t)-1,
			.MeshPrimIdx = 0,
			.RtMeshMode = RT_MESH_MODE::STATIC,
			.RtInstanceMask = RT_AS_SUBGROUP::NON_EMISSIVE };

		scene.AddInstance(SCENE_ID, ZetaMove(insdesc));
	}

	// sphere
	{
		{
			Asset::MaterialDesc desc;
			desc.Reset();
			desc.BaseColorFactor = float4(0.63f, 0.56f, 0.1f, 1.0f);
			desc.Index = 1;
			desc.MetallicFactor = 0.5f;
			desc.RoughnessFactor = 0.7f;

			// matID = Hash(index, sceneID)
			scene.AddMaterial(SCENE_ID, ZetaMove(desc));
		}

		Asset::MeshSubset subset;
		subset.MeshIdx = 1;
		subset.MeshPrimIdx = 0;
		subset.MaterialIdx = 1;

		SmallVector<VertexPosNormalTexTangent> vertices;
		SmallVector<uint32_t> indices;
		PrimitiveMesh::ComputeTeapot(vertices, indices, 5.0f, 64);
		subset.Vertices = ZetaMove(vertices);
		subset.Indices = ZetaMove(indices);

		scene.AddMesh(SCENE_ID, ZetaMove(subset));

		float4x3 T1(store(translate(-2.0f, 1.5f, 4.0f)));
		float4x3 T2(store(mul(scale(2.0f, 2.0f, 2.0f), translate(-2.0f, 4.0f, -14.0f))));

		Asset::InstanceDesc insdesc1{
			.LocalTransform = T1,
			.MeshIdx = 1,
			.Name = "Sphere1",
			.ParentID = (uint64_t)-1,
			.MeshPrimIdx = 0,
			.RtMeshMode = RT_MESH_MODE::STATIC,
			.RtInstanceMask = RT_AS_SUBGROUP::NON_EMISSIVE };

		Asset::InstanceDesc insdesc2{
			.LocalTransform = T2,
			.MeshIdx = 1,
			.Name = "Sphere2",
			.ParentID = (uint64_t)-1,
			.MeshPrimIdx = 0,
			.RtMeshMode = RT_MESH_MODE::STATIC,
			.RtInstanceMask = RT_AS_SUBGROUP::NON_EMISSIVE };

		scene.AddInstance(SCENE_ID, ZetaMove(insdesc1));
//		scene.AddInstance(SCENE_ID, ZetaMove(insdesc2));
	}

    App::FlushMainThreadPool();

    int ret = App::Run();

    return 0;
}