#include <App/Path.h>
#include <App/Common.h>
#include <Support/MemoryArena.h>
#include <algorithm>
#include <Utility/Utility.h>
#include "TexConv/texconv.h"

#define STB_IMAGE_IMPLEMENTATION
#include <stb/stb_image.h>

#define CGLTF_IMPLEMENTATION
#define CGLTF_WRITE_IMPLEMENTATION
#include <cgltf/cgltf_write.h>

#include <wrl/client.h>
using Microsoft::WRL::ComPtr;

using namespace ZetaRay;
using namespace ZetaRay::App;
using namespace ZetaRay::Util;
using namespace ZetaRay::Support;
using namespace ZetaRay::Math;

namespace
{
    using ArenaPath = Filesystem::FilePath<ArenaAllocator, 256>;
    using ArenaPathNoInline = Filesystem::FilePath<ArenaAllocator, 0>;

    static constexpr int DEFAULT_MAX_TEX_RES = 4096;
    static constexpr const char* COMPRESSED_DIR_NAME = "compressed";

    namespace TEX_CONV_ARGV_NO_OVERWRITE_SRGB
    {
        static const char* CMD = " -w %d -h %d -m 0 -ft dds -f %s -srgb -nologo -o %s %s";
        constexpr int NUM_ARGS = 16;
    };

    namespace TEX_CONV_ARGV_OVERWRITE_SRGB
    {
        static const char* CMD = " -w %d -h %d -m 0 -ft dds -f %s -srgb -nologo -y -o %s %s";
        constexpr int NUM_ARGS = 17;
    }

    namespace TEX_CONV_ARGV_NO_OVERWRITE
    {
        static const char* CMD = " -w %d -h %d -m 0 -ft dds -f %s -nologo -o %s %s";
        constexpr int NUM_ARGS = 15;
    }

    namespace TEX_CONV_ARGV_OVERWRITE
    {
        static const char* CMD = " -w %d -h %d -m 0 -ft dds -f %s -nologo -y -o %s %s";
        constexpr int NUM_ARGS = 16;
    }

    namespace TEX_CONV_ARGV_NO_OVERWRITE_SWIZZLE
    {
        static const char* CMD = " -w %d -h %d -m 0 -ft dds -f %s -nologo -swizzle bg -o %s %s";
        constexpr int NUM_ARGS = 17;
    }

    namespace TEX_CONV_ARGV_OVERWRITE_SWIZZLE
    {
        static const char* CMD = " -w %d -h %d -m 0 -ft dds -f %s -nologo -swizzle bg -y -o %s %s";
        constexpr int NUM_ARGS = 18;
    }

    static constexpr int MAX_NUM_ARGS = Max(
        Max(TEX_CONV_ARGV_NO_OVERWRITE_SRGB::NUM_ARGS, TEX_CONV_ARGV_OVERWRITE_SRGB::NUM_ARGS),
        Max(Max(TEX_CONV_ARGV_NO_OVERWRITE::NUM_ARGS, TEX_CONV_ARGV_OVERWRITE::NUM_ARGS),
            Max(TEX_CONV_ARGV_NO_OVERWRITE_SWIZZLE::NUM_ARGS, TEX_CONV_ARGV_OVERWRITE_SWIZZLE::NUM_ARGS)));

    enum TEXTURE_TYPE
    {
        BASE_COLOR,
        NORMAL_MAP,
        METALNESS_ROUGHNESS,
        EMISSIVE
    };

    const char* GetTexFormat(TEXTURE_TYPE t)
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

    ZetaInline void DecodeURI_Inplace(ArenaPathNoInline& str, MemoryArena& arena)
    {
        auto hexToDecimal = [](unsigned char c)
            {
                if (c >= 48 && c <= 57)
                    return c - 48;
                if (c >= 65 && c <= 70)
                    return c - 65 + 10;
                if (c >= 97 && c <= 102)
                    return c - 97 + 10;

                return -1;
            };

        const int N = (int)str.Length();
        const unsigned char* beg = reinterpret_cast<const unsigned char*>(str.Get());
        bool needsConversion = false;

        for (int i = 0; i < N; i++)
        {
            if (beg[i] == '%')
            {
                needsConversion = true;
                break;
            }
        }

        if (!needsConversion)
            return;

        ArenaPath converted(arena);
        converted.Resize(str.Length());
        int curr = 0;
        unsigned char* out = reinterpret_cast<unsigned char*>(converted.Get());

        while (curr < N)
        {
            unsigned char c = *(beg + curr);

            if (c == '%')
            {
                int d1 = hexToDecimal(*(beg + curr + 1));
                Check(d1 != -1, "Unrecognized percent-encoding.");
                int d2 = hexToDecimal(*(beg + curr + 2));
                Check(d2 != -1, "Unrecognized percent-encoding.");

                unsigned char newC = (unsigned char)((d1 << 4) + d2);
                *(out++) = newC;
                curr += 3;

                continue;
            }

            *(out++) = c;
            curr++;
        }

        str.Reset(converted.GetView());
    }

