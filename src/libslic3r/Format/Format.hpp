#ifndef libslic3r_format_FORMAT_HPP
#define libslic3r_format_FORMAT_HPP

#include "CWS.hpp"
#include "SL1.hpp"

namespace Slic3r {

/// Select the correct subclass of
/// Implementers of new SLA print archive formats should add to this list.
std::shared_ptr<SLAAbstractArchive> get_output_format(const ConfigBase& config);
} // namespace Slic3r

#endif // libslic3r_format_FORMAT_HPP
