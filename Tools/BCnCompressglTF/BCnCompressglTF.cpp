#include <App/Filesystem.h>
#include <App/Common.h>
#include <Support/MemoryArena.h>
#include "TexConv/texconv.h"

#define STB_IMAGE_IMPLEMENTATION
#include <stb/stb_image.h>

#define JSON_NOEXCEPTION
#define JSON_NO_IO
#include <tinyglTF/json.hpp>
using json = nlohmann::json;

#include <wrl/client.h>
using Microsoft::WRL::ComPtr;

using namespace ZetaRay;
using namespace ZetaRay::App;
using namespace ZetaRay::Util;
using namespace ZetaRay::Support;

namespace
{
    static constexpr int MAX_TEX_RES = 2048;
    static constexpr const char* COMPRESSED_DIR_NAME = "compressed";

    enum TEXTURE_TYPE
    {
        BASE_COLOR,
        NORMAL_MAP,
        METALNESS_ROUGHNESS,
        EMISSIVE
    };

    const char* GetTexFormat(TEXTURE_TYPE t) noexcept
    {
        switch (t)
        {
        case BASE_COLOR:
            return "BC7_UNORM_SRGB";
        case NORMAL_MAP:
            return "BC5_UNORM";
        case METALNESS_ROUGHNESS:
            return "BC5_UNORM";
        case EMISSIVE:
            return "BC7_UNORM_SRGB";
        default:
            Check(false, "unreachable case.");
            return "";
        }
    }

    void CreateDevice(ID3D11Device** pDevice) noexcept
    {
        Assert(pDevice, "invalid arg.");
        *pDevice = nullptr;

        const D3D_FEATURE_LEVEL featureLevels[] =
        {
            D3D_FEATURE_LEVEL_11_0,
        };

        ComPtr<IDXGIFactory1> dxgiFactory;
        HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(dxgiFactory.GetAddressOf()));
        Check(SUCCEEDED(hr), "CreateDXGIFactory1() failed with code: %d", hr);

        ComPtr<IDXGIAdapter> pAdapter;
        if (FAILED(dxgiFactory->EnumAdapters(0, pAdapter.GetAddressOf())))
            Check(false, "ERROR: Invalid GPU adapter index 0!\n");

        hr = D3D11CreateDevice(pAdapter.Get(), D3D_DRIVER_TYPE_UNKNOWN,
            nullptr, 0, featureLevels, 1,
            D3D11_SDK_VERSION, pDevice, nullptr, nullptr);

        Check(SUCCEEDED(hr), "D3D11CreateDevice() failed with code: %d", hr);

