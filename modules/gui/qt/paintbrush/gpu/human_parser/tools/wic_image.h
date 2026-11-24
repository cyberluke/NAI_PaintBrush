/*****************************************************************************
 * wic_image.h : minimal WIC PNG/WebP load + PNG save for the smoke test
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************
 * The smoke executable is MSVC and intentionally has no Qt or third-party
 * image dependency. Windows Imaging Component decodes the model-card WebP
 * example (the Windows WebP codec is present on this machine) and PNG, and
 * writes the diagnostic masks as PNG.
 *****************************************************************************/

#ifndef PBHP_WIC_IMAGE_H
#define PBHP_WIC_IMAGE_H

#ifndef NOMINMAX
# define NOMINMAX
#endif
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace pbhp_test {

using Microsoft::WRL::ComPtr;

struct Image
{
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba; /* straight RGBA, width*height*4 */

    bool valid() const
    {
        return width > 0 && height > 0
               && rgba.size() == size_t(width) * size_t(height) * 4u;
    }
};

inline std::string hr_text(HRESULT hr)
{
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer), "HRESULT 0x%08lx", static_cast<unsigned long>(hr));
    return buffer;
}

inline std::wstring widen_utf8(const std::string &text)
{
    if (text.empty())
        return {};
    const int needed = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (needed <= 1)
        return {};
    std::wstring out(size_t(needed - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, out.data(), needed);
    return out;
}

inline bool load_image(const std::string &path, Image *out, std::string *error)
{
    if (!out)
        return false;
    *out = Image{};

    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                  CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory));
    if (FAILED(hr))
    {
        if (error)
            *error = "WIC factory: " + hr_text(hr);
        return false;
    }

    const std::wstring widePath = widen_utf8(path);
    ComPtr<IWICBitmapDecoder> decoder;
    hr = factory->CreateDecoderFromFilename(widePath.c_str(), nullptr, GENERIC_READ,
                                            WICDecodeMetadataCacheOnLoad, &decoder);
    if (FAILED(hr))
    {
        if (error)
            *error = "decode " + path + ": " + hr_text(hr);
        return false;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr))
    {
        if (error)
            *error = "frame: " + hr_text(hr);
        return false;
    }

    ComPtr<IWICFormatConverter> converter;
    hr = factory->CreateFormatConverter(&converter);
    if (FAILED(hr))
    {
        if (error)
            *error = "converter: " + hr_text(hr);
        return false;
    }
    hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
                               WICBitmapDitherTypeNone, nullptr, 0.0,
                               WICBitmapPaletteTypeCustom);
    if (FAILED(hr))
    {
        if (error)
            *error = "RGBA conversion: " + hr_text(hr);
        return false;
    }

    UINT width = 0;
    UINT height = 0;
    hr = converter->GetSize(&width, &height);
    if (FAILED(hr) || width == 0 || height == 0)
    {
        if (error)
            *error = "size: " + hr_text(hr);
        return false;
    }

    const UINT stride = width * 4u;
    const UINT bytes = stride * height;
    out->width = int(width);
    out->height = int(height);
    out->rgba.resize(size_t(bytes));
    hr = converter->CopyPixels(nullptr, stride, bytes, out->rgba.data());
    if (FAILED(hr))
    {
        if (error)
            *error = "pixels: " + hr_text(hr);
        out->rgba.clear();
        return false;
    }
    return true;
}

