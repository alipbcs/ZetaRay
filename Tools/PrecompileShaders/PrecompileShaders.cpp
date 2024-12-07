#include <GBuffer/GBufferRT.h>
#include <Compositing/Compositing.h>
#include <AutoExposure/AutoExposure.h>
#include <Display/Display.h>
#include <DirectLighting/Emissive/DirectLighting.h>
#include <DirectLighting/Sky/SkyDI.h>
#include <PreLighting/PreLighting.h>
#include <IndirectLighting/IndirectLighting.h>

using namespace ZetaRay;
using namespace ZetaRay::App;
using namespace ZetaRay::Core;
using namespace ZetaRay::Support;
using namespace ZetaRay::Util;
using namespace ZetaRay::Math;
using namespace ZetaRay::RenderPass;

namespace
{
    struct Data
    {
        IndirectLighting indLighting;
        DirectLighting dirLighitng;
        PreLighting preLighting;
        Compositing compositing;
        SkyDI skyDI;
        AutoExposure autoExposure;
        GBufferRT gbuffer;
        DisplayPass display;
    };

    Data* g_data = nullptr;
}

// Indicates to hybrid graphics systems to prefer the discrete part by default
extern "C"
{
    __declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
    __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;

    _declspec(dllexport) extern const UINT D3D12SDKVersion = 613;
    _declspec(dllexport) extern const char8_t* D3D12SDKPath = u8".\\D3D12\\";
}

int main(int argc, char* argv[])
{
    App::InitBasic();
    g_data = new Data;

    g_data->indLighting.InitPSOs();
    g_data->dirLighitng.InitPSOs();
    g_data->skyDI.InitPSOs();
    g_data->preLighting.InitPSOs();
    g_data->compositing.InitPSOs();
    g_data->autoExposure.InitPSOs();
    g_data->gbuffer.InitPSOs();
    g_data->display.InitPSOs();

    App::FlushWorkerThreadPool();

    delete g_data;
    App::ShutdownBasic();

    return 0;
}