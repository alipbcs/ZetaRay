#include "Direct3DHelpers.h"
#include "dds.h"
#include "Renderer.h"
#include <xxHash/xxhash.h>
#include <memory>

using namespace ZetaRay;
using namespace ZetaRay::Util;
using namespace ZetaRay::Core;
using namespace ZetaRay::Core::Direct3DHelper;

#define ISBITMASK( r,g,b,a ) ( ddpf.RBitMask == r && ddpf.GBitMask == g && ddpf.BBitMask == b && ddpf.ABitMask == a )

namespace
{
    void AdjustPlaneResource(DXGI_FORMAT fmt,
        size_t height,
        size_t slicePlane,
        D3D12_SUBRESOURCE_DATA& res) noexcept
    {
        switch (fmt)
        {
        case DXGI_FORMAT_NV12:
        case DXGI_FORMAT_P010:
        case DXGI_FORMAT_P016:
            if (!slicePlane)
            {
                // Plane 0
                res.SlicePitch = res.RowPitch * height;
            }
            else
            {
                // Plane 1
                res.pData = static_cast<const uint8_t*>(res.pData) + res.RowPitch * height;
                res.SlicePitch = res.RowPitch * ((height + 1) >> 1);
            }
            break;

        case DXGI_FORMAT_NV11:
            if (!slicePlane)
            {
                // Plane 0
                res.SlicePitch = res.RowPitch * height;
            }
            else
            {
                // Plane 1
                res.pData = static_cast<const uint8_t*>(res.pData) + res.RowPitch * height;
                res.RowPitch = (res.RowPitch >> 1);
                res.SlicePitch = res.RowPitch * height;
            }
            break;

        default:
            break;
        }
    }

    bool IsDepthStencil(DXGI_FORMAT fmt) noexcept
    {
        switch (fmt)
        {
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
        case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
        case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
        case DXGI_FORMAT_D16_UNORM:
            return true;

        default:
            return false;
        }
    }

    UINT8 D3D12GetFormatPlaneCount(ID3D12Device* pDevice, DXGI_FORMAT Format) noexcept
    {
        D3D12_FEATURE_DATA_FORMAT_INFO formatInfo = { Format, 0 };
        if (FAILED(pDevice->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &formatInfo, sizeof(formatInfo))))
            return 0;

