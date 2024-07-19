#ifndef slic3r_Format_BBconfig_hpp_
#define slic3r_Format_BBconfig_hpp_

#include "miniz_extension.hpp"


#include <map>

#ifdef __APPLE__
    #include <boost/filesystem.hpp>
    #include <boost/nowide/fstream.hpp>
    typedef boost::filesystem::path std_path;
    typedef boost::nowide::ifstream std_ifstream;
    #define GET_STD_PATH_FOR_IFSTREAM(PARAM) PARAM.string()
#else
    #include <filesystem>
    #include <fstream>
    typedef std::filesystem::path std_path;
    typedef std::ifstream std_ifstream;
    #define GET_STD_PATH_FOR_IFSTREAM(PARAM) PARAM
#endif

namespace Slic3r {
struct ConfigSubstitutionContext;
class DynamicPrintConfig;
class Model;
class ModelConfigObject;

bool read_json_file_bambu(const std_path &temp_file,
                          DynamicPrintConfig &         config,
                          ConfigSubstitutionContext &  config_substitutions,
                          bool                         with_phony);

std::map<std::string, std::string> read_ini_file_bambu(const std_path &temp_file);

bool convert_settings_from_bambu(std::map<std::string, std::string> bambu_settings_serialized,
                                 DynamicPrintConfig &               print_config,
                                 ConfigSubstitutionContext &        config_substitutions,
                                 bool                               with_phony);

bool convert_settings_from_bambu(std::map<std::string, std::string> bambu_settings_serialized,
                                 const DynamicPrintConfig &         print_config,
                                 ModelConfigObject &                object_config,
                                 ConfigSubstitutionContext &        config_substitutions,
                                 bool                               with_phony);

std_path get_temp_file(Model &model);
std_path extract_file(Model &model, mz_zip_archive &archive, const mz_zip_archive_file_stat &stat);
} // namespace Slic3r

#endif /* slic3r_Format_BBconfig_hpp_ */
