#ifndef slic3r_GCodeThumbnails_hpp_
#define slic3r_GCodeThumbnails_hpp_

#include "../Point.hpp"
#include "../PrintConfig.hpp"
#include "ThumbnailData.hpp"

#include <vector>
#include <memory>
#include <string_view>

#include <boost/beast/core/detail/base64.hpp>

namespace Slic3r::GCodeThumbnails {

struct CompressedImageBuffer
{
    void       *data { nullptr };
    size_t      size { 0 };
    virtual ~CompressedImageBuffer() {}
    virtual std::string_view tag() const = 0;
};

std::unique_ptr<CompressedImageBuffer> compress_thumbnail(const ThumbnailData &data, GCodeThumbnailsFormat format);

template<typename WriteToOutput, typename ThrowIfCanceledCallback>
inline void export_thumbnails_to_file(ThumbnailsGeneratorCallback &thumbnail_cb, const std::vector<Vec2d> &sizes, bool with_bed, GCodeThumbnailsFormat format, WriteToOutput output, ThrowIfCanceledCallback throw_if_canceled)
{
    // Write thumbnails using base64 encoding
    if (thumbnail_cb != nullptr) {
        std::vector<Vec2d> good_sizes;
        for (const Vec2d& size : sizes)
            if (size.x() > 0 && size.y() > 0)
                good_sizes.push_back(size);
        if (good_sizes.empty()) return;

        //Create the thumbnails
        static constexpr const size_t max_row_length = 78;
        ThumbnailsList thumbnails = thumbnail_cb(ThumbnailsParams{ good_sizes, true, true, with_bed, true });
        image_count = 0
        for (const ThumbnailData& data : thumbnails)
            if (data.is_valid()) {
                auto compressed = compress_thumbnail(data, format);
                if (compressed->data && compressed->size) {
                    if (format == GCodeThumbnailsFormat::BIQU) {
                        output((boost::format("\n;\n; %s begin %dx%d %d\n") % compressed->tag() % data.width % data.height % (compressed->size - 1)).str().c_str());
                        //print size in hex
                        std::stringstream ss;
                        ss << std::setfill('0') << std::hex;
                        //biqu header
                        ss << ";" << std::setw(4) << data.width << std::setw(4) << data.height << "\n";
                        output(ss.str().c_str());
                        if (((char*)compressed->data)[compressed->size -1] == '\0')
                            output((char*)(compressed->data));
                        else
                            assert(false);
                        output("; bigtree thumbnail end\n");
                    }
                    else if (format == GCodeThumbnailsFormat::TFT) {
                        // for TFT, small and big thumbnails are necessary
                        
                        image_tag = ";simage:"

                        // if second image (big)
                        if (image_count == 1){
                            image_tag = ";;gimage:"
                        }
                        
                        output((boost::format("\n;\n; %s begin %dx%d\n") % compressed->tag() % data.width % data.height);
                        
                        if (((char*)compressed->data)[compressed->size -1] == '\0')
                            output((char*)(compressed->data));
                        else
                            assert(false);
                        
                        output("M10086 ;\n");

                        image_count++;
                    } else {
                        std::string encoded;
                        encoded.resize(boost::beast::detail::base64::encoded_size(compressed->size));
                        encoded.resize(boost::beast::detail::base64::encode((void*)encoded.data(), (const void*)compressed->data, compressed->size));

                        output((boost::format("\n;\n; %s begin %dx%d %d\n") % compressed->tag() % data.width % data.height % encoded.size()).str().c_str());
                        while (encoded.size() > max_row_length) {
                            output((boost::format("; %s\n") % encoded.substr(0, max_row_length)).str().c_str());
                            encoded = encoded.substr(max_row_length);
                        }

                        if (encoded.size() > 0)
                            output((boost::format("; %s\n") % encoded).str().c_str());
                    }
                    output((boost::format("; %s end\n;\n") % compressed->tag()).str().c_str());
                }
                throw_if_canceled();
            }
    }
}

} // namespace Slic3r::GCodeThumbnails

#endif // slic3r_GCodeThumbnails_hpp_