    ZetaInline bool IsASCII(const ArenaPathNoInline& path)
    {
        auto view = path.GetView();

        for (size_t i = 0; i < path.Length(); i++)
        {
            if (view[i] < 0)
                return false;
        }

        return true;
    }

    void DecodeImageURIs(const ArenaPath& gltfPath, cgltf_data& model, MemoryArena& arena, 
        bool validate)
    {
        for (size_t i = 0; i < model.images_count; i++)
        {
            const cgltf_image& image = model.images[i];
            Check(image.uri, "Image has no URI.");

            ArenaPathNoInline imgPath(image.uri, arena);
            if (validate)
                Check(IsASCII(imgPath), "Paths with non-ASCII characters are not supported.");

            DecodeURI_Inplace(imgPath, arena);

            // DirectXTex expects backslashes
            imgPath.ConvertToBackslashes();
            // Modify to decoded path
            Assert(!imgPath.HasInlineStorage(), "Bug");
            model.images[i].uri = imgPath.Get();
        }
    }

    void CreateDevice(ID3D11Device** pDevice)
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
                    wprintf(L"\n[Using DirectCompute on \"%ls\"]\n", desc.Description);
            }
        }
    }

    bool ConvertTextures(TEXTURE_TYPE texType, const ArenaPath& glTFPath, 
        const ArenaPath& compressedDir, const char* compressedDirName,
        cgltf_data& model, Span<int> textureMaps, MemoryArena& arena, ID3D11Device* device, 
        bool srgb, bool forceOverwrite, int maxRes, Span<int> toSkip)
    {
        const char* formatStr = srgb ?
            (forceOverwrite ? TEX_CONV_ARGV_OVERWRITE_SRGB::CMD : TEX_CONV_ARGV_NO_OVERWRITE_SRGB::CMD) :
            (texType == METALNESS_ROUGHNESS ?
                (forceOverwrite ? TEX_CONV_ARGV_OVERWRITE_SWIZZLE::CMD : TEX_CONV_ARGV_NO_OVERWRITE_SWIZZLE::CMD) :
                (forceOverwrite ? TEX_CONV_ARGV_OVERWRITE::CMD : TEX_CONV_ARGV_NO_OVERWRITE::CMD));

        const int numArgs = srgb ?
            (forceOverwrite ? TEX_CONV_ARGV_OVERWRITE_SRGB::NUM_ARGS : TEX_CONV_ARGV_NO_OVERWRITE_SRGB::NUM_ARGS) :
            (texType == METALNESS_ROUGHNESS ?
                (forceOverwrite ? TEX_CONV_ARGV_OVERWRITE_SWIZZLE::NUM_ARGS : TEX_CONV_ARGV_NO_OVERWRITE_SWIZZLE::NUM_ARGS) :
                (forceOverwrite ? TEX_CONV_ARGV_OVERWRITE::NUM_ARGS : TEX_CONV_ARGV_NO_OVERWRITE::NUM_ARGS));

        const char* texFormat = GetTexFormat(texType);

        for (auto tex : textureMaps)
        {
            auto idx = BinarySearch(toSkip, tex);
            if (idx != -1)
                continue;

            ArenaPath uriPath(model.images[tex].uri, arena);

            // URI paths are relative to gltf file
            SmallVector<char, ArenaAllocator, 256> filename(arena);

            // resize for worst case
            filename.resize(uriPath.Length() + 5);

            // extract image file name
            size_t fnLen;
            uriPath.Stem(filename, &fnLen);

            // change extension to dds
            filename[fnLen] = '.';
            filename[fnLen + 1] = 'd';
            filename[fnLen + 2] = 'd';
            filename[fnLen + 3] = 's';
            filename[fnLen + 4] = '\0';

            ArenaPath ddsPath(compressedDir.GetView(), arena);
            ddsPath.Append(filename.data());

            if (forceOverwrite || !Filesystem::Exists(ddsPath.Get()))
            {
                Filesystem::Path imgPath(glTFPath.GetView());
                imgPath.Directory();
                imgPath.Append(model.images[tex].uri);

                // DirectXTex expects backslashes
                imgPath.ConvertToBackslashes();

                int x;
                int y;
                int comp;
                Check(stbi_info(imgPath.Get(), &x, &y, &comp), "stbi_info() for path %s failed: %s",
                    imgPath.Get(), stbi_failure_reason());

                int w = Min(x, maxRes);
                int h = Min(y, maxRes);

                // Direct3D requires BC image to be multiple of 4 in width & height
                w = (int)AlignUp(w, 4);
                h = (int)AlignUp(h, 4);

                // Returns length without the null terminatir
                const int len = stbsp_snprintf(nullptr, 0, formatStr, w, h, texFormat,
                    compressedDir.GetView().data(), imgPath.Get());
                // Now allocate a buffer large enough for the whole string plus
                // null terminator
                char* buffer = reinterpret_cast<char*>(arena.AllocateAligned(len + 1, 1));
                stbsp_snprintf(buffer, len + 1, formatStr, w, h, texFormat,
                    compressedDir.GetView().data(), imgPath.Get());

                int wideStrLen = Common::CharToWideStrLen(buffer);
                wchar_t* wideBuffer = reinterpret_cast<wchar_t*>(arena.AllocateAligned(wideStrLen));
                Common::CharToWideStr(buffer, MutableSpan(wideBuffer, wideStrLen));

                wchar_t* ptr = wideBuffer;
                wchar_t* args[MAX_NUM_ARGS];
                int currArg = 0;

                while (ptr != wideBuffer + wideStrLen)
                {
                    args[currArg] = ptr;

                    // spaces are valid for last argument (file path)
                    while ((currArg == numArgs - 1 || *ptr != ' ') && *ptr != '\0')
                        ptr++;

                    *ptr++ = '\0';
                    currArg++;
                }

                auto success = TexConv(numArgs, args, device);

                if (success != 0)
                {
                    printf("TexConv for path %s failed. Exiting...\n", model.images[tex].uri);
                    return false;
                }
            }
            else
                printf("Compressed texture already exists in the path %s. Skipping...\n", ddsPath.Get());

            // Modify URI to dds path. URI paths are relative to gltf file.
            ArenaPathNoInline ddsPathRelglTF(compressedDirName, arena);
            ddsPathRelglTF.Append(filename.data());
            ddsPathRelglTF.ConvertToForwardSlashes();

            model.images[tex].uri = ddsPathRelglTF.Get();
        }

        return true;
    }

    void WriteModifiedglTF(cgltf_data& model, const ArenaPath& gltfPath, MemoryArena& arena)
    {
        SmallVector<char, ArenaAllocator, 256> filename(arena);
        filename.resize(gltfPath.Length());

        size_t fnLen;
        gltfPath.Stem(filename, &fnLen);
        filename.resize(fnLen + 11);

        filename[fnLen] = '_';
        filename[fnLen + 1] = 'z';
        filename[fnLen + 2] = 'e';
        filename[fnLen + 3] = 't';
        filename[fnLen + 4] = 'a';
        filename[fnLen + 5] = '.';
        filename[fnLen + 6] = 'g';
        filename[fnLen + 7] = 'l';
        filename[fnLen + 8] = 't';
        filename[fnLen + 9] = 'f';
        filename[fnLen + 10] = '\0';

        Filesystem::Path convertedPath(gltfPath.GetView());
        convertedPath.Directory().Append(StrView(filename.data(), filename.size()));

        cgltf_options options = {};
        if (cgltf_write_file(&options, convertedPath.Get(), &model) != cgltf_result_success)
        {
            printf("Error writing modified glTF to path %s\n", convertedPath.Get());
            return;
        }

        printf("glTF scene file with modified image URIs has been written to %s...\n", convertedPath.Get());
    }

    ZetaInline void ReportUsageError()
    {
        printf("Usage: BCnCompressglTF <path-to-glTF> [options]\n\nOptions:\n%5s%30s\n%5s%30s\n%18s%23s\n", "-y", "Force overwrite", "-sv", "Skip validation", "-mr <resolution>", "Max output resolution");
    }
}

