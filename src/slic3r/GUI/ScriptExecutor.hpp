#ifndef slic3r_ScriptExecutor_hpp_
#define slic3r_ScriptExecutor_hpp_

#include "libslic3r/Config.hpp"
#include "OptionsGroup.hpp"

#include <angelscript/include/angelscript.h>
#include <angelscript/add_on/scriptbuilder/scriptbuilder.h>

namespace Slic3r { namespace GUI {

class Tab;

namespace script {

// Base for all exceptions thrown by the sript exec layer.
class ScriptError : public Slic3r::RuntimeError
{
public:
    using RuntimeError::RuntimeError;
};
// Specialization of std::exception to indicate that a scripot file is badly written.
class CompileErrorException : public ScriptError
{
public:
    CompileErrorException() : ScriptError("Bad script file exception") {}
    CompileErrorException(const std::string &opt_key)
        : ScriptError(std::string("Bad script file exception: ") + opt_key)
    {}
};

class ScriptContainer
{
    // for exec
    std::vector<std::string>                   m_currently_reset;
    bool                                       m_need_refresh = false;
    bool                                       m_can_set      = false;
    std::map<Preset::Type, DynamicPrintConfig> m_to_update;
    std::vector<std::string>                   m_to_reset_initial;

    // main vars
    Tab *m_tab;
    bool m_initialized = false;

public:
    ScriptContainer() {}
    inline static AngelScript::PtrRelease<AngelScript::asIScriptEngine> m_script_engine;
    AngelScript::asIScriptModule *                                      m_script_module{nullptr};

    void                                        disable() { m_initialized = false; }
    bool                                        is_intialized() { return m_initialized; }
    const Tab *                                 tab() { return m_tab; }
    std::map<Preset::Type, DynamicPrintConfig> &to_update() { return m_to_update; }
    bool                                        can_set() { return m_can_set; }
    void                                        request_refresh() { m_need_refresh = true; }
    void add_to_reset(const std::string &key) { m_to_reset_initial.push_back(key); }

    void init(const std::string &tab_key, Tab *tab);
    void call_script_function_set(const ConfigOptionDef &def, const boost::any &value);
    void refresh(const ConfigOptionDef &def, boost::any value);
    // return false if the function doesn't exists.
    bool call_script_function_reset(const ConfigOptionDef &def);
    // void call_script_function_refresh(const std::string& def_id);
    boost::any call_script_function_get_value(const ConfigOptionDef &def);
    bool       call_script_function_is_enable(const ConfigOptionDef &def);
};

} // namespace script
}} // namespace Slic3r::GUI

#endif //slic3r_ScriptExecutor_hpp_