        return formatInfo.PlaneCount;
    }

    DXGI_FORMAT GetDXGIFormat(const DDS_PIXELFORMAT& ddpf) noexcept
    {
        if (ddpf.flags & DDS_RGB)
        {
            // Note that sRGB formats are written using the "DX10" extended header

            switch (ddpf.RGBBitCount)
            {
            case 32:
                if (ISBITMASK(0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000))
                {
                    return DXGI_FORMAT_R8G8B8A8_UNORM;
                }

                if (ISBITMASK(0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000))
                {
                    return DXGI_FORMAT_B8G8R8A8_UNORM;
                }

                if (ISBITMASK(0x00ff0000, 0x0000ff00, 0x000000ff, 0x00000000))
                {
                    return DXGI_FORMAT_B8G8R8X8_UNORM;
                }

                // No DXGI format maps to ISBITMASK(0x000000ff,0x0000ff00,0x00ff0000,0x00000000) aka D3DFMT_X8B8G8R8

                // Note that many common DDS reader/writers (including D3DX) swap the
                // the RED/BLUE masks for 10:10:10:2 formats. We assume
                // below that the 'backwards' header mask is being used since it is most
                // likely written by D3DX. The more robust solution is to use the 'DX10'
                // header extension and specify the DXGI_FORMAT_R10G10B10A2_UNORM format directly

                // For 'correct' writers, this should be 0x000003ff,0x000ffc00,0x3ff00000 for RGB data
                if (ISBITMASK(0x3ff00000, 0x000ffc00, 0x000003ff, 0xc0000000))
                {
                    return DXGI_FORMAT_R10G10B10A2_UNORM;
                }

                // No DXGI format maps to ISBITMASK(0x000003ff,0x000ffc00,0x3ff00000,0xc0000000) aka D3DFMT_A2R10G10B10

                if (ISBITMASK(0x0000ffff, 0xffff0000, 0x00000000, 0x00000000))
                {
                    return DXGI_FORMAT_R16G16_UNORM;
                }

                if (ISBITMASK(0xffffffff, 0x00000000, 0x00000000, 0x00000000))
                {
                    // Only 32-bit color channel format in D3D9 was R32F
                    return DXGI_FORMAT_R32_FLOAT; // D3DX writes this out as a FourCC of 114
                }
                break;

            case 24:
                // No 24bpp DXGI formats aka D3DFMT_R8G8B8
                break;

            case 16:
                if (ISBITMASK(0x7c00, 0x03e0, 0x001f, 0x8000))
                {
                    return DXGI_FORMAT_B5G5R5A1_UNORM;
                }
                if (ISBITMASK(0xf800, 0x07e0, 0x001f, 0x0000))
                {
                    return DXGI_FORMAT_B5G6R5_UNORM;
                }

                // No DXGI format maps to ISBITMASK(0x7c00,0x03e0,0x001f,0x0000) aka D3DFMT_X1R5G5B5

                if (ISBITMASK(0x0f00, 0x00f0, 0x000f, 0xf000))
                {
                    return DXGI_FORMAT_B4G4R4A4_UNORM;
                }

                // No DXGI format maps to ISBITMASK(0x0f00,0x00f0,0x000f,0x0000) aka D3DFMT_X4R4G4B4

                // No 3:3:2, 3:3:2:8, or paletted DXGI formats aka D3DFMT_A8R3G3B2, D3DFMT_R3G3B2, D3DFMT_P8, D3DFMT_A8P8, etc.
                break;
            }
        }
        else if (ddpf.flags & DDS_LUMINANCE)
        {
            if (8 == ddpf.RGBBitCount)
            {
                if (ISBITMASK(0x000000ff, 0x00000000, 0x00000000, 0x00000000))
                {
                    return DXGI_FORMAT_R8_UNORM; // D3DX10/11 writes this out as DX10 extension
                }

                // No DXGI format maps to ISBITMASK(0x0f,0x00,0x00,0xf0) aka D3DFMT_A4L4

                if (ISBITMASK(0x000000ff, 0x00000000, 0x00000000, 0x0000ff00))
                {
                    return DXGI_FORMAT_R8G8_UNORM; // Some DDS writers assume the bitcount should be 8 instead of 16
                }
            }

            if (16 == ddpf.RGBBitCount)
            {
                if (ISBITMASK(0x0000ffff, 0x00000000, 0x00000000, 0x00000000))
                {
                    return DXGI_FORMAT_R16_UNORM; // D3DX10/11 writes this out as DX10 extension
                }
                if (ISBITMASK(0x000000ff, 0x00000000, 0x00000000, 0x0000ff00))
                {
                    return DXGI_FORMAT_R8G8_UNORM; // D3DX10/11 writes this out as DX10 extension
                }
            }
        }
        else if (ddpf.flags & DDS_ALPHA)
        {
            if (8 == ddpf.RGBBitCount)
            {
                return DXGI_FORMAT_A8_UNORM;
            }
        }
        else if (ddpf.flags & DDS_BUMPDUDV)
        {
            if (16 == ddpf.RGBBitCount)
            {
                if (ISBITMASK(0x00ff, 0xff00, 0x0000, 0x0000))
                {
                    return DXGI_FORMAT_R8G8_SNORM; // D3DX10/11 writes this out as DX10 extension
                }
            }

            if (32 == ddpf.RGBBitCount)
            {
                if (ISBITMASK(0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000))
                {
                    return DXGI_FORMAT_R8G8B8A8_SNORM; // D3DX10/11 writes this out as DX10 extension
                }
                if (ISBITMASK(0x0000ffff, 0xffff0000, 0x00000000, 0x00000000))
                {
                    return DXGI_FORMAT_R16G16_SNORM; // D3DX10/11 writes this out as DX10 extension
                }

                // No DXGI format maps to ISBITMASK(0x3ff00000, 0x000ffc00, 0x000003ff, 0xc0000000) aka D3DFMT_A2W10V10U10
            }
        }
        else if (ddpf.flags & DDS_FOURCC)
        {
            if (MAKEFOURCC('D', 'X', 'T', '1') == ddpf.fourCC)
            {
                return DXGI_FORMAT_BC1_UNORM;
            }
            if (MAKEFOURCC('D', 'X', 'T', '3') == ddpf.fourCC)
            {
                return DXGI_FORMAT_BC2_UNORM;
            }
            if (MAKEFOURCC('D', 'X', 'T', '5') == ddpf.fourCC)
            {
                return DXGI_FORMAT_BC3_UNORM;
            }

            // While pre-multiplied alpha isn't directly supported by the DXGI formats,
            // they are basically the same as these BC formats so they can be mapped
            if (MAKEFOURCC('D', 'X', 'T', '2') == ddpf.fourCC)
            {
                return DXGI_FORMAT_BC2_UNORM;
            }
            if (MAKEFOURCC('D', 'X', 'T', '4') == ddpf.fourCC)
            {
                return DXGI_FORMAT_BC3_UNORM;
            }

            if (MAKEFOURCC('A', 'T', 'I', '1') == ddpf.fourCC)
            {
                return DXGI_FORMAT_BC4_UNORM;
            }
            if (MAKEFOURCC('B', 'C', '4', 'U') == ddpf.fourCC)
            {
                return DXGI_FORMAT_BC4_UNORM;
            }
            if (MAKEFOURCC('B', 'C', '4', 'S') == ddpf.fourCC)
            {
                return DXGI_FORMAT_BC4_SNORM;
            }

            if (MAKEFOURCC('A', 'T', 'I', '2') == ddpf.fourCC)
            {
                return DXGI_FORMAT_BC5_UNORM;
            }
            if (MAKEFOURCC('B', 'C', '5', 'U') == ddpf.fourCC)
            {
                return DXGI_FORMAT_BC5_UNORM;
            }
            if (MAKEFOURCC('B', 'C', '5', 'S') == ddpf.fourCC)
            {
                return DXGI_FORMAT_BC5_SNORM;
            }

            // BC6H and BC7 are written using the "DX10" extended header

            if (MAKEFOURCC('R', 'G', 'B', 'G') == ddpf.fourCC)
            {
                return DXGI_FORMAT_R8G8_B8G8_UNORM;
            }
            if (MAKEFOURCC('G', 'R', 'G', 'B') == ddpf.fourCC)
            {
                return DXGI_FORMAT_G8R8_G8B8_UNORM;
            }

            if (MAKEFOURCC('Y', 'U', 'Y', '2') == ddpf.fourCC)
            {
                return DXGI_FORMAT_YUY2;
            }

            // Check for D3DFORMAT enums being set here
            switch (ddpf.fourCC)
            {
            case 36: // D3DFMT_A16B16G16R16
                return DXGI_FORMAT_R16G16B16A16_UNORM;

            case 110: // D3DFMT_Q16W16V16U16
                return DXGI_FORMAT_R16G16B16A16_SNORM;

            case 111: // D3DFMT_R16F
                return DXGI_FORMAT_R16_FLOAT;

            case 112: // D3DFMT_G16R16F
                return DXGI_FORMAT_R16G16_FLOAT;

            case 113: // D3DFMT_A16B16G16R16F
                return DXGI_FORMAT_R16G16B16A16_FLOAT;

            case 114: // D3DFMT_R32F
                return DXGI_FORMAT_R32_FLOAT;

            case 115: // D3DFMT_G32R32F
                return DXGI_FORMAT_R32G32_FLOAT;

            case 116: // D3DFMT_A32B32G32R32F
                return DXGI_FORMAT_R32G32B32A32_FLOAT;
            }
        }

        return DXGI_FORMAT_UNKNOWN;
    }

    HRESULT FillInitData(size_t width, size_t height, size_t depth, size_t mipCount,
        size_t arraySize, size_t numberOfPlanes, DXGI_FORMAT format, size_t maxsize, size_t bitSize,
        const uint8_t* bitData, size_t& twidth, size_t& theight, size_t& tdepth, size_t& skipMip,
        Vector<D3D12_SUBRESOURCE_DATA, App::ThreadAllocator>& initData) noexcept
    {
        skipMip = 0;
        twidth = 0;
        theight = 0;
        tdepth = 0;

        size_t NumBytes = 0;
        size_t RowBytes = 0;
        const uint8_t* pEndBits = bitData + bitSize;

        for (size_t p = 0; p < numberOfPlanes; ++p)
        {
            const uint8_t* pSrcBits = bitData;

            for (size_t j = 0; j < arraySize; j++)
            {
                size_t w = width;
                size_t h = height;
                size_t d = depth;
                for (size_t i = 0; i < mipCount; i++)
                {
                    HRESULT hr = GetSurfaceInfo(w, h, format, &NumBytes, &RowBytes, nullptr);
                    if (FAILED(hr))
                        return hr;

                    if (NumBytes > UINT32_MAX || RowBytes > UINT32_MAX)
                        return HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);

                    if ((mipCount <= 1) || !maxsize || (w <= maxsize && h <= maxsize && d <= maxsize))
                    {
                        if (!twidth)
                        {
                            twidth = w;
                            theight = h;
                            tdepth = d;
                        }

                        D3D12_SUBRESOURCE_DATA res =
                        {
                            pSrcBits,
                            static_cast<LONG_PTR>(RowBytes),
                            static_cast<LONG_PTR>(NumBytes)
                        };

                        AdjustPlaneResource(format, h, p, res);

                        initData.emplace_back(res);
                    }
                    else if (!j)
                    {
                        // Count number of skipped mipmaps (first item only)
                        ++skipMip;
                    }

                    if (pSrcBits + (NumBytes * d) > pEndBits)
                    {
                        return HRESULT_FROM_WIN32(ERROR_HANDLE_EOF);
                    }

                    pSrcBits += NumBytes * d;

                    w = w >> 1;
                    h = h >> 1;
                    d = d >> 1;
                    if (w == 0)
                    {
                        w = 1;
                    }
                    if (h == 0)
                    {
                        h = 1;
                    }
                    if (d == 0)
                    {
                        d = 1;
                    }
                }
            }
        }

        return initData.empty() ? E_FAIL : S_OK;
    }

    void FillSubresourceData(const DDS_HEADER* header, Vector<D3D12_SUBRESOURCE_DATA, App::ThreadAllocator>& subresources, const uint8_t* bitData, 
        size_t bitSize, uint32_t& width, uint32_t& height, uint32_t& depth, uint32_t& mipCount, DXGI_FORMAT& format) noexcept
    {
        auto* device = App::GetRenderer().GetDevice();

        width = header->width;
        height = header->height;
        depth = header->depth;

        mipCount = header->mipMapCount;
        if (0 == mipCount)
        {
            mipCount = 1;
        }

        // Bound sizes (for security purposes we don't trust DDS file metadata larger than the Direct3D hardware requirements)
        Check(mipCount <= D3D12_REQ_MIP_LEVELS, "Not supported");

        D3D12_RESOURCE_DIMENSION resDim = D3D12_RESOURCE_DIMENSION_UNKNOWN;
        UINT arraySize = 1;
        format = DXGI_FORMAT_UNKNOWN;
        bool isCubeMap = false;

        if ((header->ddspf.flags & DDS_FOURCC) && (MAKEFOURCC('D', 'X', '1', '0') == header->ddspf.fourCC))
        {
            auto d3d10ext = reinterpret_cast<const DDS_HEADER_DXT10*>(reinterpret_cast<const char*>(header) +
                sizeof(DDS_HEADER));

            arraySize = d3d10ext->arraySize;
            Check(arraySize != 0, "Invalid Data");

            switch (d3d10ext->dxgiFormat)
            {
            case DXGI_FORMAT_AI44:
            case DXGI_FORMAT_IA44:
            case DXGI_FORMAT_P8:
            case DXGI_FORMAT_A8P8:
                Check(false, "DDSTextureLoader does not support video textures. Consider using DirectXTex instead");

            default:
                Check(BitsPerPixel(d3d10ext->dxgiFormat) != 0, 
                    "Unknown DXGI format %u", static_cast<uint32_t>(d3d10ext->dxgiFormat));              
            }

            format = d3d10ext->dxgiFormat;

            switch (d3d10ext->resourceDimension)
            {
            case D3D12_RESOURCE_DIMENSION_TEXTURE1D:
                // D3DX writes 1D textures with a fixed Height of 1
                Check(!(header->flags & DDS_HEIGHT) || height == 1, "Invalid data");

                height = depth = 1;
                break;

            case D3D12_RESOURCE_DIMENSION_TEXTURE2D:
                if (d3d10ext->miscFlag & 0x4 /* RESOURCE_MISC_TEXTURECUBE */)
                {
                    arraySize *= 6;
                    isCubeMap = true;
                }
                depth = 1;
                break;

            case D3D12_RESOURCE_DIMENSION_TEXTURE3D:
                Check(header->flags & DDS_HEADER_FLAGS_VOLUME, "Invalid data");
                Check(arraySize <= 1, "Not supported");

                break;

            default:
                Check(false, "Not supported");
            }

            resDim = static_cast<D3D12_RESOURCE_DIMENSION>(d3d10ext->resourceDimension);
        }
        else
        {
            format = GetDXGIFormat(header->ddspf);
            Check(format != DXGI_FORMAT_UNKNOWN, "Not supported");

            if (header->flags & DDS_HEADER_FLAGS_VOLUME)
            {
                resDim = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
            }
            else
            {
                if (header->caps2 & DDS_CUBEMAP)
                {
                    // We require all six faces to be defined
                    Check((header->caps2 & DDS_CUBEMAP_ALLFACES) == DDS_CUBEMAP_ALLFACES, "Not supported");

                    arraySize = 6;
                    isCubeMap = true;
                }

                depth = 1;
                resDim = D3D12_RESOURCE_DIMENSION_TEXTURE2D;

                // Note there's no way for a legacy Direct3D 9 DDS to express a '1D' texture
            }

            Assert(BitsPerPixel(format) != 0, "");
        }

        switch (resDim)
        {
        case D3D12_RESOURCE_DIMENSION_TEXTURE1D:
            Check(arraySize <= D3D12_REQ_TEXTURE1D_ARRAY_AXIS_DIMENSION &&
                width <= D3D12_REQ_TEXTURE1D_U_DIMENSION, "Not supported");
            break;

        case D3D12_RESOURCE_DIMENSION_TEXTURE2D:
            if (isCubeMap)
            {
                // This is the right bound because we set arraySize to (NumCubes*6) above
                Check(arraySize <= D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION &&
                    width <= D3D12_REQ_TEXTURECUBE_DIMENSION &&
                    height <= D3D12_REQ_TEXTURECUBE_DIMENSION,
                    "Not supported");
            }
            else
                Check(arraySize <= D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION &&
                    width <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION &&
                    height <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION,
                    "Not supported");

            break;

        case D3D12_RESOURCE_DIMENSION_TEXTURE3D:
            Check(arraySize <= 1 &&
                width <= D3D12_REQ_TEXTURE3D_U_V_OR_W_DIMENSION &&
                height <= D3D12_REQ_TEXTURE3D_U_V_OR_W_DIMENSION &&
                depth <= D3D12_REQ_TEXTURE3D_U_V_OR_W_DIMENSION,
                "Not supported");
            break;

        default:
            Check(false, "Not supported");
        }

        UINT numberOfPlanes = D3D12GetFormatPlaneCount(device, format);
        Check(numberOfPlanes, "Invalid arg");

        // DirectX 12 uses planes for stencil, DirectX 11 does not
        Check(numberOfPlanes <= 1 || !IsDepthStencil(format), "Invalid arg");

        //if (outIsCubeMap != nullptr)
        //{
        //	*outIsCubeMap = isCubeMap;
        //}

        // Create the texture
        size_t numberOfResources = (resDim == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
            ? 1 : arraySize;
        numberOfResources *= mipCount;
        numberOfResources *= numberOfPlanes;

        Check(numberOfResources <= D3D12_REQ_SUBRESOURCES, "Invalid arg");

        subresources.reserve(numberOfResources);

        size_t skipMip = 0;
        size_t twidth = 0;
        size_t theight = 0;
        size_t tdepth = 0;
        size_t maxsize = 0;

        CheckHR(FillInitData(width, height, depth, mipCount, arraySize,
            numberOfPlanes, format, maxsize, bitSize, bitData,
            twidth, theight, tdepth, skipMip, subresources));
    }

    HRESULT LoadTextureDataFromFile(const char* fileName, std::unique_ptr<uint8_t[]>& ddsData, const DDS_HEADER** header,
        const uint8_t** bitData, size_t* bitSize) noexcept
    {
        if (!header || !bitData || !bitSize)
        {
            return E_POINTER;
        }

        // open the file
        HANDLE hFile = CreateFileA(fileName,
            GENERIC_READ,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);

        CheckWin32(hFile != INVALID_HANDLE_VALUE);

        if (hFile == INVALID_HANDLE_VALUE)
        {
            return HRESULT_FROM_WIN32(GetLastError());
        }

        // Get the file size
        FILE_STANDARD_INFO fileInfo;
        if (!GetFileInformationByHandleEx(hFile, FileStandardInfo, &fileInfo, sizeof(fileInfo)))
        {
            CloseHandle(hFile);
            return HRESULT_FROM_WIN32(GetLastError());
        }

        // File is too big for 32-bit allocation, so reject read
        if (fileInfo.EndOfFile.HighPart > 0)
        {
            CloseHandle(hFile);
            return E_FAIL;
        }

        // Need at least enough data to fill the header and magic number to be a valid DDS
        if (fileInfo.EndOfFile.LowPart < (sizeof(DDS_HEADER) + sizeof(uint32_t)))
        {
            CloseHandle(hFile);
            return E_FAIL;
        }

        // create enough space for the file data
        ddsData.reset(new (std::nothrow) uint8_t[fileInfo.EndOfFile.LowPart]);
        if (!ddsData)
        {
            CloseHandle(hFile);
            return E_OUTOFMEMORY;
        }

        // read the data in
        DWORD BytesRead = 0;
        if (!ReadFile(hFile, ddsData.get(), fileInfo.EndOfFile.LowPart, &BytesRead, nullptr))
        {
            CloseHandle(hFile);
            return HRESULT_FROM_WIN32(GetLastError());
        }

        if (BytesRead < fileInfo.EndOfFile.LowPart)
        {
            CloseHandle(hFile);
            return E_FAIL;
        }

        // DDS files always start with the same magic number ("DDS ")
        uint32_t dwMagicNumber = *reinterpret_cast<const uint32_t*>(ddsData.get());
        if (dwMagicNumber != DDS_MAGIC)
        {
            CloseHandle(hFile);
            return E_FAIL;
        }

        auto hdr = reinterpret_cast<const DDS_HEADER*>(ddsData.get() + sizeof(uint32_t));

        // Verify header to validate DDS file
        if (hdr->size != sizeof(DDS_HEADER) || hdr->ddspf.size != sizeof(DDS_PIXELFORMAT))
        {
            CloseHandle(hFile);
            return E_FAIL;
        }

        // Check for DX10 extension
        bool bDXT10Header = false;
        if ((hdr->ddspf.flags & DDS_FOURCC) &&
            (MAKEFOURCC('D', 'X', '1', '0') == hdr->ddspf.fourCC))
        {
            // Must be long enough for both headers and magic value
            if (fileInfo.EndOfFile.LowPart < (sizeof(DDS_HEADER) + sizeof(uint32_t) + sizeof(DDS_HEADER_DXT10)))
            {
                CloseHandle(hFile);
                return E_FAIL;
            }

            bDXT10Header = true;
        }

        // setup the pointers in the process request
        *header = hdr;
        ptrdiff_t offset = sizeof(uint32_t) + sizeof(DDS_HEADER) + (bDXT10Header ? sizeof(DDS_HEADER_DXT10) : 0);
        *bitData = ddsData.get() + offset;
        *bitSize = fileInfo.EndOfFile.LowPart - offset;

        CloseHandle(hFile);

        return S_OK;
    }
}

