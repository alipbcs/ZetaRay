//
// Main.cpp
//

#include <App/Log.h>
#include <App/Timer.h>
#include <Scene/SceneCore.h>
#include <Model/glTF.h>
#include <Default/DefaultRenderer.h>

#ifdef _DEBUG
#include <fcntl.h>
#include <io.h>
#endif

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

int WINAPI wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPWSTR lpCmdLine, _In_ int nCmdShow)
{
#ifdef _DEBUG
    AllocConsole();
    HANDLE stdHandle = GetStdHandle(STD_OUTPUT_HANDLE);
    int hConsole = _open_osfhandle(reinterpret_cast<intptr_t>(stdHandle), _O_TEXT);
    FILE* fp = _fdopen(hConsole, "w");
    freopen_s(&fp, "CONOUT$", "w", stdout);
#endif // _DEBUG

    auto rndIntrf = DefaultRenderer::InitAndGetInterface();
    App::Init(rndIntrf);

    App::DeltaTimer timer;
    timer.Start();

    // load the gltf model(s)
    const char* p = "sponza_v9\\sponza_v9.gltf";
    //const char* p = "CornellBox_v2\\cornell9.gltf";
    //const char* p = "bistro_v6\\bistro_v6.gltf";
    //const char* p = "sponza_new\\sponza_new.gltf";
    //const char* p = "san_miguel\\san_miguel_v2.gltf";
    Model::glTF::Load(p, false);

    timer.End();
    App::FlushWorkerThreadPool();

    LOG_UI(INFO, "gltf model loaded in %u[us]\n", (uint32_t)timer.DeltaMicro());

    int ret = App::Run();

    return 0;
}