        ComPtr<IDXGIDevice> dxgiDevice;
        hr = (*pDevice)->QueryInterface(IID_PPV_ARGS(dxgiDevice.GetAddressOf()));
        if (SUCCEEDED(hr))
        {
            hr = dxgiDevice->GetAdapter(pAdapter.ReleaseAndGetAddressOf());
            if (SUCCEEDED(hr))
            {
                DXGI_ADAPTER_DESC desc;
                hr = pAdapter->GetDesc(&desc);
                if (SUCCEEDED(hr))
                {
                    wprintf(L"\n[Using DirectCompute on \"%ls\"]\n", desc.Description);
                }
            }
        }
    }

    void ConvertBaseColorMaps(const Filesystem::Path& pathToglTF, const Filesystem::Path& outDir,
        Span<int> baseColorMaps, Span<Filesystem::Path> imagePaths, ID3D11Device* device)
    {
        for (auto tex : baseColorMaps)
        {
            // URI paths are relative to gltf file
            Filesystem::Path imgPath(pathToglTF.Get());
            imgPath.ToCurrDirectory();

            auto& imgUri = imagePaths[tex];
            imgPath.Append(imgUri.Get());

            int x;
            int y;
            int comp;
            Check(stbi_info(imgPath.Get(), &x, &y, &comp), "stbi_info() for path %s failed: %s", imgPath.Get(), stbi_failure_reason());

            int w = std::min(x, MAX_TEX_RES);
            int h = std::min(y, MAX_TEX_RES);

            // Direct3D requires BC image to be multiple of 4 in width & height
            w = (int)Math::AlignUp(w, 4);
            h = (int)Math::AlignUp(h, 4);

            char buff[512];
            const char* format = GetTexFormat(TEXTURE_TYPE::BASE_COLOR);
            stbsp_snprintf(buff, sizeof(buff), " -w %d -h %d -m 0 -ft dds -f %s -srgb -nologo -o %s %s", w, h, 
                format, outDir.Get(), imgPath.Get());

            wchar_t wideBuff[1024];
            int n = Common::CharToWideStr(buff, wideBuff);

            wchar_t* ptr = wideBuff;

            constexpr int numArgs = 16;
            wchar_t* args[numArgs];
            int currArg = 0;

            while (ptr != wideBuff + n)
            {
                args[currArg++] = ptr;

                while (*ptr != ' ' && *ptr != '\0')
                    ptr++;

                *ptr++ = '\0';
            }

            TexConv(numArgs, args, device);
        }
    }

    void ConvertNormalMaps(const Filesystem::Path& pathToglTF, const Filesystem::Path& outDir,
        Span<int> normalMaps, Span<Filesystem::Path> imagePaths, ID3D11Device* device)
    {
        for (auto tex : normalMaps)
        {
            Filesystem::Path imgPath(pathToglTF.Get());
            imgPath.ToCurrDirectory();

            auto& imgUri = imagePaths[tex];
            imgPath.Append(imgUri.Get());

            int x;
            int y;
            int comp;
            Check(stbi_info(imgPath.Get(), &x, &y, &comp), "stbi_info() for path %s failed: %s", imgPath.Get(), stbi_failure_reason());

            int w = std::min(x, MAX_TEX_RES);
            int h = std::min(y, MAX_TEX_RES);

            // Direct3D requires BC image to be multiple of 4 in width & height
            w = (int)Math::AlignUp(w, 4);
            h = (int)Math::AlignUp(h, 4);

            char buff[512];
            const char* format = GetTexFormat(TEXTURE_TYPE::NORMAL_MAP);
            stbsp_snprintf(buff, sizeof(buff), " -w %d -h %d -m 0 -ft dds -f %s -nologo -o %s %s", w, h, 
                format, outDir.Get(), imgPath.Get());

            wchar_t wideBuff[1024];
            int n = Common::CharToWideStr(buff, wideBuff);

            wchar_t* ptr = wideBuff;

            constexpr int numArgs = 15;
            wchar_t* args[numArgs];
            int currArg = 0;

            while (ptr != wideBuff + n)
            {
                args[currArg++] = ptr;

                while (*ptr != ' ' && *ptr != '\0')
                    ptr++;

                *ptr++ = '\0';
            }

            TexConv(numArgs, args, device);
        }
    }

    void ConvertMetalnessRoughnessMaps(const Filesystem::Path& pathToglTF, const Filesystem::Path& outDir,
        Span<int> mrMaps, Span<Filesystem::Path> imagePaths, ID3D11Device* device)
    {
        for (auto tex : mrMaps)
        {
            Filesystem::Path imgPath(pathToglTF.Get());
            imgPath.ToCurrDirectory();

            auto& imgUri = imagePaths[tex];
            imgPath.Append(imgUri.Get());

            int x;
            int y;
            int comp;
            Check(stbi_info(imgPath.Get(), &x, &y, &comp), "stbi_info() for path %s failed: %s", imgPath.Get(), stbi_failure_reason());

            int w = std::min(x, MAX_TEX_RES);
            int h = std::min(y, MAX_TEX_RES);

            // Direct3D requires BC image to be multiple of 4 in width & height
            w = (int)Math::AlignUp(w, 4);
            h = (int)Math::AlignUp(h, 4);

            char buff[512];
            const char* format = GetTexFormat(TEXTURE_TYPE::METALNESS_ROUGHNESS);
            stbsp_snprintf(buff, sizeof(buff), " -w %d -h %d -m 0 -ft dds -f %s -nologo -o %s %s", w, h,
                format, outDir.Get(), imgPath.Get());

            wchar_t wideBuff[1024];
            int n = Common::CharToWideStr(buff, wideBuff);

            wchar_t* ptr = wideBuff;

            constexpr int numArgs = 15;
            wchar_t* args[numArgs];
            int currArg = 0;

            while (ptr != wideBuff + n)
            {
                args[currArg++] = ptr;

                while (*ptr != ' ' && *ptr != '\0')
                    ptr++;

                *ptr++ = '\0';
            }

            TexConv(numArgs, args, device);
        }
    }

    void ConvertEmissiveMaps(const Filesystem::Path& pathToglTF, const Filesystem::Path& outDir,
        Span<int> emissiveMaps, Span<Filesystem::Path> imagePaths, ID3D11Device* device)
    {
        for (auto tex : emissiveMaps)
        {
            Filesystem::Path imgPath(pathToglTF.Get());
            imgPath.ToCurrDirectory();

            auto& imgUri = imagePaths[tex];
            imgPath.Append(imgUri.Get());

            int x;
            int y;
            int comp;
            Check(stbi_info(imgPath.Get(), &x, &y, &comp), "stbi_info() for path %s failed: %s", imgPath.Get(), stbi_failure_reason());

            int w = std::min(x, MAX_TEX_RES);
            int h = std::min(y, MAX_TEX_RES);

            // Direct3D requires BC image to be multiple of 4 in width & height
            w = (int)Math::AlignUp(w, 4);
            h = (int)Math::AlignUp(h, 4);

            char buff[512];
            const char* format = GetTexFormat(TEXTURE_TYPE::EMISSIVE);
            stbsp_snprintf(buff, sizeof(buff), " -w %d -h %d -m 0 -ft dds -f %s -srgb -nologo -o %s %s", w, h,
                format, outDir.Get(), imgPath.Get());

            wchar_t wideBuff[1024];
            int n = Common::CharToWideStr(buff, wideBuff);

            wchar_t* ptr = wideBuff;

            constexpr int numArgs = 16;
            wchar_t* args[numArgs];
            int currArg = 0;

            while (ptr != wideBuff + n)
            {
                args[currArg++] = ptr;

                while (*ptr != ' ' && *ptr != '\0')
                    ptr++;

                *ptr++ = '\0';
            }

            TexConv(numArgs, args, device);
        }
    }

    void ModifyImageURIs(json& data, const char* compressedDirName, const Filesystem::Path& gltfPath) noexcept
    {
        std::string s;
        char filename[MAX_PATH];

        size_t fnLen;

        for (auto& img : data["images"])
        {
            s = img["uri"];

            // extract the image file name
            Filesystem::Path p((StrView(s.data(), s.size())));
            p.Stem(filename, &fnLen);

            // change the extension to dds
            Check(fnLen + 5 < ZetaArrayLen(filename), "buffer is too small.");
            filename[fnLen] = '.';
            filename[fnLen + 1] = 'd';
            filename[fnLen + 2] = 'd';
            filename[fnLen + 3] = 's';
            filename[fnLen + 4] = '\0';

            // URI paths are relative to gltf file
            Filesystem::Path newPath(compressedDirName);
            newPath.Append(filename);

            img["uri"] = newPath.Get();
        }
        
        // serialize
        gltfPath.Stem(filename, &fnLen);

        Check(fnLen + 13 < ZetaArrayLen(filename), "buffer is too small.");
        filename[fnLen] = '_';
        filename[fnLen + 1] = 'b';
        filename[fnLen + 2] = 'a';
        filename[fnLen + 3] = 'c';
        filename[fnLen + 4] = 'k';
        filename[fnLen + 5] = 'u';
        filename[fnLen + 6] = 'p';
        filename[fnLen + 7] = '.';
        filename[fnLen + 8] = 'g';
        filename[fnLen + 9] = 'l';
        filename[fnLen + 10] = 't';
        filename[fnLen + 11] = 'f';
        filename[fnLen + 12] = '\0';

        Filesystem::Path backupPath(gltfPath.Get());
        backupPath.ToCurrDirectory().Append(filename);

        // make a backup
        Filesystem::Copy(gltfPath.Get(), backupPath.Get());

        s = data.dump(4);
        uint8_t* str = reinterpret_cast<uint8_t*>(s.data());
        Filesystem::WriteToFile(gltfPath.Get(), str, (uint32_t)s.size());   // overwrites the old one
    }
}