//--------------------------------------------------------------------------------------
// Direct3DHelper
//--------------------------------------------------------------------------------------

size_t Direct3DHelper::BitsPerPixel(DXGI_FORMAT fmt) noexcept
{
    switch (fmt)
    {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_UINT:
    case DXGI_FORMAT_R32G32B32A32_SINT:
        return 128;

    case DXGI_FORMAT_R32G32B32_TYPELESS:
    case DXGI_FORMAT_R32G32B32_FLOAT:
    case DXGI_FORMAT_R32G32B32_UINT:
    case DXGI_FORMAT_R32G32B32_SINT:
        return 96;

    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_UINT:
    case DXGI_FORMAT_R16G16B16A16_SNORM:
    case DXGI_FORMAT_R16G16B16A16_SINT:
    case DXGI_FORMAT_R32G32_TYPELESS:
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R32G32_UINT:
    case DXGI_FORMAT_R32G32_SINT:
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
    case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
    case DXGI_FORMAT_Y416:
    case DXGI_FORMAT_Y210:
    case DXGI_FORMAT_Y216:
        return 64;

    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UINT:
    case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_UINT:
    case DXGI_FORMAT_R8G8B8A8_SNORM:
    case DXGI_FORMAT_R8G8B8A8_SINT:
    case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R16G16_UINT:
    case DXGI_FORMAT_R16G16_SNORM:
    case DXGI_FORMAT_R16G16_SINT:
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_UINT:
    case DXGI_FORMAT_R32_SINT:
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
    case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
    case DXGI_FORMAT_R9G9B9E5_SHAREDEXP:
    case DXGI_FORMAT_R8G8_B8G8_UNORM:
    case DXGI_FORMAT_G8R8_G8B8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
    case DXGI_FORMAT_AYUV:
    case DXGI_FORMAT_Y410:
    case DXGI_FORMAT_YUY2:
        return 32;

    case DXGI_FORMAT_P010:
    case DXGI_FORMAT_P016:
        return 24;

    case DXGI_FORMAT_R8G8_TYPELESS:
    case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_R8G8_UINT:
    case DXGI_FORMAT_R8G8_SNORM:
    case DXGI_FORMAT_R8G8_SINT:
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16_UINT:
    case DXGI_FORMAT_R16_SNORM:
    case DXGI_FORMAT_R16_SINT:
    case DXGI_FORMAT_B5G6R5_UNORM:
    case DXGI_FORMAT_B5G5R5A1_UNORM:
    case DXGI_FORMAT_A8P8:
    case DXGI_FORMAT_B4G4R4A4_UNORM:
        return 16;

    case DXGI_FORMAT_NV12:
    case DXGI_FORMAT_420_OPAQUE:
    case DXGI_FORMAT_NV11:
        return 12;

    case DXGI_FORMAT_R8_TYPELESS:
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_R8_UINT:
    case DXGI_FORMAT_R8_SNORM:
    case DXGI_FORMAT_R8_SINT:
    case DXGI_FORMAT_A8_UNORM:
    case DXGI_FORMAT_AI44:
    case DXGI_FORMAT_IA44:
    case DXGI_FORMAT_P8:
        return 8;

    case DXGI_FORMAT_R1_UNORM:
        return 1;

    case DXGI_FORMAT_BC1_TYPELESS:
    case DXGI_FORMAT_BC1_UNORM:
    case DXGI_FORMAT_BC1_UNORM_SRGB:
    case DXGI_FORMAT_BC4_TYPELESS:
    case DXGI_FORMAT_BC4_UNORM:
    case DXGI_FORMAT_BC4_SNORM:
        return 4;

    case DXGI_FORMAT_BC2_TYPELESS:
    case DXGI_FORMAT_BC2_UNORM:
    case DXGI_FORMAT_BC2_UNORM_SRGB:
    case DXGI_FORMAT_BC3_TYPELESS:
    case DXGI_FORMAT_BC3_UNORM:
    case DXGI_FORMAT_BC3_UNORM_SRGB:
    case DXGI_FORMAT_BC5_TYPELESS:
    case DXGI_FORMAT_BC5_UNORM:
    case DXGI_FORMAT_BC5_SNORM:
    case DXGI_FORMAT_BC6H_TYPELESS:
    case DXGI_FORMAT_BC6H_UF16:
    case DXGI_FORMAT_BC6H_SF16:
    case DXGI_FORMAT_BC7_TYPELESS:
    case DXGI_FORMAT_BC7_UNORM:
    case DXGI_FORMAT_BC7_UNORM_SRGB:
        return 8;

#if (_WIN32_WINNT >= _WIN32_WINNT_WIN10)

    case DXGI_FORMAT_V408:
        return 24;

    case DXGI_FORMAT_P208:
    case DXGI_FORMAT_V208:
        return 16;

#endif // (_WIN32_WINNT >= _WIN32_WINNT_WIN10)

    default:
        return 0;
    }
}