int main(int argc, char* argv[])
{
    if (argc < 2 || argc > 5)
    {
        ReportUsageError();
        return 0;
    }

    MemoryArena arena(64 * 1024);
    ArenaPath gltfPath(argv[1], arena);
    if (!Filesystem::Exists(gltfPath.Get()))
    {
        printf("No such file found in the path %s\nExiting...\n", gltfPath.Get());
        return 0;
    }

    bool forceOverwrite = false;
    bool validate = true;
    int maxRes = -1;

    for (int i = 2; i < argc; i++)
    {
        if (strcmp(argv[i], "-y") == 0)
            forceOverwrite = true;
        else if (strcmp(argv[i], "-sv") == 0)
            validate = false;
        else if (strcmp(argv[i], "-mr") == 0)
        {
            if (i == argc - 1)
            {
                ReportUsageError();
                return 0;
            }
            maxRes = atoi(argv[i + 1]);
            if (maxRes == 0)
            {
                ReportUsageError();
                return 0;
            }

            maxRes = Min(maxRes, DEFAULT_MAX_TEX_RES);
            i++;
        }
    }

    maxRes = maxRes == -1 ? DEFAULT_MAX_TEX_RES : maxRes;
    printf("Compressing textures for %s...\nMaximum output resolution set to %dx%d...\n",
        argv[1], maxRes, maxRes);

    cgltf_options options{};
    cgltf_data* model = nullptr;
    if (cgltf_parse_file(&options, gltfPath.Get(), &model) != cgltf_result_success)
    {
        printf("Error parsing glTF from path %s\n", gltfPath.Get());
        return 0;
    }

    if (model->images_count == 0)
    {
        printf("No images found. Exiting...");
        return 0;
    }

    DecodeImageURIs(gltfPath, *model, arena, validate);

    const size_t numMats = model->materials_count;

    SmallVector<int, ArenaAllocator> normalMaps(arena);
    normalMaps.reserve(numMats);

    // extract normal map texture indices
    for (size_t i = 0; i < numMats; i++)
    {
        if (cgltf_texture* tex = model->materials[i].normal_texture.texture; tex)
        {
            int imgIdx = (int)(tex->image - model->images);
            Assert(imgIdx < model->images_count, "Invalid image index.");
            normalMaps.push_back(imgIdx);
        }
    }

    SmallVector<int, Support::ArenaAllocator> baseColorMaps(arena);
    baseColorMaps.reserve(numMats);

    SmallVector<int, Support::ArenaAllocator> metalnessRoughnessMaps(arena);
    metalnessRoughnessMaps.reserve(numMats);

    SmallVector<int, Support::ArenaAllocator> emissiveMaps(arena);
    emissiveMaps.reserve(numMats);

    // extract pbr texture indices
    for (size_t i = 0; i < numMats; i++)
    {
        if (model->materials[i].has_pbr_metallic_roughness)
        {
            cgltf_pbr_metallic_roughness& pbr = model->materials[i].pbr_metallic_roughness;

            if (cgltf_texture* tex = pbr.base_color_texture.texture; tex)
            {
                int imgIdx = (int)(tex->image - model->images);
                Assert(imgIdx < model->images_count, "Invalid image index.");
                baseColorMaps.push_back(imgIdx);
            }

            if (cgltf_texture* tex = pbr.metallic_roughness_texture.texture; tex)
            {
                int imgIdx = (int)(tex->image - model->images);
                Assert(imgIdx < model->images_count, "Invalid image index.");
                metalnessRoughnessMaps.push_back(imgIdx);
            }
        }
    }

    // extract emissive map texture indices
    for (size_t i = 0; i < numMats; i++)
    {
        if (cgltf_texture* tex = model->materials[i].emissive_texture.texture; tex)
        {
            int imgIdx = (int)(tex->image - model->images);
            Assert(imgIdx < model->images_count, "Invalid image index.");
            emissiveMaps.push_back(imgIdx);
        }
    }

    SmallVector<int, Support::ArenaAllocator> skip(arena);

    if (validate)
    {
        bool isValid = true;
        SmallVector<int, Support::ArenaAllocator> intersections(arena);
        intersections.resize(model->textures_count, -1);

        std::sort(baseColorMaps.begin(), baseColorMaps.end());
        std::sort(normalMaps.begin(), normalMaps.end());
        std::sort(metalnessRoughnessMaps.begin(), metalnessRoughnessMaps.end());
        std::sort(emissiveMaps.begin(), emissiveMaps.end());

        auto checkIntersections = [model, &intersections, &isValid](Span<int> vec1,
            Span<int> vec2, const char* n1, const char* n2)
            {
                std::set_intersection(vec1.begin(), vec1.end(), vec2.begin(), vec2.end(), intersections.begin());

                for (int& i : intersections)
                {
                    if (i == -1)
                        break;

                    printf("WARNING: Following texture is used both as a(n) %s map and a(n) %s map:\n%s\n",
                        n1, n2, model->images[i].uri);

                    i = -1;
                    isValid = false;
                }
            };

        checkIntersections(baseColorMaps, normalMaps, "base-color", "normal");
        checkIntersections(baseColorMaps, metalnessRoughnessMaps, "base-color", "metalness-roughness");
        checkIntersections(baseColorMaps, emissiveMaps, "base-color", "emissive");
        checkIntersections(normalMaps, metalnessRoughnessMaps, "normal", "metalness-roughness");
        checkIntersections(normalMaps, emissiveMaps, "normal", "emissive");
        checkIntersections(metalnessRoughnessMaps, emissiveMaps, "metalness-roughness", "normal");

        if (!isValid)
        {
            printf("glTF validation failed. Exiting...\n");
            return 0;
        }

        const size_t allTexIndices = baseColorMaps.size() + normalMaps.size() +
            metalnessRoughnessMaps.size() + emissiveMaps.size();
        SmallVector<int, Support::ArenaAllocator> temp(arena);
        temp.resize(allTexIndices);
        skip.resize(allTexIndices);

        // Input and output can't overlap
        auto endIt = std::merge(baseColorMaps.begin(), baseColorMaps.end(), 
            normalMaps.begin(), normalMaps.end(),
            temp.begin());
        temp.resize(endIt - temp.begin());

        endIt = std::merge(temp.begin(), temp.end(),
            metalnessRoughnessMaps.begin(), metalnessRoughnessMaps.end(),
            skip.begin());
        skip.resize(endIt - skip.begin());

        temp.resize(skip.size() + emissiveMaps.size());
        endIt = std::merge(skip.begin(), skip.end(),
            emissiveMaps.begin(), emissiveMaps.end(),
            temp.begin());
        temp.resize(endIt - temp.begin());

        SmallVector<int, Support::ArenaAllocator> all(model->images_count, arena);
        for (size_t i = 0; i < model->images_count; i++)
            all[i] = (int)i;

        skip.resize(model->images_count);
        endIt = std::set_difference(all.begin(), all.end(), 
            temp.begin(), temp.end(),
            skip.begin());
        skip.resize(endIt - skip.begin());

        for (auto i : skip)
            printf("Image with URI %s is not referenced by any materials and will be skipped...\n", model->images[i].uri);
    }

    printf("Stats:\n\
        #images: %llu \n\
        #textures: %llu\n\
        #base-color textures: %llu\n\
        #normal-map textures: %llu\n\
        #metalness-roughness textures: %llu\n\
        #emissive textures: %llu\n", model->images_count, model->textures_count, baseColorMaps.size(),
        normalMaps.size(), metalnessRoughnessMaps.size(), emissiveMaps.size());

    ComPtr<ID3D11Device> device;
    CreateDevice(device.GetAddressOf());

    // Initialize COM (needed for WIC)
    auto hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Check(hr == S_OK, "CoInitializeEx() failed with code %x.", hr);

    ArenaPath compressedDir(gltfPath.Get(), arena);
    compressedDir.Directory().Append(COMPRESSED_DIR_NAME);
    Filesystem::CreateDirectoryIfNotExists(compressedDir.Get());

    if (!ConvertTextures(TEXTURE_TYPE::BASE_COLOR, gltfPath, compressedDir, COMPRESSED_DIR_NAME,
        *model, baseColorMaps, arena, device.Get(), true, forceOverwrite, maxRes, skip))
    {
        return 0;
    }
    if (!ConvertTextures(TEXTURE_TYPE::NORMAL_MAP, gltfPath, compressedDir, COMPRESSED_DIR_NAME,
        *model, normalMaps, arena, device.Get(), false, forceOverwrite, maxRes, skip))
    {
        return 0;
    }
    if (!ConvertTextures(TEXTURE_TYPE::METALNESS_ROUGHNESS, gltfPath, compressedDir, COMPRESSED_DIR_NAME,
        *model, metalnessRoughnessMaps, arena, device.Get(), false, forceOverwrite, maxRes, skip))
    {
        return 0;
    }
    if (!ConvertTextures(TEXTURE_TYPE::EMISSIVE, gltfPath, compressedDir, COMPRESSED_DIR_NAME,
        *model, emissiveMaps, arena, device.Get(), true, forceOverwrite, maxRes, skip))
    {
        return 0;
    }

    WriteModifiedglTF(*model, gltfPath, arena);

    return 0;
}