int main(int argc, char* argv[])
{
    //Filesystem::Path gltfPath("..\\assets\\cubic_realm\\zeta6.gltf");
    //Filesystem::Path gltfPath("zeta6_olagh.gltf");

    if (argc != 2)
    {
        printf("Usage: BCCompressglTF <path-to-glTF>\n");
        return 0;
    }

    Filesystem::Path gltfPath(argv[1]);
    if (!Filesystem::Exists(gltfPath.Get()))
    {
        printf("Provided path %s was not found. Exiting...\n", gltfPath.Get());
        return 0;
    }

    printf("Compressing textures for %s...\n", argv[1]);

    MemoryArena arena(64 * 1024);

	SmallVector<uint8_t, Support::ArenaAllocator> file(arena);
	Filesystem::LoadFromFile(gltfPath.Get(), file);

	json data = json::parse(file.data(), file.data() + file.size(), nullptr, false);

    SmallVector<int, Support::ArenaAllocator> textures(arena);
    textures.reserve(data["textures"].size());

    for (auto& tex : data["textures"])
    {
        int imgIdx = tex["source"];
        textures.push_back(imgIdx);
    }

    SmallVector<Filesystem::Path, Support::ArenaAllocator> imagePaths(arena);
    imagePaths.resize(data["images"].size());

    {
        std::string s;
        s.resize(MAX_PATH);
        int i = 0;

        for (auto& img : data["images"])
        {
            s = img["uri"];
            imagePaths[i++].Reset(StrView(s.data(), s.size()));
        }
    }

    const size_t numMats = data["materials"].size();

    SmallVector<int, Support::ArenaAllocator> normalMaps(arena);
    normalMaps.reserve(numMats);

    // extract normal map texture indices
    for (auto& mat : data["materials"])
    {
        if (mat.contains("normalTexture"))
        {
            int texIdx = mat["normalTexture"]["index"];
            normalMaps.push_back(texIdx);
        }
    }

    SmallVector<int, Support::ArenaAllocator> baseColorMaps(arena);
    baseColorMaps.reserve(numMats);

    SmallVector<int, Support::ArenaAllocator> metalnessRoughnessMaps(arena);
    metalnessRoughnessMaps.reserve(numMats);

    SmallVector<int, Support::ArenaAllocator> emissiveMaps(arena);
    emissiveMaps.reserve(numMats);

    // extract pbr texture indices
    for (auto& mat : data["materials"])
    {
        if (mat.contains("pbrMetallicRoughness"))
        {
            auto& pbr = mat["pbrMetallicRoughness"];
            
            if (pbr.contains("baseColorTexture"))
            {
                int texIdx = pbr["baseColorTexture"]["index"];
                baseColorMaps.push_back(texIdx);
            }

            if (pbr.contains("metallicRoughnessTexture"))
            {
                int texIdx = pbr["metallicRoughnessTexture"]["index"];
                metalnessRoughnessMaps.push_back(texIdx);
            }
        }
    }

    // extract emissive map texture indices
    for (auto& mat : data["materials"])
    {
        if (mat.contains("emissiveTexture"))
        {
            int texIdx = mat["emissiveTexture"]["index"];
            emissiveMaps.push_back(texIdx);
        }
    }

    printf("Stats:\n\
        #images: %llu \n\
        #textures: %llu\n\
        #base-color textures: %llu\n\
        #normal-map textures: %llu\n\
        #metalness-roughness textures: %llu\n\
        #emissive textures: %llu\n", imagePaths.size(), textures.size(), baseColorMaps.size(), normalMaps.size(),
        metalnessRoughnessMaps.size(), emissiveMaps.size());
   
    ComPtr<ID3D11Device> device;
    CreateDevice(device.GetAddressOf());

    Filesystem::Path outDir(gltfPath.Get());
    outDir.ToCurrDirectory().Append(COMPRESSED_DIR_NAME);
    Filesystem::CreateDirectoryIfNotExists(outDir.Get());

    ConvertBaseColorMaps(gltfPath, outDir, baseColorMaps, imagePaths, device.Get());
    ConvertNormalMaps(gltfPath, outDir, normalMaps, imagePaths, device.Get());
    ConvertMetalnessRoughnessMaps(gltfPath, outDir, metalnessRoughnessMaps, imagePaths, device.Get());
    ConvertEmissiveMaps(gltfPath, outDir, emissiveMaps, imagePaths, device.Get());

    ModifyImageURIs(data, COMPRESSED_DIR_NAME, gltfPath);

	return 0;
}