HRESULT Direct3DHelper::GetSurfaceInfo(size_t width,
    size_t height,
    DXGI_FORMAT fmt,
    size_t* outNumBytes,
    size_t* outRowBytes,
    size_t* outNumRows) noexcept
{
    uint64_t numBytes = 0;
    uint64_t rowBytes = 0;
    uint64_t numRows = 0;

    bool bc = false;
    bool packed = false;
    bool planar = false;
    size_t bpe = 0;

    switch (fmt)
    {
    case DXGI_FORMAT_BC1_TYPELESS:
    case DXGI_FORMAT_BC1_UNORM:
    case DXGI_FORMAT_BC1_UNORM_SRGB:
    case DXGI_FORMAT_BC4_TYPELESS:
    case DXGI_FORMAT_BC4_UNORM:
    case DXGI_FORMAT_BC4_SNORM:
        bc = true;
        bpe = 8;
        break;

    case DXGI_FORMAT_BC2_TYPELESS:
    case DXGI_FORMAT_BC2_UNORM:
    case DXGI_FORMAT_BC2_UNORM_SRGB:
    case DXGI_FORMAT_BC3_TYPELESS:
    case DXGI_FORMAT_BC3_UNORM:
    case DXGI_FORMAT_BC3_UNORM_SRGB:
    case DXGI_FORMAT_BC5_TYPELESS:
    case DXGI_FORMAT_BC5_UNORM:
    case DXGI_FORMAT_BC5_SNORM:
    case DXGI_FORMAT_BC6H_TYPELESS:
    case DXGI_FORMAT_BC6H_UF16:
    case DXGI_FORMAT_BC6H_SF16:
    case DXGI_FORMAT_BC7_TYPELESS:
    case DXGI_FORMAT_BC7_UNORM:
    case DXGI_FORMAT_BC7_UNORM_SRGB:
        bc = true;
        bpe = 16;
        break;

    case DXGI_FORMAT_R8G8_B8G8_UNORM:
    case DXGI_FORMAT_G8R8_G8B8_UNORM:
    case DXGI_FORMAT_YUY2:
        packed = true;
        bpe = 4;
        break;

    case DXGI_FORMAT_Y210:
    case DXGI_FORMAT_Y216:
        packed = true;
        bpe = 8;
        break;

    case DXGI_FORMAT_NV12:
    case DXGI_FORMAT_420_OPAQUE:
#if (_WIN32_WINNT >= _WIN32_WINNT_WIN10)
    case DXGI_FORMAT_P208:
#endif
        planar = true;
        bpe = 2;
        break;

    case DXGI_FORMAT_P010:
    case DXGI_FORMAT_P016:
        planar = true;
        bpe = 4;
        break;

    default:
        break;
    }

    if (bc)
    {
        uint64_t numBlocksWide = 0;
        if (width > 0)
        {
            numBlocksWide = std::max<uint64_t>(1u, (uint64_t(width) + 3u) / 4u);
        }
        uint64_t numBlocksHigh = 0;
        if (height > 0)
        {
            numBlocksHigh = std::max<uint64_t>(1u, (uint64_t(height) + 3u) / 4u);
        }
        rowBytes = numBlocksWide * bpe;
        numRows = numBlocksHigh;
        numBytes = rowBytes * numBlocksHigh;
    }
    else if (packed)
    {
        rowBytes = ((uint64_t(width) + 1u) >> 1) * bpe;
        numRows = uint64_t(height);
        numBytes = rowBytes * height;
    }
    else if (fmt == DXGI_FORMAT_NV11)
    {
        rowBytes = ((uint64_t(width) + 3u) >> 2) * 4u;
        numRows = uint64_t(height) * 2u; // Direct3D makes this simplifying assumption, although it is larger than the 4:1:1 data
        numBytes = rowBytes * numRows;
    }
    else if (planar)
    {
        rowBytes = ((uint64_t(width) + 1u) >> 1) * bpe;
        numBytes = (rowBytes * uint64_t(height)) + ((rowBytes * uint64_t(height) + 1u) >> 1);
        numRows = height + ((uint64_t(height) + 1u) >> 1);
    }
    else
    {
        size_t bpp = BitsPerPixel(fmt);
        if (!bpp)
            return E_INVALIDARG;

        rowBytes = (uint64_t(width) * bpp + 7u) / 8u; // round up to nearest byte
        numRows = uint64_t(height);
        numBytes = rowBytes * height;
    }

    static_assert(sizeof(size_t) == 8, "Not a 64-bit platform!");

    if (outNumBytes)
    {
        *outNumBytes = static_cast<size_t>(numBytes);
    }
    if (outRowBytes)
    {
        *outRowBytes = static_cast<size_t>(rowBytes);
    }
    if (outNumRows)
    {
        *outNumRows = static_cast<size_t>(numRows);
    }

    return S_OK;
}

