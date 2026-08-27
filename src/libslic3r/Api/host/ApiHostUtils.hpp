///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Api_host_ApiHostUtils_hpp_
#define slic3r_Api_host_ApiHostUtils_hpp_

#include "libslic3r/Api/plugin/c/slic3r_data_tree.h"
#include "libslic3r/Config/ConfigDef.hpp"

namespace Slic3r::ApiHost {

// ConfigBase is inherited virtually by several config classes. Always let C++
// adjust the pointer to the ConfigBase subobject before hiding it behind the C
// ABI handle, otherwise the opaque pointer may be non-null but invalid.
inline const config_handle *to_config_handle(const ConfigBase *config)
{
    return reinterpret_cast<const config_handle *>(config);
}

inline config_handle *to_config_handle(ConfigBase *config)
{
    return reinterpret_cast<config_handle *>(config);
}

template<class Config>
inline const config_handle *to_config_handle(const Config *config)
{
    const ConfigBase *base = config;
    return to_config_handle(base);
}

template<class Config>
inline config_handle *to_config_handle(Config *config)
{
    ConfigBase *base = config;
    return to_config_handle(base);
}

// Reverse conversion for handles that were created through to_config_handle().
inline const ConfigBase *to_config(const config_handle *config)
{
    return reinterpret_cast<const ConfigBase *>(config);
}

inline ConfigBase *to_config(config_handle *config)
{
    return reinterpret_cast<ConfigBase *>(config);
}

} // namespace Slic3r::ApiHost


#endif // slic3r_Api_host_ApiHostUtils_hpp_
