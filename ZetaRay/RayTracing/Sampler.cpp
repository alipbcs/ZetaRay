#include "Sampler.h"
#include "../Core/Renderer.h"
#include "../Core/SharedShaderResources.h"
#include "../App/Filesystem.h"
#include "../Support/Task.h"

using namespace ZetaRay;
using namespace ZetaRay::RT;
using namespace ZetaRay::Support;
using namespace ZetaRay::Util;
using namespace ZetaRay::App;

void Sampler::InitLowDiscrepancyBlueNoise() noexcept
{
    TaskSet ts;

    auto h0 = ts.EmplaceTask("SobolSeq", [this]()
        {
            Filesystem::Path p(App::GetAssetDir());
            p.Append(SobolSeqPath);

            const int sobolSeqSizeInBytes = 256 * 256 * sizeof(int);
            //Vector<uint8_t> sobelSeq(sobolSeqSizeInBytes);
            SmallVector<uint8_t, App::ThreadAllocator> sobelSeq;
            sobelSeq.resize(sobolSeqSizeInBytes);

            Filesystem::LoadFromFile(p.Get(), sobelSeq);

            StackStr(buff, n, "Sampler/SobolSeq");
            auto& renderer = App::GetRenderer();

            m_sobolSeq = renderer.GetGpuMemory().GetDefaultHeapBufferAndInit(buff,
                sobolSeqSizeInBytes, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, false,
                sobelSeq.data());

            renderer.GetSharedShaderResources().InsertOrAssignDefaultHeapBuffer(Sampler::SOBOL_SEQ, m_sobolSeq);
        });
    auto h1 = ts.EmplaceTask("ScramblingTile", [this]()
        {
            Filesystem::Path p(App::GetAssetDir());
            p.Append(ScramblingTilePath);

            const int scramblingTileSizeInBytes = 128 * 128 * 8 * sizeof(int);
            //Vector<uint8_t> scramblingTile(scramblingTileSizeInBytes);
            SmallVector<uint8_t, App::ThreadAllocator> scramblingTile;
            scramblingTile.resize(scramblingTileSizeInBytes);

            Filesystem::LoadFromFile(p.Get(), scramblingTile);

            StackStr(buff, n, "Sampler/ScramblingTile");
            auto& renderer = App::GetRenderer();

            m_scramblingTile = renderer.GetGpuMemory().GetDefaultHeapBufferAndInit(buff,
                scramblingTileSizeInBytes, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, false,
                scramblingTile.data());

            renderer.GetSharedShaderResources().InsertOrAssignDefaultHeapBuffer(Sampler::SCRAMBLING_TILE, m_scramblingTile);
        });
    auto h2 = ts.EmplaceTask("RankingTile", [this]()
        {
            Filesystem::Path p(App::GetAssetDir());
            p.Append(RankingTilePath);

            const int rankingTileSizeInBytes = 128 * 128 * 8 * sizeof(int);
            //Vector<uint8_t> rankingTile(rankingTileSizeInBytes);
            SmallVector<uint8_t, App::ThreadAllocator> rankingTile;
            rankingTile.resize(rankingTileSizeInBytes);

            Filesystem::LoadFromFile(p.Get(), rankingTile);

            StackStr(buff, n, "Sampler/RankingTile");
            auto& renderer = App::GetRenderer();

            m_rankingTile = renderer.GetGpuMemory().GetDefaultHeapBufferAndInit(buff,
                rankingTileSizeInBytes, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, false,
                rankingTile.data());

            renderer.GetSharedShaderResources().InsertOrAssignDefaultHeapBuffer(Sampler::RANKING_TILE, m_rankingTile);
        });

    ts.Sort();
    ts.Finalize();
    App::Submit(ZetaMove(ts));
}

void Sampler::Clear() noexcept
{
    m_rankingTile.Reset();
    m_scramblingTile.Reset();
    m_sobolSeq.Reset();
}