void Direct3DHelper::LoadDDSFromFile(const char* path,
    Vector<D3D12_SUBRESOURCE_DATA, App::ThreadAllocator>& subresources,
    DXGI_FORMAT& format, 
    std::unique_ptr<uint8_t[]>& ddsData,
    uint32_t& width, 
    uint32_t& height, 
    uint32_t& depth, 
    uint32_t& mipCount) noexcept
{
    // ddsData is populated with the file contents
    // bitData offsets into ddsData where the data starts

    const DDS_HEADER* header = nullptr;
    const uint8_t* bitData = nullptr;
    size_t bitSize = 0;

    CheckHR(LoadTextureDataFromFile(path, ddsData, &header, &bitData, &bitSize));
    FillSubresourceData(header, subresources, bitData, bitSize, width, height, depth, mipCount, format);
}

D3D12_GRAPHICS_PIPELINE_STATE_DESC Direct3DHelper::GetPSODesc(const D3D12_INPUT_LAYOUT_DESC* inputLayout,
    int numRenderTargets, 
    DXGI_FORMAT* rtvFormats, 
    DXGI_FORMAT dsvFormat, 
    D3D12_RASTERIZER_DESC* rasterizerDesc, 
    D3D12_BLEND_DESC* blendDesc, 
    D3D12_DEPTH_STENCIL_DESC* depthStencilDesc, 
    D3D12_PRIMITIVE_TOPOLOGY_TYPE primitiveTopology) noexcept
{
    //DXGI_FORMAT unknownFormats[D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT] = { DXGI_FORMAT_UNKNOWN };

    Assert(numRenderTargets <= D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT,
        "Invalid number of render targets.");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.BlendState = blendDesc ? *blendDesc : DefaultBlendDesc();
    psoDesc.RasterizerState = rasterizerDesc ? *rasterizerDesc : DefaultRasterizerDesc();
    psoDesc.DepthStencilState = depthStencilDesc ? *depthStencilDesc : DefaultDepthStencilDesc();
    psoDesc.NumRenderTargets = numRenderTargets;
    memcpy(psoDesc.RTVFormats, rtvFormats, sizeof(DXGI_FORMAT) * numRenderTargets);
    psoDesc.DSVFormat = dsvFormat;
    psoDesc.PrimitiveTopologyType = primitiveTopology;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
    psoDesc.SampleDesc.Count = 1;
    psoDesc.NodeMask = 0;

    if(inputLayout)
        psoDesc.InputLayout = *inputLayout;

    return psoDesc;
}