inline bool save_png_rgba(const std::string &path, int width, int height,
                          const uint8_t *rgba, std::string *error)
{
    if (width <= 0 || height <= 0 || !rgba)
        return false;

    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                  CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory));
    if (FAILED(hr))
    {
        if (error)
            *error = "WIC factory: " + hr_text(hr);
        return false;
    }

    const std::wstring widePath = widen_utf8(path);
    ComPtr<IWICStream> stream;
    hr = factory->CreateStream(&stream);
    if (SUCCEEDED(hr))
        hr = stream->InitializeFromFilename(widePath.c_str(), GENERIC_WRITE);
    if (FAILED(hr))
    {
        if (error)
            *error = "stream " + path + ": " + hr_text(hr);
        return false;
    }

    ComPtr<IWICBitmapEncoder> encoder;
    hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (SUCCEEDED(hr))
        hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> properties;
    if (SUCCEEDED(hr))
        hr = encoder->CreateNewFrame(&frame, &properties);
    if (SUCCEEDED(hr))
        hr = frame->Initialize(properties.Get());
    if (SUCCEEDED(hr))
        hr = frame->SetSize(UINT(width), UINT(height));
    /* All diagnostics are opaque visualisations, so write 24bpp BGR. The PNG
     * encoder reliably accepts it; 32bpp RGBA is rejected by some WIC codecs
     * with E_FAIL even though the samples carry no useful alpha. */
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    if (SUCCEEDED(hr))
        hr = frame->SetPixelFormat(&format);
    if (SUCCEEDED(hr) && !IsEqualGUID(format, GUID_WICPixelFormat24bppBGR))
        hr = E_FAIL;
    std::vector<uint8_t> bgr(size_t(width) * size_t(height) * 3u);
    for (size_t i = 0; i < size_t(width) * size_t(height); ++i)
    {
        bgr[i * 3u + 0] = rgba[i * 4u + 2];
        bgr[i * 3u + 1] = rgba[i * 4u + 1];
        bgr[i * 3u + 2] = rgba[i * 4u + 0];
    }
    const UINT stride = UINT(width) * 3u;
    const UINT bytes = stride * UINT(height);
    if (SUCCEEDED(hr))
        hr = frame->WritePixels(UINT(height), stride, bytes, bgr.data());
    if (SUCCEEDED(hr))
        hr = frame->Commit();
    if (SUCCEEDED(hr))
        hr = encoder->Commit();
    if (FAILED(hr))
    {
        if (error)
            *error = "encode " + path + ": " + hr_text(hr);
        return false;
    }
    return true;
}

inline bool save_png_gray(const std::string &path, int width, int height,
                          const uint8_t *gray, std::string *error)
{
    if (width <= 0 || height <= 0 || !gray)
        return false;
    std::vector<uint8_t> rgba(size_t(width) * size_t(height) * 4u);
    for (size_t i = 0; i < size_t(width) * size_t(height); ++i)
    {
        const uint8_t v = gray[i];
        rgba[i * 4u + 0] = v;
        rgba[i * 4u + 1] = v;
        rgba[i * 4u + 2] = v;
        rgba[i * 4u + 3] = 255;
    }
    return save_png_rgba(path, width, height, rgba.data(), error);
}

inline bool save_class_map(const std::string &path, int width, int height,
                           const uint8_t *classMap, std::string *error)
{
    static const uint8_t palette[18][3] = {
        {18, 18, 22},     {255, 214, 189}, {92, 58, 36},    {255, 86, 120},
        {255, 170, 60},   {255, 220, 60},  {90, 120, 255},  {180, 120, 60},
        {230, 80, 180},   {120, 220, 160},{200, 160, 255}, {120, 200, 255},
        {255, 190, 160},  {255, 230, 200},{140, 90, 60},   {90, 60, 40},
        {200, 120, 90},   {255, 240, 120}
    };
    std::vector<uint8_t> rgba(size_t(width) * size_t(height) * 4u);
    for (size_t i = 0; i < size_t(width) * size_t(height); ++i)
    {
        const int c = std::min<int>(17, classMap[i]);
        rgba[i * 4u + 0] = palette[c][0];
        rgba[i * 4u + 1] = palette[c][1];
        rgba[i * 4u + 2] = palette[c][2];
        rgba[i * 4u + 3] = 255;
    }
    return save_png_rgba(path, width, height, rgba.data(), error);
}

} // namespace pbhp_test

#endif // PBHP_WIC_IMAGE_H
