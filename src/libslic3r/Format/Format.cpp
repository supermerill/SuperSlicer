#include "Format.hpp"
#include "libslic3r/Exception.hpp"

namespace Slic3r {
std::shared_ptr<SLAAbstractArchive> get_output_format(const ConfigBase& config)
{
        OutputFormat output_format = Slic3r::output_format(config);
        if (output_format & ofSL1)
            return std::make_shared<SL1Archive>();
        else if (output_format & ofMaskedCWS) {
            return std::make_shared<MaskedCWSArchive>();
        }

        // Default to the base class
        return std::make_shared<SL1Archive>();
}

} // namespace Slic3r