uint64_t Direct3DHelper::GetPSODescHash(D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc) noexcept
{
    // exclude pointers
    constexpr int offset1 = offsetof(D3D12_GRAPHICS_PIPELINE_STATE_DESC, BlendState);
    constexpr int offset2 = offsetof(D3D12_GRAPHICS_PIPELINE_STATE_DESC, NodeMask);
    constexpr int range = offset2 - offset1;

    uint8_t* start = reinterpret_cast<uint8_t*>(&desc) + offset1;

    return XXH3_64bits(start, range);
}

void Direct3DHelper::CreateGraphicsPSO(D3D12_GRAPHICS_PIPELINE_STATE_DESC& psDesc,
    ID3D12RootSignature* rootSignature, const D3D12_SHADER_BYTECODE* vertexShader, 
    const D3D12_SHADER_BYTECODE* pixelShader, const D3D12_SHADER_BYTECODE* hullShader, 
    const D3D12_SHADER_BYTECODE* domainShader, ID3D12PipelineState** pipelineState) noexcept
{
    psDesc.pRootSignature = rootSignature;

    psDesc.VS = *vertexShader;
    psDesc.PS = *pixelShader;

    if (hullShader)
    {
        psDesc.HS = *hullShader;
        psDesc.DS = *domainShader;
    }

    auto* device = App::GetRenderer().GetDevice();
    CheckHR(device->CreateGraphicsPipelineState(&psDesc, IID_PPV_ARGS(pipelineState)));
}

