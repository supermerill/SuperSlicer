#define CATCH_CONFIG_DISABLE
#include <catch2/catch.hpp>
#include "test_utils.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Format/AMF.hpp"
#include "libslic3r/Config/PrintConfig.hpp"

using namespace Slic3r;
using namespace std::literals::string_literals;

SCENARIO("Reading deflated AMF files", "[AMF]") {
    auto _tmp_config = DynamicPrintConfig{};
    auto _tmp_conf_sub = ConfigSubstitutionContext{ForwardCompatibilitySubstitutionRule::Disable};
    GIVEN("Compressed AMF file of a 20mm cube") {
        auto model {new Slic3r::Model()};
        WHEN("file is read") {
            bool result_code = load_amf(get_model_path("test_amf/20mmbox_deflated.amf"s).c_str(), &_tmp_config, &_tmp_conf_sub, model, false);
            THEN("Does not return false.") {
                REQUIRE(result_code == true);
            }
            THEN("Model object contains a single ModelObject.") {
                REQUIRE(model->objects.size() == 1);
            }
        }
        WHEN("single file is read with some subdirectories") {
            bool result_code = load_amf(get_model_path("test_amf/20mmbox_deflated-in_directories.amf"s).c_str(), &_tmp_config, &_tmp_conf_sub, model, false);
            THEN("Read returns false.") {
                REQUIRE(result_code == true);
            }
            THEN("Model object contains no ModelObjects.") {
                REQUIRE(model->objects.size() == 1);
            }
        }
        WHEN("file is read with multiple files in the archive") {
            bool result_code = load_amf(get_model_path("test_amf/20mmbox_deflated-mult_files.amf"s).c_str(), &_tmp_config, &_tmp_conf_sub, model, false);
            THEN("Read returns ture.") {
                REQUIRE(result_code == true);
            }
            THEN("Model object contains one ModelObject.") {
                REQUIRE(model->objects.size() == 1);
            }
        }
        delete model;
    }
    GIVEN("Uncompressed AMF file of a 20mm cube") {
        auto model {new Slic3r::Model()};
        WHEN("file is read") {
            bool result_code = load_amf(get_model_path("test_amf/20mmbox.amf"s).c_str(), &_tmp_config, &_tmp_conf_sub, model, false);
            THEN("Does not return false.") {
                REQUIRE(result_code == true);
            }
            THEN("Model object contains a single ModelObject.") {
                REQUIRE(model->objects.size() == 1);
            }
        }
        WHEN("nonexistant file is read") {
            bool result_code = load_amf(get_model_path("test_amf/20mmbox-doesnotexist.amf"s).c_str(), &_tmp_config, &_tmp_conf_sub, model, false);
            THEN("Read returns false.") {
                REQUIRE(result_code == false);
            }
            THEN("Model object contains no ModelObject.") {
                REQUIRE(model->objects.size() == 0);
            }
        }
        delete model;
    }
}

SCENARIO("Reading AMF file", "[AMF]") {
    auto _tmp_config = DynamicPrintConfig{};
    auto _tmp_conf_sub = ConfigSubstitutionContext{ ForwardCompatibilitySubstitutionRule::Disable };
    GIVEN("badly formed AMF file (missing vertices)") {
        auto model {new Slic3r::Model()};
        WHEN("AMF model is read") {
            auto ret = Slic3r::load_amf(get_model_path("test_amf/5061-malicious.amf").c_str(), &_tmp_config, &_tmp_conf_sub, model, false);
            THEN("read should return False") {
                REQUIRE(!ret);
            }
        }
        delete model;
    }
    GIVEN("Ok formed AMF file") {
        auto model {new Slic3r::Model()};
        WHEN("AMF model is read") {
            std::cerr << "TEST_DATA_DIR/test_amf/read-amf.amf";
            auto ret = Slic3r::load_amf(get_model_path("test_amf/read-amf.amf").c_str(), &_tmp_config, &_tmp_conf_sub, model, false);
            THEN("read should return True") {
                REQUIRE(ret);
            }
        }
        delete model;
    }
}
