#include "Sampler.h"
#include "../Core/RendererCore.h"
#include "../Core/SharedShaderResources.h"
#include "../App/Filesystem.h"
#include "../Support/Task.h"
#include "../Core/GpuMemory.h"

using namespace ZetaRay;
using namespace ZetaRay::RT;
using namespace ZetaRay::Support;
using namespace ZetaRay::Util;
using namespace ZetaRay::App;
using namespace ZetaRay::Core;
using namespace ZetaRay::Core::GpuMemory;

void Sampler::InitLowDiscrepancyBlueNoise32()
{
    TaskSet ts;

    auto h0 = ts.EmplaceTask("SobolSeq32", [this]()
        {
            Filesystem::Path p(App::GetAssetDir());
            p.Append(SobolSeqPath32);

            const int sobolSeqSizeInBytes = 256 * 256 * sizeof(int);
            SmallVector<uint8_t> sobelSeq;
            sobelSeq.resize(sobolSeqSizeInBytes);

            Filesystem::LoadFromFile(p.Get(), sobelSeq);

            auto& renderer = App::GetRenderer();

            m_sobolSeq32 = GpuMemory::GetDefaultHeapBufferAndInit("SobolSeq32", sobolSeqSizeInBytes, 
                false, sobelSeq.data());

            renderer.GetSharedShaderResources().InsertOrAssignDefaultHeapBuffer(Sampler::SOBOL_SEQ_32, m_sobolSeq32);
        });
    auto h1 = ts.EmplaceTask("ScramblingTile32", [this]()
        {
            Filesystem::Path p(App::GetAssetDir());
            p.Append(ScramblingTilePath32);

            const int scramblingTileSizeInBytes = 128 * 128 * 8 * sizeof(int);
            SmallVector<uint8_t> scramblingTile;
            scramblingTile.resize(scramblingTileSizeInBytes);

            Filesystem::LoadFromFile(p.Get(), scramblingTile);

            auto& renderer = App::GetRenderer();

            m_scramblingTile32 = GpuMemory::GetDefaultHeapBufferAndInit("ScramblingTile32",
                scramblingTileSizeInBytes, false, scramblingTile.data());

            renderer.GetSharedShaderResources().InsertOrAssignDefaultHeapBuffer(Sampler::SCRAMBLING_TILE_32, m_scramblingTile32);
        });
    auto h2 = ts.EmplaceTask("RankingTile32", [this]()
        {
            Filesystem::Path p(App::GetAssetDir());
            p.Append(RankingTilePath32);

            const int rankingTileSizeInBytes = 128 * 128 * 8 * sizeof(int);
            SmallVector<uint8_t> rankingTile;
            rankingTile.resize(rankingTileSizeInBytes);

            Filesystem::LoadFromFile(p.Get(), rankingTile);

            auto& renderer = App::GetRenderer();

            m_rankingTile32 = GpuMemory::GetDefaultHeapBufferAndInit("RankingTile32",
                rankingTileSizeInBytes, false, rankingTile.data());

            renderer.GetSharedShaderResources().InsertOrAssignDefaultHeapBuffer(Sampler::RANKING_TILE_32, m_rankingTile32);
        });

    ts.Sort();
    ts.Finalize();
    App::Submit(ZetaMove(ts));
}

void Sampler::Clear()
{
    m_rankingTile32.Reset();
    m_scramblingTile32.Reset();
    m_sobolSeq32.Reset();    
}