void Direct3DHelper::CreateBufferSRV(const DefaultHeapBuffer& buff, D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle, 
    UINT stride, UINT numElements) noexcept
{
    auto* res = const_cast<DefaultHeapBuffer&>(buff).GetResource();
    Assert(res, "Buffer hasn't been initialized.");

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = DXGI_FORMAT_UNKNOWN;
    srvDesc.Buffer.NumElements = numElements;
    srvDesc.Buffer.StructureByteStride = stride;

    auto* device = App::GetRenderer().GetDevice();
    device->CreateShaderResourceView(res, &srvDesc, cpuHandle);
}

void Direct3DHelper::CreateBufferUAV(const DefaultHeapBuffer& buff, D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle, 
    UINT stride, UINT numElements) noexcept
{
    auto* res = const_cast<DefaultHeapBuffer&>(buff).GetResource();
    Assert(res, "Buffer hasn't been initialized.");

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uavDesc.Format = DXGI_FORMAT_UNKNOWN;
    uavDesc.Buffer.NumElements = numElements;
    uavDesc.Buffer.StructureByteStride = stride;

    auto* device = App::GetRenderer().GetDevice();
    device->CreateUnorderedAccessView(res, nullptr, &uavDesc, cpuHandle);
}

void Direct3DHelper::CreateRawBufferUAV(const DefaultHeapBuffer& buff, D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle, UINT stride, UINT numElements) noexcept
{
    auto* res = const_cast<DefaultHeapBuffer&>(buff).GetResource();
    Assert(res, "Buffer hasn't been initialized.");
    Assert((stride & (4 - 1)) == 0, "Stride must be a multiple of 4.");

    const uint32_t byteWidth = stride * numElements;

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
    uavDesc.Buffer.NumElements = byteWidth >> 2;    // should be equal to number of 4-byte (unsigned) integers
    uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;

    auto* device = App::GetRenderer().GetDevice();
    device->CreateUnorderedAccessView(res, nullptr, &uavDesc, cpuHandle);
}

