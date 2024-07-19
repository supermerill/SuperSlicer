#include "ScriptExecutor.hpp"

#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"

#include "GUI_App.hpp"
#include "Plater.hpp"
#include "Tab.hpp"

#include <boost/log/trivial.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/algorithm/string/trim_all.hpp>
#include <boost/algorithm/string/erase.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/filesystem.hpp>

#include <string>

#include <angelscript/source/as_config.h>
#include <angelscript/add_on/autowrapper/aswrappedcall.h>
#include <angelscript/add_on/scriptarray/scriptarray.h>
#include <angelscript/add_on/scriptbuilder/scriptbuilder.h>
#include <angelscript/add_on/scriptstdstring/scriptstdstring.h>
#include <angelscript/add_on/scriptmath/scriptmath.h>

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/log/trivial.hpp>

using namespace gw;

namespace Slic3r {  namespace GUI { namespace script {

class NoDefinitionExceptionEmitLog : public ConfigurationError
{
public:
    NoDefinitionExceptionEmitLog(const std::string &message) :
        ConfigurationError(message) {
        BOOST_LOG_TRIVIAL(error) << message;
    }
};

void as_message_callback(const AngelScript::asSMessageInfo* msg, void* param)
{
    const char* type = "ERR ";
    if (msg->type == AngelScript::asMSGTYPE_WARNING)
        type = "WARN";
    else if (msg->type == AngelScript::asMSGTYPE_INFORMATION)
        type = "INFO";

    printf("%s (%d, %d) : %s : %s\n", msg->section, msg->row, msg->col, type, msg->message);
}

//TODO find a way to put script methods in the ScriptContainer class so there isn't this mutex-locked global var.
std::mutex current_script_mutex;
ScriptContainer* current_script;
void as_print(std::string& str)
{
    std::cout << str;
}
void as_print_float(float f)
{
    std::cout << f;
}
//void as_register_key(std::string& key) {
//    if (watching_keys != nullptr)
//        watching_keys->push_back(key);
//}
std::pair<const PresetCollection*, const ConfigOption*> get_coll(const std::string& str) {
    const PresetCollection* coll = nullptr;
    const ConfigOption* opt = nullptr;
    if(opt == nullptr && (current_script->tab()->get_printer_technology() & PrinterTechnology::ptFFF) != 0) {
        coll = &current_script->tab()->m_preset_bundle->fff_prints;
        opt = coll->get_edited_preset().config.option(str);
        if (opt == nullptr) {
            coll = &current_script->tab()->m_preset_bundle->filaments;
            opt = coll->get_edited_preset().config.option(str);
        }
    }
    if (opt == nullptr && (current_script->tab()->get_printer_technology() & PrinterTechnology::ptSLA) != 0) {
        coll = &current_script->tab()->m_preset_bundle->sla_prints;
        opt = coll->get_edited_preset().config.option(str);
        if (opt == nullptr) {
            coll = &current_script->tab()->m_preset_bundle->sla_materials;
            opt = coll->get_edited_preset().config.option(str);
        }
    }
    if (opt == nullptr) {
        coll = &current_script->tab()->m_preset_bundle->printers;
        opt = coll->get_edited_preset().config.option(str);
    }
    return std::pair<const PresetCollection*, const ConfigOption*>{ coll,  opt };
}
//PresetCollection* get_coll(int preset_type) {
//    if (preset_type <= 0)
//        return current_script->tech() == (PrinterTechnology::ptFFF)
//        ? &current_script->tab()->m_preset_bundle->fff_prints
//        : &current_script->tab()->m_preset_bundle->sla_prints;
//    else if (preset_type == 1)
//        return current_script->tech() == (PrinterTechnology::ptFFF)
//        ? &current_script->tab()->m_preset_bundle->filaments
//        : &current_script->tab()->m_preset_bundle->sla_materials;
//    else return &current_script->tab()->m_preset_bundle->printers;
//}
bool as_get_bool_idx(std::string& key, int idx)
{
    const ConfigOption* opt = get_coll(key).second;
    if (opt == nullptr || (opt->type() != ConfigOptionType::coBool && opt->type() != ConfigOptionType::coBools))
        throw NoDefinitionExceptionEmitLog("get_bool(): error, can't find bool option " + key);
    //can use get_float()!=0 instead of get_bool() if we want to make it works on evry type
    return opt->get_bool(idx);
}
bool as_get_bool(std::string &key) { return as_get_bool_idx(key, 0); }
void _set_bool(DynamicPrintConfig& conf, const ConfigOption* opt, std::string& key, int idx, bool b_val)
{
    if (opt->type() == ConfigOptionType::coBool) {
        conf.set_key_value(key, new ConfigOptionBool(b_val));
    } else if (opt->type() == ConfigOptionType::coBools) {
        ConfigOptionBools* new_val = static_cast<ConfigOptionBools*>(opt->clone());
        if(idx < 0)
            // replace all values
            for(size_t i=0; i<new_val->size(); ++i)
                new_val->set_at(b_val, i);
        else
            new_val->set_at(b_val, idx);
        conf.set_key_value(key, new_val);
    } else {
        throw NoDefinitionExceptionEmitLog("set_bool(): error, can't find bool option (wrong type?) " + key);
    }
}

void as_set_bool(std::string& key, bool b)
{
    if (!current_script->can_set()) return;
    std::pair<const PresetCollection*, const ConfigOption*> result = get_coll(key);
    if (result.second == nullptr)
        throw NoDefinitionExceptionEmitLog("set_bool(): error, can't find bool option " + key);
    DynamicPrintConfig& conf = current_script->to_update()[result.first->type()];
    if (auto newer_opt = conf.optptr(key)) {
        _set_bool(conf, newer_opt, key, -1, b);
    } else {
        _set_bool(conf, result.second, key, -1, b);
    }
}

int32_t as_get_int_idx(std::string& key, int idx)
{
    const ConfigOption* opt = get_coll(key).second;
    if (opt == nullptr || (opt->type() != ConfigOptionType::coInt && opt->type() != ConfigOptionType::coInts && opt->type() != ConfigOptionType::coEnum))
        throw NoDefinitionExceptionEmitLog("get_int(): error, can't find int option " + key);
    if (opt->is_vector()) {
        const ConfigOptionVectorBase* vector = static_cast<const ConfigOptionVectorBase*>(opt);
        return (int32_t)vector->get_int(idx);
    } else {
        return (int32_t)(opt->get_int());
    }
}
int32_t as_get_int(std::string &key) { return as_get_int_idx(key, 0); }
void    _set_int(DynamicPrintConfig &conf, const ConfigOption *opt, std::string &key, int idx, int i_val)
{
    if (opt->type() == ConfigOptionType::coInt) {
        conf.set_key_value(key, new ConfigOptionInt(i_val));
    } else if (opt->type() == ConfigOptionType::coInts) {
        ConfigOptionInts *new_val = static_cast<ConfigOptionInts *>(opt->clone());
        if(idx < 0)
            for (size_t i = 0; i < new_val->size(); ++i) new_val->set_at(i_val, i);
        else
            new_val->set_at(i_val, idx);
        conf.set_key_value(key, new_val);
    } else if (opt->type() == ConfigOptionType::coEnum) {
        ConfigOption *copy = opt->clone();
        copy->set_enum_int(i_val);
        conf.set_key_value(key, copy);
    } else {
        throw NoDefinitionExceptionEmitLog("set_int(): error, can't find int option (wrong type?) " + key);
    }
}
void as_set_int(std::string& key, int val)
{
    if (!current_script->can_set()) return;
    std::pair<const PresetCollection*, const ConfigOption*> result = get_coll(key);
    if (result.second == nullptr)
        throw NoDefinitionExceptionEmitLog("set_int(): error, can't find int option " + key);
    DynamicPrintConfig& conf = current_script->to_update()[result.first->type()];
    if (auto newer_opt = conf.optptr(key)) {
        _set_int(conf, newer_opt, key, -1, val);
    } else {
        _set_int(conf, result.second, key, -1, val);
    }
}
float as_get_float_idx(std::string& key, int idx)
{
    const ConfigOption* opt = get_coll(key).second;
    if (opt == nullptr) //TODO check if  float, etc..
        throw NoDefinitionExceptionEmitLog("get_float(): error, can't find float option " + key);
    float val = 1;
    // if precent, divide by 100
    if (opt->type() == ConfigOptionType::coPercent || opt->type() == ConfigOptionType::coPercents) {
        val *= 0.01f;
    }
    if (opt->type() == ConfigOptionType::coFloatOrPercent && static_cast<const ConfigOptionFloatOrPercent*>(opt)->percent)
        val *= 0.01f;
    if (opt->is_vector()) {
        const ConfigOptionVectorBase* vector = static_cast<const ConfigOptionVectorBase*>(opt);
        if (opt->type() == ConfigOptionType::coFloatsOrPercents && static_cast<const ConfigOptionFloatsOrPercents*>(vector)->get_at(idx).percent)
            val *= 0.01f;
        val *= (float)vector->get_float(idx);
    } else {
        val *= (float)(opt->get_float());
    }
    return val;
}
float  as_get_float(std::string &key) { return as_get_float_idx(key, 0); }
double round(float value) {
    double intpart;
    if (modf(value, &intpart) == 0.0) {
        // shortcut for int
        return value;
    }
    std::stringstream ss;
    //first, get the int part, to see how many digit it takes
    int long10 = 0;
    if (intpart > 9)
        long10 = (int)std::floor(std::log10(std::abs(intpart)));
        //set the usable precision: there is only ~7 decimal digit in a float (15-16 decimal digit in a double)
        ss << std::fixed << std::setprecision(7 - long10) << value;
    double dbl_val;
    ss >> dbl_val;
    return dbl_val;
}

void _set_float(DynamicPrintConfig& conf, const ConfigOption* opt, std::string& key, int idx, float f_val)
{
    if (opt->type() == ConfigOptionType::coFloat) {
        double old_value = opt->get_float();
        double new_val = round(f_val);
        // only update if difference is significant
        if (std::abs(old_value - new_val) / std::abs(old_value) < 0.0000001)
            new_val = old_value; // don't return int these check, as it can escpae a refresh of the scripted widget
        conf.set_key_value(key, new ConfigOptionFloat(new_val));
    } else if (opt->type() == ConfigOptionType::coFloats) {
        ConfigOptionFloats* new_opt = static_cast<ConfigOptionFloats*>(opt->clone());
        double new_val = round(f_val);
        if (!new_opt->empty()) {
            // only update if difference is significant
            double old_value = idx < 0 ? new_opt->get_float(0) : new_opt->get_float(idx);
            if (std::abs(old_value - new_val) / std::abs(old_value) < 0.0000001)
                new_val = old_value;
        }
        if(idx < 0)
            // replace all values
            for(size_t i=0; i<new_opt->size(); ++i)
                new_opt->set_at(new_val, i);
        else
            new_opt->set_at(new_val, idx);
        conf.set_key_value(key, new_opt);
    } else if (opt->type() == ConfigOptionType::coPercent) {
        double percent_f = floor(f_val * 100000. + 0.5) / 1000.;
        // only update if difference is significant
        double old_value = opt->get_float();
        if (std::abs(old_value - percent_f) / std::abs(old_value) < 0.0000001)
            percent_f = old_value;
        conf.set_key_value(key, new ConfigOptionPercent(percent_f));
    } else if (opt->type() == ConfigOptionType::coPercents) {
        ConfigOptionPercents* new_opt = static_cast<ConfigOptionPercents*>(opt->clone());
        double percent_f = floor(f_val * 100000. + 0.5) / 1000.;
        if (!new_opt->empty()) {
            // only update if difference is significant
            double old_value = idx < 0 ? new_opt->get_float(0) : new_opt->get_float(idx);
            if (std::abs(old_value - percent_f) / std::abs(old_value) < 0.0000001)
                percent_f = old_value;
        }
        if (idx < 0)
            // replace all values
            for (size_t i = 0; i < new_opt->size(); ++i) new_opt->set_at(percent_f, i);
        else
            new_opt->set_at(percent_f, idx);
        conf.set_key_value(key, new_opt);
    } else if (opt->type() == ConfigOptionType::coFloatOrPercent) {
        double new_val = round(f_val);
        if (!static_cast<const ConfigOptionFloatOrPercent*>(opt)->percent) {
            // only update if difference is significant
            double old_value = opt->get_float();
            if (std::abs(old_value - new_val) / std::abs(old_value) < 0.0000001)
                new_val = old_value;
        }
        conf.set_key_value(key, new ConfigOptionFloatOrPercent(new_val, false));
    } else if (opt->type() == ConfigOptionType::coFloatsOrPercents) {
        ConfigOptionFloatsOrPercents* new_opt = static_cast<ConfigOptionFloatsOrPercents*>(opt->clone());
        double new_val = round(f_val);
        if (!new_opt->empty() && !new_opt->get_at(0).percent) {
            // only update if difference is significant
            double old_value = idx < 0 ? new_opt->get_float(0) : new_opt->get_float(idx);
            if (std::abs(old_value - new_val) / std::abs(old_value) < 0.0000001)
                new_val = old_value;
        }
        if (idx < 0)
            // replace all values
            for (size_t i = 0; i < new_opt->size(); ++i) new_opt->set_at(FloatOrPercent{new_val, false}, i);
        else
            new_opt->set_at(FloatOrPercent{new_val, false}, idx);
        conf.set_key_value(key, new_opt);
    } else {
        throw NoDefinitionExceptionEmitLog("set_float(): error, can't find float option (wrong type?) " + key);
    }
}
void as_set_float(std::string& key, float f_val)
{
    if (!current_script->can_set()) return;
    std::pair<const PresetCollection*, const ConfigOption*> result = get_coll(key);
    if (result.second == nullptr)
        throw NoDefinitionExceptionEmitLog("set_float(): error, can't find float option " + key);

    DynamicPrintConfig& conf = current_script->to_update()[result.first->type()];
    if (auto newer_opt = conf.optptr(key)) {
        _set_float(conf, newer_opt, key, -1, f_val);
    } else {
        _set_float(conf, result.second, key, -1, f_val);
    }
}
bool as_is_percent_idx(std::string& key, int idx)
{
    const ConfigOption* opt = get_coll(key).second;
    if (opt == nullptr)
        throw NoDefinitionExceptionEmitLog("is_percent(): error, can't find percent option " + key);
    return (opt->type() == ConfigOptionType::coPercent) || (opt->type() == ConfigOptionType::coPercents) 
        || (opt->type() == ConfigOptionType::coFloatOrPercent && ((ConfigOptionFloatOrPercent*)opt)->percent) 
        || (opt->type() == ConfigOptionType::coFloatsOrPercents && ((ConfigOptionFloatsOrPercents*)opt)->get_at(idx).percent);
}
bool as_is_percent(std::string &key) { return as_is_percent_idx(key, 0); }

void _set_percent(DynamicPrintConfig& conf, const ConfigOption* opt, std::string& key, int idx, float p_val)
{
    double percent_f = floor(p_val * 1000. + 0.5) / 1000.;
    if (opt->type() == ConfigOptionType::coFloat) {
        // only update if difference is significant
        double old_value = opt->get_float() * 100;
        if (std::abs(old_value - percent_f) / std::abs(old_value) < 0.0000001)
            percent_f = old_value; // don't return int these check, as it can escpae a refresh of the scripted widget
        conf.set_key_value(key, new ConfigOptionFloat(percent_f / 100.));
    } else if (opt->type() == ConfigOptionType::coFloats) {
        ConfigOptionFloats* new_opt = static_cast<ConfigOptionFloats*>(opt->clone());
        if (!new_opt->empty()) {
            // only update if difference is significant
            double old_value = new_opt->get_at(0) * 100;
            if (std::abs(old_value - percent_f) / std::abs(old_value) < 0.0000001)
                percent_f = old_value;
        }
        if (idx < 0)
            for(size_t i=0; i<new_opt->size(); ++i)
                new_opt->set_at(percent_f / 100., i);
        else
            new_opt->set_at(percent_f / 100., idx);
        conf.set_key_value(key, new_opt);
    } else if (opt->type() == ConfigOptionType::coPercent) {
        // only update if difference is significant
        double old_value = get_coll(key).second->get_float();
        if (std::abs(old_value - percent_f) / std::abs(old_value) < 0.0000001)
            percent_f = old_value;
        conf.set_key_value(key, new ConfigOptionPercent(percent_f));
    } else if (opt->type() == ConfigOptionType::coPercents) {
        ConfigOptionPercents* new_opt = static_cast<ConfigOptionPercents*>(opt->clone());
        if (!new_opt->empty()) {
            // only update if difference is significant
            double old_value = new_opt->get_at(0);
            if (std::abs(old_value - percent_f) / std::abs(old_value) < 0.0000001)
                percent_f = old_value;
        }
        if (idx < 0)
            for(size_t i=0; i<new_opt->size(); ++i)
                new_opt->set_at(percent_f, i);
        else
            new_opt->set_at(percent_f, idx);
        conf.set_key_value(key, new_opt);
    } else if (opt->type() == ConfigOptionType::coFloatOrPercent) {
        if (static_cast<const ConfigOptionFloatOrPercent*>(opt)->percent) {
            // only update if difference is significant
            double old_value = opt->get_float();
            if (std::abs(old_value - percent_f) / std::abs(old_value) < 0.0000001)
                percent_f = old_value;
        }
        conf.set_key_value(key, new ConfigOptionFloatOrPercent(percent_f, true));
    } else if (opt->type() == ConfigOptionType::coFloatsOrPercents) {
        ConfigOptionFloatsOrPercents* new_opt = static_cast<ConfigOptionFloatsOrPercents*>(opt->clone());
        if (!new_opt->empty() && new_opt->get_at(0).percent) {
            // only update if difference is significant
            double old_value = new_opt->get_at(0).value;
            if (std::abs(old_value - percent_f) / std::abs(old_value) < 0.0000001)
                percent_f = old_value;
        }
        if (idx < 0)
            for(size_t i=0; i<new_opt->size(); ++i)
                new_opt->set_at(FloatOrPercent{ percent_f, true }, i);
        else
            new_opt->set_at(FloatOrPercent{ percent_f, true }, idx);
        conf.set_key_value(key, new_opt);
    }
}
void as_set_percent(std::string &key, float p_val)
{
    if (!current_script->can_set())
        return;
    std::pair<const PresetCollection *, const ConfigOption *> result = get_coll(key);
    if (result.second == nullptr)
        throw NoDefinitionExceptionEmitLog("set_percent(): error, can't find percent option " + key);
    DynamicPrintConfig &conf = current_script->to_update()[result.first->type()];
    if (auto newer_opt = conf.optptr(key)) {
        _set_percent(conf, newer_opt, key, -1, p_val);
    } else {
        _set_percent(conf, result.second, key, -1, p_val);
    }
}

void as_get_string_idx(std::string& key, int idx, std::string& val)
{
    std::pair<const PresetCollection*, const ConfigOption*> result = get_coll(key);
    const ConfigOption* opt = result.second;
    if (opt == nullptr) //TODO check if  float, etc..
        throw NoDefinitionExceptionEmitLog("get_string(): error, can't find string option " + key);
    if (opt->type() == ConfigOptionType::coString) {
        val = ((ConfigOptionString*)opt)->value;
    } else if (opt->type() == ConfigOptionType::coStrings) {
        val = ((ConfigOptionStrings*)opt)->get_at(idx);
    } else if (opt->type() == ConfigOptionType::coEnum) {
        val = opt->serialize();
    } else {
        throw NoDefinitionExceptionEmitLog("get_string(): error, can't find string option (wrong type?) " + key);
    }
}
void as_get_string(std::string &key, std::string &val) { as_get_string_idx(key, 0, val); }

void _set_string(DynamicPrintConfig& conf, const PresetCollection* pcoll, const ConfigOption* opt, std::string& key, int idx, std::string& val)
{
    if (opt->type() == ConfigOptionType::coString) {
        conf.set_key_value(key, new ConfigOptionString(val));
    } else if (opt->type() == ConfigOptionType::coStrings) {
        ConfigOptionStrings* new_val = (ConfigOptionStrings*)opt->clone();
        for(size_t i=0; i<new_val->size(); ++i)
            new_val->set_at(val, i);
        conf.set_key_value(key, new_val);
    } else if (opt->type() == ConfigOptionType::coEnum) {
        const ConfigOptionDef* def = pcoll->get_edited_preset().config.get_option_def(key);
        std::optional<int> it_idx = def->enum_def->value_to_index(val);
        if (!it_idx) {
            it_idx = def->enum_def->label_to_index(val);
            if (!it_idx)
                throw NoDefinitionExceptionEmitLog("set_string(): error, can't find enum option '" + val + "' in " + key);
        }
        ConfigOption* copy = opt->clone();
        copy->set_enum_int(*it_idx);
        conf.set_key_value(key, copy);
    } else {
        throw NoDefinitionExceptionEmitLog("set_string(): error, can't find string option (wrong type?) " + key);
    }
}
void as_set_string(std::string &key, std::string &val) {
    if (!current_script->can_set()) return;
    std::pair<const PresetCollection*, const ConfigOption*> result = get_coll(key);
    if (result.second == nullptr)
        throw NoDefinitionExceptionEmitLog("set_string(): error, can't find string option " + key);
    DynamicPrintConfig& conf = current_script->to_update()[result.first->type()];
    if (auto newer_opt = conf.optptr(key)) {
        _set_string(conf, result.first, newer_opt, key, -1, val);
    } else {
        _set_string(conf, result.first, result.second, key, -1, val);
    }
}

//// vector vars ////

int as_size(std::string &key) {
    const ConfigOption* opt = get_coll(key).second;
    if (opt->is_vector()) {
        const ConfigOptionVectorBase* vector = static_cast<const ConfigOptionVectorBase*>(opt);
        return vector->size();
    } else {
        return 1;
    }
}

void as_clear(std::string &key) {
    std::pair<const PresetCollection*, const ConfigOption*> result = get_coll(key);
    const ConfigOption* opt = result.second;
    DynamicPrintConfig& conf = current_script->to_update()[result.first->type()];
    if (opt->is_vector()) {
        ConfigOptionVectorBase* copy = static_cast<ConfigOptionVectorBase*>(opt->clone());
        copy->clear();
        conf.set_key_value(key, copy);
    } else {
        // ? return to default value ?
        const ConfigOptionDef* def = result.first->get_edited_preset().config.get_option_def(key);
        conf.set_key_value(key, def->default_value->clone());
    }
}

void as_set_bool_idx(std::string& key, int idx, bool b_val)
{
    if (!current_script->can_set()) return;
    std::pair<const PresetCollection*, const ConfigOption*> result = get_coll(key);
    if (result.second == nullptr)
        throw NoDefinitionExceptionEmitLog("set_bool_idx(): error, can't find bool option " + key);

    DynamicPrintConfig& conf = current_script->to_update()[result.first->type()];
    if (auto newer_opt = conf.optptr(key)) {
        _set_bool(conf, newer_opt, key, idx, b_val);
    } else {
        _set_bool(conf, result.second, key, idx, b_val);
    }
}

void as_set_int_idx(std::string& key, int idx, int i_val)
{
    if (!current_script->can_set()) return;
    std::pair<const PresetCollection*, const ConfigOption*> result = get_coll(key);
    if (result.second == nullptr)
        throw NoDefinitionExceptionEmitLog("set_int_idx(): error, can't find int option " + key);

    DynamicPrintConfig& conf = current_script->to_update()[result.first->type()];
    if (auto newer_opt = conf.optptr(key)) {
        _set_int(conf, newer_opt, key, idx, i_val);
    } else {
        _set_int(conf, result.second, key, idx, i_val);
    }
}

void as_set_float_idx(std::string& key, int idx, float f_val)
{
    if (!current_script->can_set()) return;
    std::pair<const PresetCollection*, const ConfigOption*> result = get_coll(key);
    if (result.second == nullptr)
        throw NoDefinitionExceptionEmitLog("set_float_idx(): error, can't find float option " + key);

    DynamicPrintConfig& conf = current_script->to_update()[result.first->type()];
    if (auto newer_opt = conf.optptr(key)) {
        _set_float(conf, newer_opt, key, idx, f_val);
    } else {
        _set_float(conf, result.second, key, idx, f_val);
    }
}
void as_set_percent_idx(std::string& key, int idx, float p_val)
{
    if (!current_script->can_set()) return;
    std::pair<const PresetCollection*, const ConfigOption*> result = get_coll(key);
    if (result.second == nullptr)
        throw NoDefinitionExceptionEmitLog("set_percent_idx(): error, can't find float/percent option " + key);

    DynamicPrintConfig& conf = current_script->to_update()[result.first->type()];
    if (auto newer_opt = conf.optptr(key)) {
        _set_percent(conf, newer_opt, key, idx, p_val);
    } else {
        _set_percent(conf, result.second, key, idx, p_val);
    }
}

void as_set_string_idx(std::string& key, int idx, std::string& str_val)
{
    if (!current_script->can_set()) return;
    std::pair<const PresetCollection*, const ConfigOption*> result = get_coll(key);
    if (result.second == nullptr)
        throw NoDefinitionExceptionEmitLog("set_string_idx(): error, can't find string option " + key);

    DynamicPrintConfig& conf = current_script->to_update()[result.first->type()];
    if (auto newer_opt = conf.optptr(key)) {
        _set_string(conf, result.first, newer_opt, key, idx, str_val);
    } else {
        _set_string(conf, result.first, result.second, key, idx, str_val);
    }
}


/////// custom vars ////////

std::string get_custom_var_option(int preset_type) {
    if (preset_type <= 0)
        return (current_script->tab()->get_printer_technology() & PrinterTechnology::ptFFF) != 0
        ? current_script->tab()->m_preset_bundle->fff_prints.get_edited_preset().config.opt_string("print_custom_variables")
        : current_script->tab()->m_preset_bundle->sla_prints.get_edited_preset().config.opt_string("print_custom_variables");
    else if (preset_type == 1) {
        return (current_script->tab()->get_printer_technology() & PrinterTechnology::ptFFF) != 0
            ? current_script->tab()->m_preset_bundle->filaments.get_edited_preset().config.opt_string("filament_custom_variables", size_t(0))
            : current_script->tab()->m_preset_bundle->sla_materials.get_edited_preset().config.opt_string("filament_custom_variables", size_t(0));
    } else return current_script->tab()->m_preset_bundle->printers.get_edited_preset().config.opt_string("printer_custom_variables");
}
std::string get_custom_value(std::string custom_var_field, const std::string& opt_key) {
    if (custom_var_field.find(opt_key) != std::string::npos) {
        boost::erase_all(custom_var_field, "\r");
        std::vector<std::string> lines;
        boost::algorithm::split(lines, custom_var_field, boost::is_any_of("\n"));
        for (const std::string& line : lines) {
            size_t equal_pos = line.find_first_of('=');
            if (equal_pos != std::string::npos) {
                std::string name = line.substr(0, equal_pos);
                std::string value = line.substr(equal_pos + 1);
                boost::algorithm::trim(name);
                if (name == opt_key) {
                    boost::algorithm::trim(value);
                    return value;
                }
            }
        }
    }
    return "";
}
void set_custom_option(int preset_type, std::string new_value) {
    if (!current_script->can_set()) return;
    if (preset_type <= 0) {
        PresetCollection& coll = (current_script->tab()->get_printer_technology() & PrinterTechnology::ptFFF) != 0
            ? current_script->tab()->m_preset_bundle->fff_prints
            : current_script->tab()->m_preset_bundle->sla_prints;
        DynamicPrintConfig& conf = current_script->to_update()[coll.type()];
        conf.set_key_value("print_custom_variables", new ConfigOptionString(new_value));
    } else if (preset_type == 1) {
        const PresetCollection& coll = (current_script->tab()->get_printer_technology() & PrinterTechnology::ptFFF) != 0
            ? current_script->tab()->m_preset_bundle->filaments
            : current_script->tab()->m_preset_bundle->sla_materials;
        DynamicPrintConfig& conf = current_script->to_update()[coll.type()];
        conf.set_key_value("filament_custom_variables", new ConfigOptionString(new_value));
    } else {
        DynamicPrintConfig& conf = current_script->to_update()[Preset::Type::TYPE_PRINTER];
        conf.set_key_value("printer_custom_variables", new ConfigOptionString(new_value));
    }
}
void set_custom_value(std::string& custom_var_field, const std::string& opt_key, const std::string& new_value) {
    boost::erase_all(custom_var_field, "\r");
    bool found = false;
    //iterate onlines,until you find the good one
    std::vector<std::string> lines;
    boost::algorithm::split(lines, custom_var_field, boost::is_any_of("\n"));
    for (auto line = lines.begin(); line != lines.end(); ++line) {
        size_t equal_pos = line->find_first_of('=');
        if (equal_pos != std::string::npos) {
            std::string name = line->substr(0, equal_pos);
            std::string value = line->substr(equal_pos + 1);
            boost::algorithm::trim(name);
            if (name == opt_key) {
                if (new_value == "") {
                    //delete
                    lines.erase(line);
                } else {
                    *line = name + " = " + new_value;
                }
                found = true;
                break;
            }
        }
    }
    if (!found) {
        //put it at the end
        lines.push_back(opt_key + " = " + new_value);
    }
    //reconstruct from lines
    custom_var_field = "";
    for (std::string& line : lines)
        if(!line.empty())
            custom_var_field += line + "\n";
}
bool as_get_custom_bool(int preset, std::string& key, bool& result)
{
    std::string serialized_value = get_custom_value(get_custom_var_option(preset), key);
    if (serialized_value.empty())
        return false;

    if (serialized_value == "true") {
        result = true;
        return true;
    } else if (serialized_value == "false") {
        result = false;
        return true;
    }
    return false;
}
void as_set_custom_bool(int preset, std::string& key, bool b)
{
    if (!current_script->can_set()) return;
    std::string serialized_vars = get_custom_var_option(preset);
    set_custom_value(serialized_vars, key, b ? "true" : "false");
    set_custom_option(preset, serialized_vars);
}
bool as_get_custom_int(int preset, std::string& key, int32_t& result)
{
    std::string serialized_value = get_custom_value(get_custom_var_option(preset), key);
    if (serialized_value.empty())
        return false;

    try {
        result = boost::lexical_cast<int32_t>(serialized_value);
        return true;
    }
    catch (boost::bad_lexical_cast&) {
    }
    return false;
}
void as_set_custom_int(int preset, std::string& key, int32_t val)
{
    if (!current_script->can_set()) return;
    std::string serialized_vars = get_custom_var_option(preset);
    set_custom_value(serialized_vars, key, std::to_string(val));
    set_custom_option(preset, serialized_vars);
}
bool as_get_custom_float(int preset, std::string& key, float& result)
{
    std::string serialized_value = get_custom_value(get_custom_var_option(preset), key);
    if (serialized_value.empty())
        return false;

    try {
        result = boost::lexical_cast<float>(serialized_value);
        return true;
    }
    catch (boost::bad_lexical_cast&) {
    }
    return false;
}
void as_set_custom_float(int preset, std::string& key, float f)
{
    if (!current_script->can_set()) return;
    std::string serialized_vars = get_custom_var_option(preset);
    set_custom_value(serialized_vars, key, std::to_string(f)); //FIXME: 0.0 format, with only 9 significant numbers
    set_custom_option(preset, serialized_vars);
}
bool as_get_custom_string(int preset, std::string& key, std::string& result)
{
    std::string serialized_value = get_custom_value(get_custom_var_option(preset), key);
    if (serialized_value.empty())
        return false;

    result = serialized_value;
    return true;
}
void as_set_custom_string(int preset, std::string& key, std::string& val)
{
    if (!current_script->can_set()) return;
    std::string serialized_vars = get_custom_var_option(preset);
    set_custom_value(serialized_vars, key, val);
    set_custom_option(preset, serialized_vars);
}

////// others //////

class ConfigAdapter : public ConfigBase
{
public:
    const ConfigBase* real_storage;
    ConfigAdapter(const ConfigBase* conf) : real_storage(conf) { this->parent = nullptr; }
    ConfigAdapter(const ConfigBase* conf, const ConfigBase* conf_parent) : real_storage(conf) { this->parent = conf_parent;  }
    virtual ~ConfigAdapter() { if (this->parent != nullptr) delete parent; }

