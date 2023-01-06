#include "Thumbnails.hpp"
#include "../miniz_extension.hpp"

#include <qoi/qoi.h>
#include <jpeg-compressor/jpge.h>

namespace Slic3r::GCodeThumbnails {

using namespace std::literals;

struct CompressedPNG : CompressedImageBuffer 
{
    ~CompressedPNG() override { if (data) mz_free(data); }
    std::string_view tag() const override { return "thumbnail"sv; }
};

struct CompressedJPG : CompressedImageBuffer
{
    ~CompressedJPG() override { free(data); }
    std::string_view tag() const override { return "thumbnail_JPG"sv; }
};

struct CompressedQOI : CompressedImageBuffer
{
    ~CompressedQOI() override { free(data); }
    std::string_view tag() const override { return "thumbnail_QOI"sv; }
};

struct CompressedBIQU : CompressedImageBuffer
{
    ~CompressedBIQU() override { free(data); }
    std::string_view tag() const override { return "thumbnail_BIQU"sv; }
};

struct CompressedTFT : CompressedImageBuffer
{
    ~CompressedTFT() override { free(data); }
    std::string_view tag() const override { return "thumbnail_TFT"sv; }
};

std::unique_ptr<CompressedImageBuffer> compress_thumbnail_png(const ThumbnailData& data)
{
    auto out = std::make_unique<CompressedPNG>();
    out->data = tdefl_write_image_to_png_file_in_memory_ex((const void*)data.pixels.data(), data.width, data.height, 4, &out->size, MZ_DEFAULT_LEVEL, 1);
    return out;
}

std::unique_ptr<CompressedImageBuffer> compress_thumbnail_biqu(const ThumbnailData& data)
{
    // Take vector of RGBA pixels and flip the image vertically
    std::vector<uint8_t> rgba_pixels(data.pixels.size());
    const size_t row_size = data.width * 4;
    for (size_t y = 0; y < data.height; ++y)
        ::memcpy(rgba_pixels.data() + (data.height - y - 1) * row_size, data.pixels.data() + y * row_size, row_size);

    auto out = std::make_unique<CompressedBIQU>();
    //size: height is number of lines. Add 2 byte to each line for the ';' and '\n'. Each pixel is 4 byte, +1 for the 0 of the c_str
    out->size = data.height * (2 + data.width * 4) + 1;
    out->data = malloc(out->size);

    int idx = 0;
    std::stringstream tohex;
    tohex << std::setfill('0') << std::hex;
    for (size_t y = 0; y < data.height; ++y) {
        tohex << ";";
        for (size_t x = 0; x < data.width; ++x) {
            uint16_t pixel = 0;
            //r
            pixel |= uint16_t((rgba_pixels[y * row_size + x * 4 + 0 ] & 0x000000F8) >> 3);
            //g
            pixel |= uint16_t((rgba_pixels[y * row_size + x * 4 + 1 ] & 0x000000FC) << 3);
            //b
            pixel |= uint16_t((rgba_pixels[y * row_size + x * 4 + 2 ] & 0x000000F8) << 8);
            tohex << std::setw(4) << pixel;
        }
        tohex << "\n";
    }
    std::string str = tohex.str();
    assert(str.size() + 1 == out->size);
    ::memcpy(out->data, (const void*)str.c_str(), out->size);
    return out;
}

std::unique_ptr<CompressedImageBuffer> compress_thumbnail_tft(const ThumbnailData& data)
{
    auto out = std::make_unique<CompressedTFT>();
    //size: height is number of lines. Add 9 byte to each line for the 'M10086 ;' and '\n'. Each pixel is 4 byte, +1 for the 0 of the c_str
    out->size = data.height * (9 + data.width * 4) + 1;
    out->data = malloc(out->size);

    // Take vector of RGBA pixels and flip the image vertically
    std::vector<uint8_t> rgba_pixels(data.pixels.size());
    const size_t row_size = data.width * 4;
    for (size_t y = 0; y < data.height; ++y)
        ::memcpy(rgba_pixels.data() + (data.height - y - 1) * row_size, data.pixels.data() + y * row_size, row_size);

    int idx = 0;
    std::stringstream tohex;
    tohex << std::setfill('0') << std::hex;
    for (size_t y = 0; y < data.height; ++y) {
        tohex << "M10086 ;";
        for (size_t x = 0; x < data.width; ++x) {
            uint16_t pixel = 0;
            //r
            pixel |= uint16_t((rgba_pixels[y * row_size + x * 4 + 0 ] & 0x000000F8) >> 3);
            //g
            pixel |= uint16_t((rgba_pixels[y * row_size + x * 4 + 1 ] & 0x000000FC) << 3);
            //b
            pixel |= uint16_t((rgba_pixels[y * row_size + x * 4 + 2 ] & 0x000000F8) << 8);
            tohex << std::setw(4) << pixel;
        }
        tohex << "\n";
    }
    std::string str = tohex.str();

    assert(str.size() + 1 == out->size);
    ::memcpy(out->data, (const void*)str.c_str(), out->size);
    return out;
}

std::unique_ptr<CompressedImageBuffer> compress_thumbnail_jpg(const ThumbnailData& data)
{
    // Take vector of RGBA pixels and flip the image vertically
    std::vector<uint8_t> rgba_pixels(data.pixels.size());
    const size_t row_size = data.width * 4;
    for (size_t y = 0; y < data.height; ++y)
        ::memcpy(rgba_pixels.data() + (data.height - y - 1) * row_size, data.pixels.data() + y * row_size, row_size);

    auto out = std::make_unique<CompressedJPG>();

    std::vector<jpge::uint8> compressed_data(data.pixels.size());
    jpge::params params;
    params.m_quality = 85;
    params.m_subsampling = jpge::H2V2;
    params.m_no_chroma_discrim_flag = false;
    params.m_two_pass_flag = false;
    params.m_use_std_tables = false;

    int compressed_data_size = int(compressed_data.size());
    if (jpge::compress_image_to_jpeg_file_in_memory(compressed_data.data(), compressed_data_size, data.width, data.height, 4, rgba_pixels.data(), params)) {
        out->data = malloc(compressed_data_size);
        out->size = size_t(compressed_data_size);
        ::memcpy(out->data, (const void*)compressed_data.data(), out->size);
    }
    return out;
}

std::unique_ptr<CompressedImageBuffer> compress_thumbnail_qoi(const ThumbnailData &data)
{
    qoi_desc desc;
    desc.width      = data.width;
    desc.height     = data.height;
    desc.channels   = 4;
    desc.colorspace = QOI_SRGB;

    // Take vector of RGBA pixels and flip the image vertically
    std::vector<uint8_t> rgba_pixels(data.pixels.size() * 4);
    size_t row_size = data.width * 4;
    for (size_t y = 0; y < data.height; ++ y)
        memcpy(rgba_pixels.data() + (data.height - y - 1) * row_size, data.pixels.data() + y * row_size, row_size);
    
    auto out = std::make_unique<CompressedQOI>();
    int  size;
    out->data = qoi_encode((const void*)rgba_pixels.data(), &desc, &size);
    out->size = size;
    return out;
}

std::unique_ptr<CompressedImageBuffer> compress_thumbnail(const ThumbnailData &data, GCodeThumbnailsFormat format)
{
    switch (format) {
        case GCodeThumbnailsFormat::PNG:
        default:
            return compress_thumbnail_png(data);
        case GCodeThumbnailsFormat::JPG:
            return compress_thumbnail_jpg(data);
        case GCodeThumbnailsFormat::QOI:
            return compress_thumbnail_qoi(data);
        case GCodeThumbnailsFormat::BIQU:
            return compress_thumbnail_biqu(data);
        case GCodeThumbnailsFormat::TFT:
            return compress_thumbnail_tft(data);
    }
}

} // namespace Slic3r::GCodeThumbnails