void Direct3DHelper::CreateTexture2DSRV(const Texture& t, D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle, DXGI_FORMAT f,
    float minLODClamp, UINT mostDetailedMip, UINT planeSlice) noexcept
{
    Assert(cpuHandle.ptr != 0, "Uninitialized D3D12_CPU_DESCRIPTOR_HANDLE");
    auto* device = App::GetRenderer().GetDevice();
    auto* res = const_cast<Texture&>(t).GetResource();
    Assert(res, "Texture hasn't been initialized.");
    auto desc = res->GetDesc();

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MostDetailedMip = mostDetailedMip;
    srvDesc.Texture2D.PlaneSlice = planeSlice;
    srvDesc.Texture2D.ResourceMinLODClamp = minLODClamp;
    srvDesc.Texture2D.MipLevels = desc.MipLevels;
    srvDesc.Format = f == DXGI_FORMAT_UNKNOWN ? desc.Format : f;
    
    device->CreateShaderResourceView(res, &srvDesc, cpuHandle);
}

void Direct3DHelper::CreateTexture3DSRV(const Texture& t, D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle, DXGI_FORMAT f,
    float minLODClamp, UINT mostDetailedMip, UINT planeSlice) noexcept
{
    Assert(cpuHandle.ptr != 0, "Uninitialized D3D12_CPU_DESCRIPTOR_HANDLE");
    auto* device = App::GetRenderer().GetDevice();
    auto* res = const_cast<Texture&>(t).GetResource();
    auto desc = res->GetDesc();

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture3D.MipLevels = desc.MipLevels;
    srvDesc.Texture3D.MostDetailedMip = mostDetailedMip;
    srvDesc.Texture3D.ResourceMinLODClamp = minLODClamp;
    srvDesc.Format = f == DXGI_FORMAT_UNKNOWN ? desc.Format : f;

    device->CreateShaderResourceView(res, &srvDesc, cpuHandle);
}

void Direct3DHelper::CreateRTV(const Texture& t, D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle, DXGI_FORMAT f, 
    UINT mipSlice, UINT planeSlice) noexcept
{
    Assert(cpuHandle.ptr != 0, "Uninitialized D3D12_CPU_DESCRIPTOR_HANDLE");
    auto* device = App::GetRenderer().GetDevice();
    auto* res = const_cast<Texture&>(t).GetResource();
    auto desc = res->GetDesc();

    D3D12_RENDER_TARGET_VIEW_DESC rtvDesc;
    rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    rtvDesc.Texture2D.MipSlice = mipSlice;
    rtvDesc.Texture2D.PlaneSlice = planeSlice;
    rtvDesc.Format = f == DXGI_FORMAT_UNKNOWN ? desc.Format : f;

    device->CreateRenderTargetView(res, &rtvDesc, cpuHandle);
}

void Direct3DHelper::CreateTexture2DUAV(const Texture& t, D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle, DXGI_FORMAT f, 
    UINT mipSlice, UINT planeSlice) noexcept
{
    Assert(cpuHandle.ptr != 0, "Uninitialized D3D12_CPU_DESCRIPTOR_HANDLE");
    auto* device = App::GetRenderer().GetDevice();
    auto* res = const_cast<Texture&>(t).GetResource();
    auto desc = res->GetDesc();

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uavDesc.Texture2D.MipSlice = mipSlice;
    uavDesc.Texture2D.PlaneSlice = planeSlice;
    uavDesc.Format = f == DXGI_FORMAT_UNKNOWN ? desc.Format : f;

    device->CreateUnorderedAccessView(res, nullptr, &uavDesc, cpuHandle);
}

void Direct3DHelper::CreateTexture3DUAV(const Texture& t, D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle, DXGI_FORMAT f, 
    UINT mipSlice, UINT numSlices, UINT firstSliceIdx) noexcept
{
    Assert(cpuHandle.ptr != 0, "Uninitialized D3D12_CPU_DESCRIPTOR_HANDLE");
    auto* device = App::GetRenderer().GetDevice();
    auto* res = const_cast<Texture&>(t).GetResource();
    auto desc = res->GetDesc();

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
    uavDesc.Texture3D.MipSlice = mipSlice;
    uavDesc.Texture3D.WSize = numSlices > 0 ? numSlices : desc.DepthOrArraySize;
    uavDesc.Texture3D.FirstWSlice = firstSliceIdx;
    uavDesc.Format = f == DXGI_FORMAT_UNKNOWN ? desc.Format : f;

    device->CreateUnorderedAccessView(res, nullptr, &uavDesc, cpuHandle);
}