    virtual const ConfigDef* def() const override { return real_storage->def(); }
    virtual ConfigOption* optptr(const t_config_option_key& opt_key, bool create = false) { return nullptr; }
    virtual t_config_option_keys keys() const override { return real_storage->keys(); };

    const ConfigOption* optptr(const t_config_option_key& opt_key) const override
    {
        const ConfigOption* opt = real_storage->optptr(opt_key);
            //if not find, try with the parent config.
        if (opt == nullptr && parent != nullptr)
            opt = parent->optptr(opt_key);
        return opt;
    }


};

float as_get_computed_float(std::string& key)
{
    if ((current_script->tab()->get_printer_technology() & PrinterTechnology::ptFFF) != 0) {
        ConfigAdapter fullconfig(
            &current_script->tab()->m_preset_bundle->fff_prints.get_edited_preset().config, 
            new ConfigAdapter(
                &current_script->tab()->m_preset_bundle->filaments.get_edited_preset().config, 
                new ConfigAdapter(&current_script->tab()->m_preset_bundle->printers.get_edited_preset().config)));
        try {
            return (float)fullconfig.get_computed_value(key, 0);
            //return (float)wxGetApp().plater()->fff_print().full_print_config().get_computed_value(key, 0);
        }
        catch (Exception e) {
            if(wxGetApp().initialized())
                BOOST_LOG_TRIVIAL(error) << "Error, can't compute fff option '" << key << "'";
        }

    } else {
        ConfigAdapter fullconfig(
            &current_script->tab()->m_preset_bundle->sla_prints.get_edited_preset().config,
            new ConfigAdapter(
                &current_script->tab()->m_preset_bundle->sla_materials.get_edited_preset().config,
                new ConfigAdapter(&current_script->tab()->m_preset_bundle->printers.get_edited_preset().config)));
        try {
            return (float)fullconfig.get_computed_value(key, 0);
            //return (float)wxGetApp().plater()->fff_print().full_print_config().get_computed_value(key, 0);
        }
        catch (Exception e) {
            if (wxGetApp().initialized())
                BOOST_LOG_TRIVIAL(error) << "Error, can't compute sla option '" << key << "'";
        }

    }
    return 0;
}

void as_ask_for_refresh()
{
    current_script->request_refresh();
}

bool as_is_enabled(std::string &key)
{
    Page *selected_page;
    Field *f = current_script->tab()->get_field(selected_page, key, -1);
    if (!f)
        return true;
    return f->is_enabled();
}

//function to reset a field
void as_back_initial_value(std::string& key) {
    current_script->add_to_reset(key);
}
void as_back_custom_initial_value(int preset_type, std::string& key) {
    if (!current_script->can_set()) return;
    std::string initial_serialized_vars;
    if (preset_type <= 0)
        initial_serialized_vars = (current_script->tab()->get_printer_technology() & PrinterTechnology::ptFFF) != 0
        ? current_script->tab()->m_preset_bundle->fff_prints.get_selected_preset().config.opt_string("print_custom_variables")
        : current_script->tab()->m_preset_bundle->sla_prints.get_selected_preset().config.opt_string("print_custom_variables");
    else if (preset_type == 1) {
        initial_serialized_vars = (current_script->tab()->get_printer_technology() & PrinterTechnology::ptFFF) != 0
            ? current_script->tab()->m_preset_bundle->filaments.get_selected_preset().config.opt_string("filament_custom_variables", size_t(0))
            : current_script->tab()->m_preset_bundle->sla_materials.get_selected_preset().config.opt_string("filament_custom_variables", size_t(0));
    } else initial_serialized_vars = current_script->tab()->m_preset_bundle->printers.get_selected_preset().config.opt_string("printer_custom_variables");
    std::string serialized_value = get_custom_value(initial_serialized_vars, key);
    std::string serialized_vars = get_custom_var_option(preset_type);
    set_custom_value(serialized_vars, key, serialized_value);
    set_custom_option(preset_type, serialized_vars);
}

/////// main script fucntions //////

//TODO: add "unset" function, that revert to last value (befoer a scripted set) if a set has been made since last not-scripted change.
void ScriptContainer::init(const std::string& tab_key, Tab* tab)
{
    m_tab = tab;
    const boost::filesystem::path ui_script_file = Slic3r::GUI::get_app_config()->layout_config_path() / (tab_key + ".as");
    if (boost::filesystem::exists(ui_script_file)) {
        //launch the engine if not yet
        if (m_script_engine.get() == nullptr) {

            m_script_engine.reset(AngelScript::asCreateScriptEngine());
            // Create the script engine
            if (m_script_engine.get() == nullptr)
            {
                throw ScriptError("Failed to create script engine.");
            }
            // The script compiler will send any compiler messages to the callback function
#ifdef AS_MAX_PORTABILITY
            m_script_engine->SetMessageCallback(WRAP_FN(as_message_callback), 0, AngelScript::asCALL_GENERIC);
#else
            m_script_engine->SetMessageCallback(AngelScript::asFUNCTION(as_message_callback), 0, AngelScript::asCALL_CDECL);
#endif
            // Configure the script engine with the callback function
            AngelScript::RegisterScriptArray(m_script_engine.get(), false);
            AngelScript::RegisterStdString(m_script_engine.get());
            AngelScript::RegisterStdStringUtils(m_script_engine.get());
            AngelScript::RegisterScriptMath(m_script_engine.get());

            //if (use_generic) {
            //    r = engine->RegisterGlobalFunction("void print(string & in)", WRAP_FN(print), asCALL_GENERIC); assert(r >= 0);
            //} else {
            //    r = engine->RegisterGlobalFunction("void print(string & in)", asFUNCTION(print), asCALL_CDECL); assert(r >= 0);
            //}
#ifdef AS_MAX_PORTABILITY
            m_script_engine.get()->RegisterGlobalFunction("void print(string &in)", WRAP_FN(as_print), AngelScript::asCALL_GENERIC);
            m_script_engine.get()->RegisterGlobalFunction("void print_float(float)", WRAP_FN(as_print_float), AngelScript::asCALL_GENERIC);
            //m_script_engine.get()->RegisterGlobalFunction("void register_key(string &in)", WRAP_FN(as_register_key), AngelScript::asCALL_GENERIC);

            m_script_engine.get()->RegisterGlobalFunction("bool get_bool(string &in)", WRAP_FN(as_get_bool), AngelScript::asCALL_GENERIC);
            m_script_engine.get()->RegisterGlobalFunction("void set_bool(string &in, bool new_val)", WRAP_FN(as_set_bool), AngelScript::asCALL_GENERIC);
            m_script_engine.get()->RegisterGlobalFunction("int get_int(string &in)", WRAP_FN(as_get_int), AngelScript::asCALL_GENERIC);
            m_script_engine.get()->RegisterGlobalFunction("void set_int(string &in, int new_val)", WRAP_FN(as_set_int), AngelScript::asCALL_GENERIC);
            m_script_engine.get()->RegisterGlobalFunction("float get_float(string &in)", WRAP_FN(as_get_float), AngelScript::asCALL_GENERIC);
            m_script_engine.get()->RegisterGlobalFunction("void set_float(string &in, float new_val)", WRAP_FN(as_set_float), AngelScript::asCALL_GENERIC);
            m_script_engine.get()->RegisterGlobalFunction("bool is_percent(string &in)", WRAP_FN(as_is_percent), AngelScript::asCALL_GENERIC);
            m_script_engine.get()->RegisterGlobalFunction("void set_percent(string &in, float new_val)", WRAP_FN(as_set_percent), AngelScript::asCALL_GENERIC);
            m_script_engine.get()->RegisterGlobalFunction("void get_string(string &in, string &out get_val)", WRAP_FN(as_get_string), AngelScript::asCALL_GENERIC);
            m_script_engine.get()->RegisterGlobalFunction("void set_string(string &in, string &in new_val)", WRAP_FN(as_set_string), AngelScript::asCALL_GENERIC);

            // for vector fields
            m_script_engine.get()->RegisterGlobalFunction("int size(string &in)",                           WRAP_FN(as_size), AngelScript::asCALL_GENERIC);
            m_script_engine.get()->RegisterGlobalFunction("void clear(string &in)",                           WRAP_FN(as_clear), AngelScript::asCALL_GENERIC);
            m_script_engine.get()->RegisterGlobalFunction("bool get_bool_idx(string &in, int idx)", WRAP_FN(as_get_bool_idx), AngelScript::asCALL_GENERIC);
            m_script_engine.get()->RegisterGlobalFunction("void set_bool_idx(string &in, int idx, bool new_val)", WRAP_FN(as_set_bool_idx), AngelScript::asCALL_GENERIC);
            m_script_engine.get()->RegisterGlobalFunction("int get_int_idx(string &in, int idx)", WRAP_FN(as_get_int_idx), AngelScript::asCALL_GENERIC);
            m_script_engine.get()->RegisterGlobalFunction("void set_int_idx(string &in, int idx, int new_val)", WRAP_FN(as_set_int_idx), AngelScript::asCALL_GENERIC);
            m_script_engine.get()->RegisterGlobalFunction("float get_float_idx(string &in, int idx)", WRAP_FN(as_get_float_idx), AngelScript::asCALL_GENERIC);
            m_script_engine.get()->RegisterGlobalFunction("void set_float_idx(string &in, int idx, float new_val)", WRAP_FN(as_set_float_idx), AngelScript::asCALL_GENERIC);
            m_script_engine.get()->RegisterGlobalFunction("bool is_percent_idx(string &in, int idx)", WRAP_FN(as_is_percent_idx), AngelScript::asCALL_GENERIC);
            m_script_engine.get()->RegisterGlobalFunction("void set_percent_idx(string &in, int idx, float new_val)", WRAP_FN(as_set_percent_idx), AngelScript::asCALL_GENERIC);
            m_script_engine.get()->RegisterGlobalFunction("void get_string_idx(string &in, int idx, string &out get_val)", WRAP_FN(as_get_string_idx), AngelScript::asCALL_GENERIC);
            m_script_engine.get()->RegisterGlobalFunction("void set_string_idx(string &in, int idx, string &in new_val)", WRAP_FN(as_set_string_idx), AngelScript::asCALL_GENERIC);


            m_script_engine.get()->RegisterGlobalFunction("bool as_get_custom_bool(int, string &in, bool &out)", WRAP_FN(as_get_custom_bool), AngelScript::asCALL_GENERIC);
            m_script_engine.get()->RegisterGlobalFunction("void as_set_custom_bool(int, string &in, bool)", WRAP_FN(as_set_custom_bool), AngelScript::asCALL_GENERIC);
            m_script_engine.get()->RegisterGlobalFunction("bool as_get_custom_int(int, string &in, int &out)", WRAP_FN(as_get_custom_int), AngelScript::asCALL_GENERIC);
            m_script_engine.get()->RegisterGlobalFunction("void as_set_custom_int(int, string &in, int)", WRAP_FN(as_set_custom_int), AngelScript::asCALL_GENERIC);
            m_script_engine.get()->RegisterGlobalFunction("bool as_get_custom_float(int, string &in, float &out)", WRAP_FN(as_get_custom_float), AngelScript::asCALL_GENERIC);
            m_script_engine.get()->RegisterGlobalFunction("void as_set_custom_float(int, string &in, float)", WRAP_FN(as_set_custom_float), AngelScript::asCALL_GENERIC);
            m_script_engine.get()->RegisterGlobalFunction("bool as_get_custom_string(int, string &in, string &out)", WRAP_FN(as_get_custom_string), AngelScript::asCALL_GENERIC);
            m_script_engine.get()->RegisterGlobalFunction("void as_set_custom_string(int, string &in, string &in)", WRAP_FN(as_set_custom_string), AngelScript::asCALL_GENERIC);

            m_script_engine.get()->RegisterGlobalFunction("float get_computed_float(string &in)", WRAP_FN(as_get_computed_float), AngelScript::asCALL_GENERIC);
            m_script_engine.get()->RegisterGlobalFunction("void back_initial_value(string &in)", WRAP_FN(as_back_initial_value), AngelScript::asCALL_GENERIC);
            m_script_engine.get()->RegisterGlobalFunction("void back_custom_initial_value(int, string &in)", WRAP_FN(as_back_custom_initial_value), AngelScript::asCALL_GENERIC);
            m_script_engine.get()->RegisterGlobalFunction("void ask_for_refresh()", WRAP_FN(as_ask_for_refresh), AngelScript::asCALL_GENERIC);
            m_script_engine.get()->RegisterGlobalFunction("bool is_enabled(string &in)", WRAP_FN(as_is_enabled), AngelScript::asCALL_GENERIC);

#else
            m_script_engine.get()->RegisterGlobalFunction("void print(string &in)",     AngelScript::asFUNCTION(as_print),          AngelScript::asCALL_CDECL);
            m_script_engine.get()->RegisterGlobalFunction("void print_float(float)",    AngelScript::asFUNCTION(as_print_float),    AngelScript::asCALL_CDECL);
            //m_script_engine.get()->RegisterGlobalFunction("void register_key(string &in)", AngelScript::asFUNCTION(as_register_key), AngelScript::asCALL_CDECL);

            m_script_engine.get()->RegisterGlobalFunction("bool get_bool(string &in)",                          AngelScript::asFUNCTION(as_get_bool),   AngelScript::asCALL_CDECL);
            m_script_engine.get()->RegisterGlobalFunction("void set_bool(string &in, bool new_val)",            AngelScript::asFUNCTION(as_set_bool),   AngelScript::asCALL_CDECL);
            m_script_engine.get()->RegisterGlobalFunction("int get_int(string &in)",                            AngelScript::asFUNCTION(as_get_int),    AngelScript::asCALL_CDECL);
            m_script_engine.get()->RegisterGlobalFunction("void set_int(string &in, int new_val)",              AngelScript::asFUNCTION(as_set_int),    AngelScript::asCALL_CDECL);
            m_script_engine.get()->RegisterGlobalFunction("float get_float(string &in)",                        AngelScript::asFUNCTION(as_get_float),  AngelScript::asCALL_CDECL);
            m_script_engine.get()->RegisterGlobalFunction("void set_float(string &in, float new_val)",          AngelScript::asFUNCTION(as_set_float),  AngelScript::asCALL_CDECL);
            m_script_engine.get()->RegisterGlobalFunction("bool is_percent(string &in)",                        AngelScript::asFUNCTION(as_is_percent), AngelScript::asCALL_CDECL);
            m_script_engine.get()->RegisterGlobalFunction("void set_percent(string &in, float new_val)",        AngelScript::asFUNCTION(as_set_percent),AngelScript::asCALL_CDECL);
            m_script_engine.get()->RegisterGlobalFunction("void get_string(string &in, string &out get_val)",   AngelScript::asFUNCTION(as_get_string), AngelScript::asCALL_CDECL);
            m_script_engine.get()->RegisterGlobalFunction("void set_string(string &in, string &in new_val)",    AngelScript::asFUNCTION(as_set_string), AngelScript::asCALL_CDECL);

            // for vector fields
            m_script_engine.get()->RegisterGlobalFunction("int size(string &in)",                           AngelScript::asFUNCTION(as_size), AngelScript::asCALL_CDECL);
            m_script_engine.get()->RegisterGlobalFunction("void clear(string &in)",                           AngelScript::asFUNCTION(as_clear), AngelScript::asCALL_CDECL);
            m_script_engine.get()->RegisterGlobalFunction("bool get_bool_idx(string &in, int idx)",                          AngelScript::asFUNCTION(as_get_bool_idx),   AngelScript::asCALL_CDECL);
            m_script_engine.get()->RegisterGlobalFunction("void set_bool_idx(string &in, int idx, bool new_val)",            AngelScript::asFUNCTION(as_set_bool_idx),   AngelScript::asCALL_CDECL);
            m_script_engine.get()->RegisterGlobalFunction("int get_int_idx(string &in, int idx)",                            AngelScript::asFUNCTION(as_get_int_idx),    AngelScript::asCALL_CDECL);
            m_script_engine.get()->RegisterGlobalFunction("void set_int_idx(string &in, int idx, int new_val)",              AngelScript::asFUNCTION(as_set_int_idx),    AngelScript::asCALL_CDECL);
            m_script_engine.get()->RegisterGlobalFunction("float get_float_idx(string &in, int idx)",           AngelScript::asFUNCTION(as_get_float_idx), AngelScript::asCALL_CDECL);
            m_script_engine.get()->RegisterGlobalFunction("void set_float_idx(string &in, int idx, float new_val)", AngelScript::asFUNCTION(as_set_float_idx), AngelScript::asCALL_CDECL);
            m_script_engine.get()->RegisterGlobalFunction("bool is_percent_idx(string &in, int idx)",                        AngelScript::asFUNCTION(as_is_percent_idx), AngelScript::asCALL_CDECL);
            m_script_engine.get()->RegisterGlobalFunction("void set_percent_idx(string &in, int idx, float new_val)",        AngelScript::asFUNCTION(as_set_percent_idx),AngelScript::asCALL_CDECL);
            m_script_engine.get()->RegisterGlobalFunction("void get_string_idx(string &in, int idx, string &out get_val)",   AngelScript::asFUNCTION(as_get_string_idx), AngelScript::asCALL_CDECL);
            m_script_engine.get()->RegisterGlobalFunction("void set_string_idx(string &in, int idx, string &in new_val)",    AngelScript::asFUNCTION(as_set_string_idx), AngelScript::asCALL_CDECL);

            m_script_engine.get()->RegisterGlobalFunction("bool get_custom_bool(int, string &in, bool &out)",       AngelScript::asFUNCTION(as_get_custom_bool),    AngelScript::asCALL_CDECL);
            m_script_engine.get()->RegisterGlobalFunction("void set_custom_bool(int, string &in, bool)",            AngelScript::asFUNCTION(as_set_custom_bool),    AngelScript::asCALL_CDECL);
            m_script_engine.get()->RegisterGlobalFunction("bool get_custom_int(int, string &in, int &out)",         AngelScript::asFUNCTION(as_get_custom_int),     AngelScript::asCALL_CDECL);
            m_script_engine.get()->RegisterGlobalFunction("void set_custom_int(int, string &in, int)",              AngelScript::asFUNCTION(as_set_custom_int),     AngelScript::asCALL_CDECL);
            m_script_engine.get()->RegisterGlobalFunction("bool get_custom_float(int, string &in, float &out)",     AngelScript::asFUNCTION(as_get_custom_float),   AngelScript::asCALL_CDECL);
            m_script_engine.get()->RegisterGlobalFunction("void set_custom_float(int, string &in, float)",          AngelScript::asFUNCTION(as_set_custom_float),   AngelScript::asCALL_CDECL);
            m_script_engine.get()->RegisterGlobalFunction("bool get_custom_string(int, string &in, string &out)",   AngelScript::asFUNCTION(as_get_custom_string),  AngelScript::asCALL_CDECL);
            m_script_engine.get()->RegisterGlobalFunction("void set_custom_string(int, string &in, string &in)",    AngelScript::asFUNCTION(as_set_custom_string),  AngelScript::asCALL_CDECL);

            m_script_engine.get()->RegisterGlobalFunction("float get_computed_float(string &in)",   AngelScript::asFUNCTION(as_get_computed_float), AngelScript::asCALL_CDECL);
            m_script_engine.get()->RegisterGlobalFunction("void back_initial_value(string &in)",    AngelScript::asFUNCTION(as_back_initial_value), AngelScript::asCALL_CDECL);
            m_script_engine.get()->RegisterGlobalFunction("void back_custom_initial_value(int, string &in)",    AngelScript::asFUNCTION(as_back_custom_initial_value), AngelScript::asCALL_CDECL);
            m_script_engine.get()->RegisterGlobalFunction("void ask_for_refresh()",                 AngelScript::asFUNCTION(as_ask_for_refresh),    AngelScript::asCALL_CDECL);
            m_script_engine.get()->RegisterGlobalFunction("bool is_enabled(string &in)",                        AngelScript::asFUNCTION(as_is_enabled), AngelScript::asCALL_CDECL);
#endif
        }

        //m_script_module = m_script_engine->GetModule(tab_key.c_str(), AngelScript::asGM_CREATE_IF_NOT_EXISTS);
        AngelScript::CScriptBuilder builder;
        int res = builder.StartNewModule(m_script_engine.get(), tab_key.c_str());
        if (res < 0) throw CompileErrorException("Error, can't build the script for tab " + tab_key);
        // Let the builder load the script, and do the necessary pre-processing (include files, etc)
        //res = builder.AddSectionFromFile(ui_script_file.string().c_str()); //seems to be problematic on cyrillic locale
        {
            //std::string all_file;
            //boost::filesystem::load_string_file(ui_script_file, all_file);
	        boost::nowide::ifstream file(ui_script_file.string());
	        std::string all_file { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
            res = builder.AddSectionFromMemory(ui_script_file.string().c_str(), all_file.c_str(), (unsigned int)(all_file.length()), 0);
        }
        if (res < 0) throw CompileErrorException("Error, can't build the script for tab " + tab_key);
        res = builder.BuildModule();
        if (res < 0) throw CompileErrorException("Error, can't build the script for tab " + tab_key);
        m_script_module = m_script_engine->GetModule(tab_key.c_str(), AngelScript::asGM_ONLY_IF_EXISTS);
        //AngelScript::asIScriptFunction* func = m_script_module->GetFunctionByDecl("void main()");
        //AngelScript::asIScriptContext* ctx = m_script_engine->CreateContext();
        //ctx->Prepare(func);

        //script_current_tab = this;
        //res = ctx->Execute();
        //script_current_tab = nullptr;
        //std::cout << "\nres is " << res << "\n";
        m_initialized = true;
    } else {
        BOOST_LOG_TRIVIAL(warning) << "Warning, can't find file script '" << ui_script_file.string() << "', is it needed? Even if something need it, it won't execute.";
        m_script_module = nullptr;
        disable();
    }
}

std::string get_type_name(ConfigOptionType type)
{
    switch (type) {
    case coFloat: return "float";
    case coFloats: return "float";
    case coInt: return "int";
    case coInts: return "int";
    case coString: return "string &in";
    case coStrings: return "string &in";
    case coPercent: return "float";
    case coPercents: return "float";
    case coFloatOrPercent: return "float, bool";
    case coFloatsOrPercents: return "float, bool";
    case coPoint: return "float, float";
    case coPoints: return "float, float";
    case coPoint3: return "float, float, float";
    case coBool: return "bool";
    case coBools: return "bool";
    case coEnum: return "string &in, int idx";
    default: return "";
    }
}

void ScriptContainer::call_script_function_set(const ConfigOptionDef& def, const boost::any& value)
{
    if (value.empty() || !is_intialized())
        return;
    std::string func_name = ("void " + def.opt_key + "_set(" + get_type_name(def.type) + ")");
    AngelScript::asIScriptFunction* func = m_script_module->GetFunctionByDecl(func_name.c_str());
    if (func == nullptr) {
        BOOST_LOG_TRIVIAL(error) << "Error, can't find function '" << func_name << "' in the script file";
        return;
    }
    AngelScript::asIScriptContext* ctx = m_script_engine->CreateContext();
    if (ctx == nullptr) {
        BOOST_LOG_TRIVIAL(error) << "Error, can't create script context for function '" << func_name << "'";
        return;
    }
    ctx->Prepare(func);
    std::string str_arg;
    switch (def.type) {
    case coBools: ctx->SetArgByte(0, boost::any_cast<uint8_t>(value)); break;
    case coBool: ctx->SetArgByte(0, boost::any_cast<bool>(value)); break;
    case coInt:
    case coInts: ctx->SetArgDWord(0, boost::any_cast<int32_t>(value)); break;
    case coPercent:
    case coPercents:
    case coFloat:
    case coFloats: ctx->SetArgFloat(0, (float)boost::any_cast<double>(value)); break;
    case coFloatOrPercent:
    case coFloatsOrPercents: {
        FloatOrPercent fl_percent = boost::any_cast<FloatOrPercent>(value);
        ctx->SetArgDWord(0, boost::any_cast<float>((float) fl_percent.value));
        ctx->SetArgByte(1, boost::any_cast<bool>(fl_percent.percent));
        break;
    }
    case coPoint:
    case coPoints: { 
        Vec2d vec = boost::any_cast<Vec2d>(value);
        ctx->SetArgFloat(0, (float) vec.x());
        ctx->SetArgFloat(1, (float) vec.y());
        break;
    }
    case coPoint3: { 
        Vec3d vec = boost::any_cast<Vec3d>(value);
        ctx->SetArgFloat(0, (float) vec.x());
        ctx->SetArgFloat(1, (float) vec.y());
        ctx->SetArgFloat(2, (float) vec.z());
        break;
    }
    case coString:
    case coStrings: {
        str_arg = boost::any_cast<std::string>(value);
        ctx->SetArgAddress(0, &str_arg);
        break;
    }
    case coEnum: {
        int32_t enum_idx = boost::any_cast<std::int32_t>(value);
        assert(def.enum_def);
        assert(enum_idx < def.enum_def->values().size());
        if (enum_idx >= 0 && def.enum_def && enum_idx < def.enum_def->values().size()) {
            str_arg = def.enum_def->label(enum_idx);
            ctx->SetArgAddress(0, &str_arg);
            ctx->SetArgDWord(1, enum_idx);
        }
        break;
    }
    }
    m_need_refresh = false;
    m_to_update.clear();
    for (Tab* tab : wxGetApp().tabs_list)
        if (tab->completed())
            m_to_update[tab->type()] = {};
    m_can_set = true;
    // init globals for script exec (TODO find a way to change that)
    assert(current_script == nullptr);
    {
        std::lock_guard<std::mutex> lock(current_script_mutex);
        current_script = this;
        // exec
        /*int res = */ ctx->Execute();
        current_script = nullptr;
    }
    m_can_set = false;
    std::map<Preset::Type, DynamicPrintConfig> to_update = m_to_update;
    m_to_update.clear();
    auto to_reset = m_to_reset_initial;
    m_to_reset_initial.clear();

    if(ctx->GetState() == asEContextState::asEXECUTION_EXCEPTION)
        return;

    //update the tabs from the results
    for (auto& data : to_update) {
        Tab* tab = wxGetApp().get_tab(data.first);
        //also reset
        if (!to_reset.empty()) {
            const DynamicPrintConfig& initial_conf = tab->m_presets->get_selected_preset().config;
            for (size_t key_idx = 0; key_idx != to_reset.size(); ++key_idx) {
                const std::string& key = to_reset[key_idx];
                if (initial_conf.has(key)) {
                    data.second.set_key_value(key, initial_conf.option(key)->clone());
                    to_reset.erase(to_reset.begin() + key_idx);
                    key_idx--;
                }
            }
        }
        tab->load_config(data.second);
    }
    //also call for value_changed, as it's not really a load but a change
    for (const auto& data : to_update) {
        Tab* tab = wxGetApp().get_tab(data.first);
        for (auto opt_key : data.second.keys()) {
            tab->on_value_change(opt_key, data.second.option(opt_key)->get_any(-1));
        }
    }
    // refresh the field if needed
    if (m_need_refresh && m_tab) {
        Field* f = m_tab->get_field(def.opt_key);
        if (f != nullptr) {
            f->set_any_value(call_script_function_get_value(def), false);
        }
    }
}

bool ScriptContainer::call_script_function_reset(const ConfigOptionDef& def)
{
    std::string func_name = ("void " + def.opt_key + "_reset()");
    AngelScript::asIScriptFunction* func = m_script_module->GetFunctionByDecl(func_name.c_str());
    if (func == nullptr) {
        return false;
    }
    AngelScript::asIScriptContext* ctx = m_script_engine->CreateContext();
    if (ctx == nullptr) {
        BOOST_LOG_TRIVIAL(error) << "Error, can't create script context for function '" << func_name << "'";
        return false;
    }
    ctx->Prepare(func);
    m_can_set = true;
    // init globals for script exec (TODO find a way to change that)
    assert(current_script == nullptr);
    {
        std::lock_guard<std::mutex> lock(current_script_mutex);
        current_script = this;
        // exec
        /*int res = */ ctx->Execute();
        current_script = nullptr;
    }
    m_can_set = false;
    std::map<Preset::Type, DynamicPrintConfig> to_update = m_to_update;
    m_to_update.clear();
    auto to_reset = m_to_reset_initial;
    m_to_reset_initial.clear();

    //update the tabs from the results
    for (auto& data : to_update) {
        Tab* tab = wxGetApp().get_tab(data.first);
        //also reset
        if (!to_reset.empty()) {
            const DynamicPrintConfig& initial_conf = tab->m_presets->get_selected_preset().config;
            for (size_t key_idx = 0; key_idx != to_reset.size(); ++key_idx) {
                const std::string& key = to_reset[key_idx];
                if (initial_conf.has(key)) {
                    data.second.set_key_value(key, initial_conf.option(key)->clone());
                    to_reset.erase(to_reset.begin() + key_idx);
                    key_idx--;
                }
            }
        }
        tab->load_config(data.second);
    }
    //also call for value_changed, as it's not really a load but a change
    for (const auto& data : to_update) {
        Tab* tab = wxGetApp().get_tab(data.first);
        for (auto opt_key : data.second.keys()) {
            tab->on_value_change(opt_key, data.second.option(opt_key)->get_any(-1));
        }
    }
    // refresh the field if needed
    if (m_need_refresh && m_tab) {
        Field* f = m_tab->get_field(def.opt_key);
        if (f != nullptr) {
            f->set_any_value(call_script_function_get_value(def), false);
        }
    }
    return true;
}

boost::any ScriptContainer::call_script_function_get_value(const ConfigOptionDef& def)
{
    if (!is_intialized())
        return boost::any();

    std::string func_name;

    switch (def.type) {
    case coBool:
    case coBools:
    case coInt:
    case coInts: func_name = "int"; break;
    case coPercent:
    case coPercents:
    case coFloat:
    case coFloats: 
    case coFloatOrPercent:
    case coFloatsOrPercents: func_name = "float"; break;
    case coPoint:
    case coPoints: func_name = "float"; break; //FIXME
    case coPoint3: func_name = "float"; break;
    case coString:
    case coStrings:func_name = "void"; break;
    case coEnum: func_name = "int"; break;
    default:
        assert(false);
    }
    func_name += (" " + def.opt_key + "_get(");
    switch (def.type) {
    case coFloatOrPercent:
    case coFloatsOrPercents: func_name += "bool &out"; break;
    case coString:
    case coStrings:
    case coEnum: func_name += "string &out"; break;
    default:;
    }
    func_name += ")";
    AngelScript::asIScriptFunction* func = m_script_module->GetFunctionByDecl(func_name.c_str());
    if (func == nullptr) {
        BOOST_LOG_TRIVIAL(error) << "Error, can't find function '" << func_name << "' in the script file";
        return boost::any{};
    }
    AngelScript::asIScriptContext* ctx = m_script_engine->CreateContext();
    if (ctx == nullptr) {
        BOOST_LOG_TRIVIAL(error) << "Error, can't create script context for function '" << func_name << "'";
        return boost::any{};
    }
    ctx->Prepare(func);
    std::string ret_str;
    bool ret_percent = false;
    switch (def.type) {
    case coFloatOrPercent:
    case coFloatsOrPercents: ctx->SetArgAddress(0, &ret_percent); break;
    case coString:
    case coStrings:
    case coEnum: ctx->SetArgObject(0, &ret_str); break;
    }
    // init globals for script exec (TODO find a way to change that)
    assert(current_script == nullptr);
    {
        std::lock_guard<std::mutex> lock(current_script_mutex);
        current_script = this;
        m_need_refresh = false;
        // exec
        int res        = ctx->Execute();
        current_script = nullptr;
    }
    int32_t ret_int;
    float ret_float;
    boost::any opt_val;
    switch (def.type) {
    case coBool:
    case coBools: {
        ret_int = ctx->GetReturnDWord();
        opt_val = uint8_t(ret_int < 0 ? 2 : ret_int);
        break;
    }
    case coInt:
    case coInts: {
        ret_int = ctx->GetReturnDWord();
        opt_val = int32_t(ret_int);
        break;
    } // SpinCtrl
    case coString:
    case coStrings: {
        opt_val = ret_str;
        break;
    } // TextCtrl
    case coPercent:
    case coPercents:
    case coFloat:
    case coFloats: {
        opt_val = double(ctx->GetReturnFloat());
        break;
    }
    case coFloatOrPercent:
    case coFloatsOrPercents:
    {
        ret_float = ctx->GetReturnFloat();
        opt_val   = FloatOrPercent{ret_float, ret_percent};
        break;
    }
    case coPoint:
    case coPoints: {
        double pt_x = ctx->GetReturnFloat();
        opt_val     = Vec2d{pt_x, pt_x}; // FIXME
        break;
    } // FIXME PointCtrl
    case coPoint3: {
        double pt_x = ctx->GetReturnFloat();
        opt_val     = Vec3d{pt_x, pt_x, pt_x};
        break;
    }
    case coEnum: { 
        ret_int = ctx->GetReturnDWord();
        assert(def.enum_def && ret_int < def.enum_def->values().size());
        if (ret_int >= 0 && def.enum_def && ret_int < def.enum_def->values().size()) {
            opt_val = int32_t(ret_int);
        } else {
            opt_val = int32_t(0);
            std::optional<int> idx;
            if (def.enum_def->is_valid_closed_enum()) {
                idx = def.enum_def->value_to_index(ret_str);
            } else {
                idx = def.enum_def->label_to_index(ret_str);
            }
            assert(idx);
            if(idx)
                opt_val = int32_t(*idx);
        }
        break; //Choice
    }
    }
    if (m_need_refresh) {
        refresh(def, opt_val);
    }
    if (opt_val.empty()) {
        std::cout << "Error nullptr for script\n";
    }
    return opt_val;
}

void ScriptContainer::refresh(const ConfigOptionDef& def, boost::any value)
{
    auto it = std::find(m_currently_reset.begin(), m_currently_reset.end(), def.opt_key);
    // Don't refresh while in refresh
    if (it != m_currently_reset.end())
        return;
    //if bool, need to change the int in bool
    if (def.type == coBool || def.type == coBools) {
        value = bool(boost::any_cast<uint8_t>(value) != 0);
    }
    m_currently_reset.push_back(def.opt_key);
    call_script_function_set(def, value);
    //remove opt_key
    it = std::find(m_currently_reset.begin(), m_currently_reset.end(), def.opt_key);
    if (it != m_currently_reset.end())
        m_currently_reset.erase(it);
}

 bool ScriptContainer::call_script_function_is_enable(const ConfigOptionDef &def)
{
    std::string                     func_name = ("bool " + def.opt_key + "_is_enabled()");
    AngelScript::asIScriptFunction *func      = m_script_module->GetFunctionByDecl(func_name.c_str());
    if (func == nullptr) {
        // default true
        return true;
    }
    AngelScript::asIScriptContext *ctx = m_script_engine->CreateContext();
    if (ctx == nullptr) {
        BOOST_LOG_TRIVIAL(error) << "Error, can't create script context for function '" << func_name << "'";
        return true;
    }
    ctx->Prepare(func);
    // init globals for script exec (TODO find a way to change that)
    assert(current_script == nullptr);
    {
        std::lock_guard<std::mutex> lock(current_script_mutex);
        current_script = this;
        // exec
        /*int res = */ ctx->Execute();
        current_script = nullptr;
    }
    uint8_t ret = ctx->GetReturnByte();
    return ret != 0;
}

//TODO find a way to use the depends_on to add the same lock & points as real configoption in the gui

} } }//namespace Slic3r Gui script

