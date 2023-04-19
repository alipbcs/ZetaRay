//
// Main.cpp
//

#include <App/Log.h>
#include <App/Timer.h>
#include <Scene/SceneCore.h>
#include <Model/glTF.h>
#include <Default/DefaultRenderer.h>
#include <App/Filesystem.h>

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

    _declspec(dllexport) extern const UINT D3D12SDKVersion = 608;
    _declspec(dllexport) extern const char8_t* D3D12SDKPath = u8".\\D3D12\\";
}

int WINAPI WinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ PSTR lpCmdLine, _In_ int nCmdShow)
{
#ifdef _DEBUG
    AllocConsole();
    HANDLE stdHandle = GetStdHandle(STD_OUTPUT_HANDLE);
    int hConsole = _open_osfhandle(reinterpret_cast<intptr_t>(stdHandle), _O_TEXT);
    FILE* fp = _fdopen(hConsole, "w");
    freopen_s(&fp, "CONOUT$", "w", stdout);
#endif // _DEBUG

    Check(strlen(lpCmdLine), "Usage: ZetaLab <path-to-gltf>\n");

    {
        App::Filesystem::Path path(App::GetAssetDir());
        path.Append(lpCmdLine);
        Check(App::Filesystem::Exists(path.Get()), "Provided path was not found: %s\nExiting...\n", lpCmdLine);

        App::DeltaTimer timer;
        timer.Start();

        auto rndIntrf = DefaultRenderer::InitAndGetInterface();
        App::Init(rndIntrf);

        timer.End();

        LOG_UI(INFO, "App initialization completed in %u[ms]\n", (uint32_t)timer.DeltaMilli());

        // load the gltf model(s)
        timer.Start();

        Model::glTF::Load(path);

        App::FlushWorkerThreadPool();

        timer.End();

        LOG_UI(INFO, "gltf model(s) loaded in %u[ms]\n", (uint32_t)timer.DeltaMilli());
    }

    int ret = App::Run();

    return 0;
}