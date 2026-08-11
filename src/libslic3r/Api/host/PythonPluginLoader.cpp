///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

// PythonPluginLoader is the native bridge between Python plugin objects and
// the public C plugin ABI. It loads scripts bundled with the bridge and scans
// sibling installed packages that contain description.ini, version.ini and a
// Python entry point but no native plugin library. External scripts are loaded
// by absolute path under unique module names, then registered with their own
// package root so relative resources and diagnostics belong to the package
// which supplied them.

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <dlfcn.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif
#endif

#if defined(_MSC_VER) && defined(_DEBUG)
#define SLIC3R_RESTORE_DEBUG_MACRO
#undef _DEBUG
#endif
#include <Python.h>
#if defined(SLIC3R_RESTORE_DEBUG_MACRO)
#define _DEBUG
#undef SLIC3R_RESTORE_DEBUG_MACRO
#endif

#include <cstdint>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>

#include "libslic3r/Api/plugin/c/slic3r_plugin.h"

namespace {

using OrchestratorRegisterPluginFn = void (*)(orchestrator_handle *, plugin_instance);
using OrchestratorRegisterPluginFromPackageFn = void (*)(orchestrator_handle *, plugin_instance, const char *);

std::vector<std::unique_ptr<class PythonPlugin>> s_python_plugins;

struct PythonUsedConfigKey
{
    std::string key;
    raw_config_option_type type = RAW_CO_NONE;
    raw_container_type container_type = RAW_CONTAINER_TYPE_NONE;
    raw_option_preset_type option_preset_type = RAW_PRESET_TYPE_NONE;
};

bool ensure_python_initialized_for_plugins()
{
    if (Py_IsInitialized())
        return true;

    Py_Initialize();
    if (!Py_IsInitialized())
        return false;

    // Py_Initialize() leaves the startup thread holding the Python GIL. Plugin
    // callbacks run later from slicing/background threads and use
    // PyGILState_Ensure() before entering Python. If the startup thread keeps
    // the initial GIL, those callbacks block forever. Release it once here; all
    // later Python entry points explicitly acquire and release the GIL.
    PyEval_SaveThread();
    return true;
}

boost::filesystem::path current_module_path()
{
#ifdef _WIN32
    HMODULE module = NULL;
    const DWORD flags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
    if (GetModuleHandleExW(flags, reinterpret_cast<LPCWSTR>(&current_module_path), &module) == 0 || module == NULL)
        return {};

    std::vector<wchar_t> buffer(MAX_PATH);
    DWORD length = 0;
    for (;;) {
        length = GetModuleFileNameW(module, buffer.data(), DWORD(buffer.size()));
        if (length == 0)
            return {};
        if (length < buffer.size() - 1)
            break;
        buffer.resize(buffer.size() * 2);
    }
    return boost::filesystem::path(std::wstring(buffer.data(), length));
#else
    Dl_info info = {};
    if (dladdr(reinterpret_cast<void *>(&current_module_path), &info) == 0 || info.dli_fname == nullptr)
        return {};
    return boost::filesystem::path(info.dli_fname);
#endif
}

boost::filesystem::path current_executable_path()
{
#ifdef _WIN32
    std::vector<wchar_t> buffer(MAX_PATH);
    DWORD length = 0;
    for (;;) {
        length = GetModuleFileNameW(NULL, buffer.data(), DWORD(buffer.size()));
        if (length == 0)
            return {};
        if (length < buffer.size() - 1)
            break;
        buffer.resize(buffer.size() * 2);
    }
    return boost::filesystem::path(std::wstring(buffer.data(), length));
#else
#ifdef __APPLE__
    uint32_t length = 0;
    _NSGetExecutablePath(nullptr, &length);
    std::vector<char> buffer(length);
    if (_NSGetExecutablePath(buffer.data(), &length) != 0)
        return {};
    return boost::filesystem::canonical(boost::filesystem::path(buffer.data()));
#else
    std::vector<char> buffer(1024);
    for (;;) {
        const ssize_t length = readlink("/proc/self/exe", buffer.data(), buffer.size());
        if (length < 0)
            return {};
        if (size_t(length) < buffer.size())
            return boost::filesystem::path(std::string(buffer.data(), size_t(length)));
        buffer.resize(buffer.size() * 2);
    }
#endif
#endif
}

PyObject *python_path_from_boost(const boost::filesystem::path &path)
{
#ifdef _WIN32
    const std::wstring native = path.wstring();
    return PyUnicode_FromWideChar(native.c_str(), Py_ssize_t(native.size()));
#else
    const std::string native = path.string();
    return PyUnicode_DecodeFSDefault(native.c_str());
#endif
}

void append_python_path(const boost::filesystem::path &path)
{
    PyObject *sys_path = PySys_GetObject("path");
    if (sys_path == nullptr || !PyList_Check(sys_path))
        return;

    PyObject *py_path = python_path_from_boost(path);
    if (py_path == nullptr) {
        PyErr_Print();
        return;
    }

    const int contains = PySequence_Contains(sys_path, py_path);
    if (contains == 0 && PyList_Append(sys_path, py_path) != 0)
        PyErr_Print();
    else if (contains < 0)
        PyErr_Clear();

    Py_DECREF(py_path);
}

std::string py_object_to_string(PyObject *object)
{
    if (object == nullptr)
        return {};

    PyObject *bytes = PyUnicode_AsUTF8String(object);
    if (bytes == nullptr) {
        PyErr_Clear();
        return {};
    }

    const char *text = PyBytes_AsString(bytes);
    std::string out = text != nullptr ? text : "";
    Py_DECREF(bytes);
    return out;
}

std::string string_attribute(PyObject *object, const char *name, const char *fallback = "")
{
    PyObject *attr = PyObject_GetAttrString(object, name);
    if (attr == nullptr) {
        PyErr_Clear();
        return fallback != nullptr ? fallback : "";
    }

    std::string out = py_object_to_string(attr);
    Py_DECREF(attr);
    return out.empty() && fallback != nullptr ? fallback : out;
}

int int_attribute(PyObject *object, const char *name, int fallback)
{
    PyObject *attr = PyObject_GetAttrString(object, name);
    if (attr == nullptr) {
        PyErr_Clear();
        return fallback;
    }

    const long value = PyLong_AsLong(attr);
    if (PyErr_Occurred()) {
        PyErr_Clear();
        Py_DECREF(attr);
        return fallback;
    }

    Py_DECREF(attr);
    return int(value);
}

bool py_object_to_long(PyObject *object, long &out)
{
    if (object == nullptr)
        return false;

    const long value = PyLong_AsLong(object);
    if (PyErr_Occurred()) {
        PyErr_Clear();
        return false;
    }

    out = value;
    return true;
}

std::vector<std::string> string_list_attribute(PyObject *object, const char *name)
{
    std::vector<std::string> out;
    PyObject *attr = PyObject_GetAttrString(object, name);
    if (attr == nullptr) {
        PyErr_Clear();
        return out;
    }

    PyObject *iterator = PyObject_GetIter(attr);
    Py_DECREF(attr);
    if (iterator == nullptr) {
        PyErr_Clear();
        return out;
    }

    for (;;) {
        PyObject *item = PyIter_Next(iterator);
        if (item == nullptr)
            break;
        out.emplace_back(py_object_to_string(item));
        Py_DECREF(item);
    }
    if (PyErr_Occurred())
        PyErr_Clear();

    Py_DECREF(iterator);
    return out;
}

PythonUsedConfigKey used_config_key_from_sequence(PyObject *item)
{
    PythonUsedConfigKey out;
    const Py_ssize_t size = PySequence_Size(item);
    if (size < 2) {
        PyErr_Clear();
        return out;
    }

    PyObject *key = PySequence_GetItem(item, 0);
    PyObject *type = PySequence_GetItem(item, 1);
    PyObject *container = size > 2 ? PySequence_GetItem(item, 2) : nullptr;
    PyObject *preset = size > 3 ? PySequence_GetItem(item, 3) : nullptr;

    out.key = py_object_to_string(key);
    long value = 0;
    if (py_object_to_long(type, value))
        out.type = raw_config_option_type(value);
    if (py_object_to_long(container, value))
        out.container_type = raw_container_type(value);
    if (py_object_to_long(preset, value))
        out.option_preset_type = raw_option_preset_type(value);

    Py_XDECREF(key);
    Py_XDECREF(type);
    Py_XDECREF(container);
    Py_XDECREF(preset);
    return out;
}

PythonUsedConfigKey used_config_key_from_dict(PyObject *item)
{
    PythonUsedConfigKey out;
    PyObject *key = PyDict_GetItemString(item, "key");
    PyObject *type = PyDict_GetItemString(item, "type");
    PyObject *container = PyDict_GetItemString(item, "container_type");
    PyObject *preset = PyDict_GetItemString(item, "option_preset_type");

    out.key = py_object_to_string(key);
    long value = 0;
    if (py_object_to_long(type, value))
        out.type = raw_config_option_type(value);
    if (py_object_to_long(container, value))
        out.container_type = raw_container_type(value);
    if (py_object_to_long(preset, value))
        out.option_preset_type = raw_option_preset_type(value);
    return out;
}

std::vector<PythonUsedConfigKey> used_config_key_list_attribute(PyObject *object, const char *name)
{
    std::vector<PythonUsedConfigKey> out;
    PyObject *attr = PyObject_GetAttrString(object, name);
    if (attr == nullptr) {
        PyErr_Clear();
        return out;
    }

    PyObject *iterator = PyObject_GetIter(attr);
    Py_DECREF(attr);
    if (iterator == nullptr) {
        PyErr_Clear();
        return out;
    }

    for (;;) {
        PyObject *item = PyIter_Next(iterator);
        if (item == nullptr)
            break;

        if (PyDict_Check(item)) {
            out.emplace_back(used_config_key_from_dict(item));
        } else if (PySequence_Check(item) && !PyUnicode_Check(item)) {
            out.emplace_back(used_config_key_from_sequence(item));
        } else {
            // Old string-only declarations are not enough for the typed ABI.
            // Keep the key so the host can report the offending plugin/key.
            PythonUsedConfigKey key;
            key.key = py_object_to_string(item);
            out.emplace_back(std::move(key));
        }
        Py_DECREF(item);
    }
    if (PyErr_Occurred())
        PyErr_Clear();

    Py_DECREF(iterator);
    return out;
}

PyObject *pointer_to_python_uint(const void *ptr)
{
    return PyLong_FromUnsignedLongLong(static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(ptr)));
}

void call_python_void(PyObject *object, const char *method_name, PyObject *args, const std::string &plugin_id)
{
    PyObject *method = PyObject_GetAttrString(object, method_name);
    if (method == nullptr) {
        PyErr_Clear();
        return;
    }

    if (!PyCallable_Check(method)) {
        BOOST_LOG_TRIVIAL(warning) << "Python plugin '" << plugin_id << "' attribute "
                                   << method_name << " is not callable.";
        Py_DECREF(method);
        return;
    }

    PyObject *result = PyObject_CallObject(method, args);
    if (result == nullptr) {
        PyErr_Print();
        BOOST_LOG_TRIVIAL(error) << "Python plugin '" << plugin_id << "' failed in " << method_name << "().";
        Py_DECREF(method);
        return;
    }

    Py_DECREF(result);
    Py_DECREF(method);
}

class PythonPlugin
{
public:
    explicit PythonPlugin(PyObject *plugin)
        : m_plugin(plugin)
        , m_id(string_attribute(plugin, "plugin_id", "python.unnamed"))
        , m_name(string_attribute(plugin, "name", m_id.c_str()))
        , m_description(string_attribute(plugin, "description", ""))
        , m_exclusive_group(string_attribute(plugin, "exclusive_group", ""))
        , m_exclusive_group_label(string_attribute(plugin, "exclusive_group_label", ""))
        , m_exclusive_group_tooltip(string_attribute(plugin, "exclusive_group_tooltip", ""))
        , m_step(slicing_step_t(int_attribute(plugin, "step", STEP_POST_SLICING)))
        , m_priority(int_attribute(plugin, "priority", 0))
        , m_dependencies(string_list_attribute(plugin, "dependencies"))
        , m_used_config_keys(used_config_key_list_attribute(plugin, "used_config_keys"))
        , m_defined_config_keys(string_list_attribute(plugin, "defined_config_keys"))
    {
        Py_INCREF(m_plugin);
        for (const std::string &dependency : m_dependencies)
            m_dependency_ptrs.push_back(dependency.c_str());
        for (const PythonUsedConfigKey &key : m_used_config_keys) {
            raw_used_config_key view = {};
            view.key = key.key.c_str();
            view.type = key.type;
            view.container_type = key.container_type;
            view.option_preset_type = key.option_preset_type;
            m_used_config_key_views.push_back(view);
        }
        for (const std::string &key : m_defined_config_keys)
            m_defined_config_key_ptrs.push_back(key.c_str());
    }

    ~PythonPlugin()
    {
        PyGILState_STATE gil_state = PyGILState_Ensure();
        Py_DECREF(m_plugin);
        PyGILState_Release(gil_state);
    }

    plugin_instance c_instance()
    {
        plugin_instance instance = {};
        instance.ctx = this;
        instance.vt = &vtable();
        return instance;
    }

private:
    static const char *get_id_bridge(void *plugin_ctx)
    {
        return static_cast<PythonPlugin *>(plugin_ctx)->m_id.c_str();
    }

    static const char *get_name_bridge(void *plugin_ctx)
    {
        return static_cast<PythonPlugin *>(plugin_ctx)->m_name.c_str();
    }

    static const char *get_description_bridge(void *plugin_ctx)
    {
        return static_cast<PythonPlugin *>(plugin_ctx)->m_description.c_str();
    }

    static const char *get_exclusive_group_bridge(void *plugin_ctx)
    {
        return static_cast<PythonPlugin *>(plugin_ctx)->m_exclusive_group.c_str();
    }

    static const char *get_exclusive_group_label_bridge(void *plugin_ctx)
    {
        return static_cast<PythonPlugin *>(plugin_ctx)->m_exclusive_group_label.c_str();
    }

    static const char *get_exclusive_group_tooltip_bridge(void *plugin_ctx)
    {
        return static_cast<PythonPlugin *>(plugin_ctx)->m_exclusive_group_tooltip.c_str();
    }

    static slicing_step_t get_step_bridge(void *plugin_ctx)
    {
        return static_cast<PythonPlugin *>(plugin_ctx)->m_step;
    }

    static const_strings_t get_dependencies_bridge(void *plugin_ctx)
    {
        PythonPlugin *plugin = static_cast<PythonPlugin *>(plugin_ctx);
        const_strings_t out = {};
        out.items = plugin->m_dependency_ptrs.empty() ? nullptr : plugin->m_dependency_ptrs.data();
        out.size = uint32_t(plugin->m_dependency_ptrs.size());
        return out;
    }

    static int32_t get_priority_bridge(void *plugin_ctx)
    {
        return static_cast<PythonPlugin *>(plugin_ctx)->m_priority;
    }

    static int32_t used_config_keys_bridge(void *plugin_ctx, raw_used_config_key *keys)
    {
        PythonPlugin *plugin = static_cast<PythonPlugin *>(plugin_ctx);
        if (keys != nullptr) {
            for (size_t i = 0; i < plugin->m_used_config_key_views.size(); ++i)
                keys[i] = plugin->m_used_config_key_views[i];
        }
        return int32_t(plugin->m_used_config_key_views.size());
    }

    static int32_t defined_config_keys_bridge(void *plugin_ctx, const char **keys)
    {
        PythonPlugin *plugin = static_cast<PythonPlugin *>(plugin_ctx);
        if (keys != nullptr) {
            for (size_t i = 0; i < plugin->m_defined_config_key_ptrs.size(); ++i)
                keys[i] = plugin->m_defined_config_key_ptrs[i];
        }
        return int32_t(plugin->m_defined_config_key_ptrs.size());
    }

    static void initialize_bridge(void *plugin_ctx, storage_handle *storage)
    {
        PythonPlugin *plugin = static_cast<PythonPlugin *>(plugin_ctx);
        PyGILState_STATE gil_state = PyGILState_Ensure();
        PyObject *arg = pointer_to_python_uint(storage);
        PyObject *args = arg != nullptr ? PyTuple_Pack(1, arg) : nullptr;
        Py_XDECREF(arg);
        if (args != nullptr) {
            call_python_void(plugin->m_plugin, "initialize", args, plugin->m_id);
            Py_DECREF(args);
        } else {
            PyErr_Print();
        }
        PyGILState_Release(gil_state);
    }

    static void setup_bridge(void *plugin_ctx, const plugin_run_context *run_ctx, uint32_t run_count)
    {
        PythonPlugin *plugin = static_cast<PythonPlugin *>(plugin_ctx);
        PyGILState_STATE gil_state = PyGILState_Ensure();
        PyObject *py_run_ctx = pointer_to_python_uint(run_ctx);
        PyObject *py_run_count = PyLong_FromUnsignedLong(run_count);
        PyObject *args = py_run_ctx != nullptr && py_run_count != nullptr ? PyTuple_Pack(2, py_run_ctx, py_run_count) : nullptr;
        Py_XDECREF(py_run_ctx);
        Py_XDECREF(py_run_count);
        if (args != nullptr) {
            call_python_void(plugin->m_plugin, "setup", args, plugin->m_id);
            Py_DECREF(args);
        } else {
            PyErr_Print();
        }
        PyGILState_Release(gil_state);
    }

    static void setup_run_bridge(void *plugin_ctx, const plugin_run_context *run_ctx)
    {
        PythonPlugin *plugin = static_cast<PythonPlugin *>(plugin_ctx);
        PyGILState_STATE gil_state = PyGILState_Ensure();
        PyObject *arg = pointer_to_python_uint(run_ctx);
        PyObject *args = arg != nullptr ? PyTuple_Pack(1, arg) : nullptr;
        Py_XDECREF(arg);
        if (args != nullptr) {
            call_python_void(plugin->m_plugin, "setup_run", args, plugin->m_id);
            Py_DECREF(args);
        } else {
            PyErr_Print();
        }
        PyGILState_Release(gil_state);
    }

    static void run_bridge(void *plugin_ctx, const plugin_run_context *run_ctx)
    {
        PythonPlugin *plugin = static_cast<PythonPlugin *>(plugin_ctx);
        PyGILState_STATE gil_state = PyGILState_Ensure();
        PyObject *arg = pointer_to_python_uint(run_ctx);
        PyObject *args = arg != nullptr ? PyTuple_Pack(1, arg) : nullptr;
        Py_XDECREF(arg);
        if (args != nullptr) {
            call_python_void(plugin->m_plugin, "run", args, plugin->m_id);
            Py_DECREF(args);
        } else {
            PyErr_Print();
        }
        PyGILState_Release(gil_state);
    }

    static const plugin_vtable &vtable()
    {
        static const plugin_vtable vt = {
            SLIC3R_PLUGIN_ABI_VERSION,
            &PythonPlugin::get_id_bridge,
            &PythonPlugin::get_name_bridge,
            &PythonPlugin::get_description_bridge,
            &PythonPlugin::get_exclusive_group_bridge,
            &PythonPlugin::get_exclusive_group_label_bridge,
            &PythonPlugin::get_exclusive_group_tooltip_bridge,
            &PythonPlugin::get_step_bridge,
            &PythonPlugin::get_dependencies_bridge,
            &PythonPlugin::get_priority_bridge,
            &PythonPlugin::used_config_keys_bridge,
            &PythonPlugin::defined_config_keys_bridge,
            &PythonPlugin::initialize_bridge,
            &PythonPlugin::setup_bridge,
            &PythonPlugin::setup_run_bridge,
            &PythonPlugin::run_bridge
        };
        return vt;
    }

    PyObject *m_plugin = nullptr;
    std::string m_id;
    std::string m_name;
    std::string m_description;
    std::string m_exclusive_group;
    std::string m_exclusive_group_label;
    std::string m_exclusive_group_tooltip;
    slicing_step_t m_step;
    int32_t m_priority = 0;
    std::vector<std::string> m_dependencies;
    std::vector<const char *> m_dependency_ptrs;
    std::vector<PythonUsedConfigKey> m_used_config_keys;
    std::vector<raw_used_config_key> m_used_config_key_views;
    std::vector<std::string> m_defined_config_keys;
    std::vector<const char *> m_defined_config_key_ptrs;
};

bool resolve_registration_functions(const boost::filesystem::path &host_library_path,
                                    OrchestratorRegisterPluginFn &register_plugin_fn,
                                    OrchestratorRegisterPluginFromPackageFn &register_package_plugin_fn)
{
    register_plugin_fn = nullptr;
    register_package_plugin_fn = nullptr;
#ifdef _WIN32
    static std::vector<HMODULE> loaded_modules;
    HMODULE module = LoadLibraryW(host_library_path.wstring().c_str());
    if (module == NULL) {
        BOOST_LOG_TRIVIAL(error) << "Cannot open host library '" << host_library_path.string()
                                 << "' for Python plugin registration: error " << GetLastError();
        return false;
    }
    loaded_modules.push_back(module);

    FARPROC farproc = GetProcAddress(module, "orchestrator_register_plugin");
    if (farproc == NULL) {
        BOOST_LOG_TRIVIAL(error) << "Host library '" << host_library_path.string()
                                 << "' does not export orchestrator_register_plugin.";
        return false;
    }
    FARPROC package_farproc = GetProcAddress(module, "orchestrator_register_plugin_from_package");
    if (package_farproc == NULL) {
        BOOST_LOG_TRIVIAL(error) << "Host library '" << host_library_path.string()
                                 << "' does not export orchestrator_register_plugin_from_package.";
        return false;
    }
    register_plugin_fn = reinterpret_cast<OrchestratorRegisterPluginFn>(farproc);
    register_package_plugin_fn = reinterpret_cast<OrchestratorRegisterPluginFromPackageFn>(package_farproc);
#else
    void *symbol = dlsym(RTLD_DEFAULT, "orchestrator_register_plugin");
    void *package_symbol = dlsym(RTLD_DEFAULT, "orchestrator_register_plugin_from_package");
    if (symbol != nullptr && package_symbol != nullptr) {
        register_plugin_fn = reinterpret_cast<OrchestratorRegisterPluginFn>(symbol);
        register_package_plugin_fn = reinterpret_cast<OrchestratorRegisterPluginFromPackageFn>(package_symbol);
        return true;
    }

    void *module = dlopen(host_library_path.string().c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (module == nullptr) {
        BOOST_LOG_TRIVIAL(error) << "Cannot open host library '" << host_library_path.string()
                                 << "' for Python plugin registration: " << dlerror();
        return false;
    }

    symbol = dlsym(module, "orchestrator_register_plugin");
    if (symbol == nullptr) {
        BOOST_LOG_TRIVIAL(error) << "Host library '" << host_library_path.string()
                                 << "' does not export orchestrator_register_plugin: " << dlerror();
        return false;
    }
    package_symbol = dlsym(module, "orchestrator_register_plugin_from_package");
    if (package_symbol == nullptr) {
        BOOST_LOG_TRIVIAL(error) << "Host library '" << host_library_path.string()
                                 << "' does not export orchestrator_register_plugin_from_package: " << dlerror();
        return false;
    }
    register_plugin_fn = reinterpret_cast<OrchestratorRegisterPluginFn>(symbol);
    register_package_plugin_fn = reinterpret_cast<OrchestratorRegisterPluginFromPackageFn>(package_symbol);
#endif
    return true;
}

void register_python_plugin_object(PyObject *plugin_object,
                                   OrchestratorRegisterPluginFn register_plugin_fn,
                                   OrchestratorRegisterPluginFromPackageFn register_package_plugin_fn,
                                   orchestrator_handle *orchestrator,
                                   const boost::filesystem::path &package_root)
{
    if (plugin_object == nullptr || plugin_object == Py_None)
        return;

    std::unique_ptr<PythonPlugin> plugin(new PythonPlugin(plugin_object));
    plugin_instance instance = plugin->c_instance();
    if (package_root.empty())
        register_plugin_fn(orchestrator, instance);
    else
        register_package_plugin_fn(orchestrator, instance, package_root.string().c_str());
    BOOST_LOG_TRIVIAL(info) << "Registered Python plugin '" << plugin->c_instance().vt->get_id(plugin.get()) << "'.";
    s_python_plugins.emplace_back(std::move(plugin));
}

void register_python_plugin_result(PyObject *result,
                                   OrchestratorRegisterPluginFn register_plugin_fn,
                                   OrchestratorRegisterPluginFromPackageFn register_package_plugin_fn,
                                   orchestrator_handle *orchestrator,
                                   const boost::filesystem::path &package_root)
{
    if (result == nullptr || result == Py_None)
        return;

    if (PyList_Check(result) || PyTuple_Check(result)) {
        const Py_ssize_t count = PySequence_Size(result);
        for (Py_ssize_t idx = 0; idx < count; ++idx) {
            PyObject *item = PySequence_GetItem(result, idx);
            register_python_plugin_object(item, register_plugin_fn, register_package_plugin_fn,
                                          orchestrator, package_root);
            Py_XDECREF(item);
        }
        return;
    }

    register_python_plugin_object(result, register_plugin_fn, register_package_plugin_fn,
                                  orchestrator, package_root);
}

PyObject *create_python_api(orchestrator_handle *orchestrator, const boost::filesystem::path &host_library_path)
{
    PyObject *api_module = PyImport_ImportModule("slic3r_api");
    if (api_module == nullptr) {
        PyErr_Print();
        return nullptr;
    }

    PyObject *api_class = PyObject_GetAttrString(api_module, "Slic3rAPI");
    Py_DECREF(api_module);
    if (api_class == nullptr) {
        PyErr_Print();
        return nullptr;
    }

    PyObject *py_orchestrator = pointer_to_python_uint(orchestrator);
    PyObject *py_host_library = python_path_from_boost(host_library_path);
    PyObject *api = py_orchestrator != nullptr && py_host_library != nullptr ?
                        PyObject_CallFunctionObjArgs(api_class, py_orchestrator, py_host_library, nullptr) :
                        nullptr;
    Py_DECREF(api_class);
    Py_XDECREF(py_orchestrator);
    Py_XDECREF(py_host_library);

    if (api == nullptr)
        PyErr_Print();
    return api;
}

void call_python_register(PyObject *module,
                          PyObject *api,
                          OrchestratorRegisterPluginFn register_plugin_fn,
                          OrchestratorRegisterPluginFromPackageFn register_package_plugin_fn,
                          orchestrator_handle *orchestrator,
                          const boost::filesystem::path &plugin_path,
                          const boost::filesystem::path &package_root)
{
    PyObject *register_fn = PyObject_GetAttrString(module, "register_plugin");
    if (register_fn == nullptr) {
        PyErr_Clear();
        BOOST_LOG_TRIVIAL(warning) << "Python plugin '" << plugin_path.string()
                                   << "' does not define register_plugin(api).";
        return;
    }

    if (!PyCallable_Check(register_fn)) {
        BOOST_LOG_TRIVIAL(warning) << "Python plugin '" << plugin_path.string()
                                   << "' has a non-callable register_plugin attribute.";
        Py_DECREF(register_fn);
        return;
    }

    PyObject *result = PyObject_CallFunctionObjArgs(register_fn, api, nullptr);
    Py_DECREF(register_fn);

    if (result == nullptr) {
        PyErr_Print();
        BOOST_LOG_TRIVIAL(error) << "Python plugin '" << plugin_path.string() << "' failed during registration.";
        return;
    }

    register_python_plugin_result(result, register_plugin_fn, register_package_plugin_fn,
                                  orchestrator, package_root);
    Py_DECREF(result);
    BOOST_LOG_TRIVIAL(info) << "Loaded Python plugin module '" << plugin_path.string() << "'.";
}

void load_python_plugin(const boost::filesystem::path &plugin_path,
                        PyObject *api,
                        OrchestratorRegisterPluginFn register_plugin_fn,
                        OrchestratorRegisterPluginFromPackageFn register_package_plugin_fn,
                        orchestrator_handle *orchestrator,
                        const boost::filesystem::path &package_root)
{
    const std::string module_name = plugin_path.stem().string();
    PyObject *module = PyImport_ImportModule(module_name.c_str());
    if (module == nullptr) {
        PyErr_Print();
        BOOST_LOG_TRIVIAL(error) << "Cannot import Python plugin '" << plugin_path.string() << "'.";
        return;
    }

    call_python_register(module, api, register_plugin_fn, register_package_plugin_fn,
                         orchestrator, plugin_path, package_root);
    Py_DECREF(module);
}

// External packages are loaded by absolute path under a unique module name.
// Two packages may both use plugin.py, so importing by stem would otherwise
// return the first package from Python's module cache.
void load_python_plugin_from_path(const boost::filesystem::path &plugin_path,
                                  PyObject *api,
                                  OrchestratorRegisterPluginFn register_plugin_fn,
                                  OrchestratorRegisterPluginFromPackageFn register_package_plugin_fn,
                                  orchestrator_handle *orchestrator,
                                  const boost::filesystem::path &package_root)
{
    boost::nowide::ifstream stream(plugin_path.string(), std::ios::in | std::ios::binary);
    if (!stream) {
        BOOST_LOG_TRIVIAL(error) << "Cannot read Python plugin '" << plugin_path.string() << "'.";
        return;
    }
    const std::string source((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    static uint64_t next_module_id = 0;
    const std::string module_name = "_slic3r_package_" + std::to_string(++next_module_id);
    PyObject *code = Py_CompileString(source.c_str(), plugin_path.string().c_str(), Py_file_input);
    if (code == nullptr) {
        PyErr_Print();
        BOOST_LOG_TRIVIAL(error) << "Cannot compile Python plugin '" << plugin_path.string() << "'.";
        return;
    }
    PyObject *module = PyImport_ExecCodeModuleEx(
        module_name.c_str(), code, plugin_path.string().c_str());
    Py_DECREF(code);
    if (module == nullptr) {
        PyErr_Print();
        BOOST_LOG_TRIVIAL(error) << "Cannot load Python plugin '" << plugin_path.string() << "'.";
        return;
    }

    call_python_register(module, api, register_plugin_fn, register_package_plugin_fn,
                         orchestrator, plugin_path, package_root);
    Py_DECREF(module);
}

std::string native_plugin_filename()
{
#ifdef _WIN32
    return "plugin.dll";
#elif defined(__APPLE__)
    return "plugin.dylib";
#else
    return "plugin.so";
#endif
}

boost::filesystem::path external_python_entry(const boost::filesystem::path &package_root)
{
    const std::string package_id = package_root.filename().string();
    const boost::filesystem::path named_entry = package_root / (package_id + ".py");
    if (boost::filesystem::is_regular_file(named_entry))
        return named_entry;
    const boost::filesystem::path conventional_entry = package_root / "plugin.py";
    if (boost::filesystem::is_regular_file(conventional_entry))
        return conventional_entry;

    boost::filesystem::path unique_entry;
    for (boost::filesystem::directory_iterator it(package_root), end; it != end; ++it) {
        if (!boost::filesystem::is_regular_file(it->path()) || it->path().extension() != ".py")
            continue;
        if (!unique_entry.empty())
            return {};
        unique_entry = it->path();
    }
    return unique_entry;
}

void load_python_plugins(orchestrator_handle *orchestrator)
{
    const boost::filesystem::path loader_path = current_module_path();
    if (loader_path.empty()) {
        BOOST_LOG_TRIVIAL(warning) << "Cannot locate python_plugin_loader module.";
        return;
    }

    const boost::filesystem::path plugin_repository = loader_path.parent_path();
    const boost::filesystem::path python_root = plugin_repository / "python";
    const boost::filesystem::path python_plugins = python_root / "plugins";
#ifdef _WIN32
    boost::filesystem::path host_library_path = plugin_repository.parent_path() / "Slic3r.dll";
    if (!boost::filesystem::exists(host_library_path))
        host_library_path = current_executable_path();
#else
    boost::filesystem::path host_library_path = plugin_repository.parent_path() / "Slic3r";
    if (!boost::filesystem::exists(host_library_path))
        host_library_path = current_executable_path();
#endif

    OrchestratorRegisterPluginFn register_plugin_fn = nullptr;
    OrchestratorRegisterPluginFromPackageFn register_package_plugin_fn = nullptr;
    if (!resolve_registration_functions(host_library_path, register_plugin_fn, register_package_plugin_fn))
        return;

    if (!ensure_python_initialized_for_plugins()) {
        BOOST_LOG_TRIVIAL(error) << "Cannot initialize Python runtime for plugins.";
        return;
    }

    PyGILState_STATE gil_state = PyGILState_Ensure();
    append_python_path(python_root);
    append_python_path(python_plugins);

    PyObject *api = create_python_api(orchestrator, host_library_path);
    if (api != nullptr) {
        // Scripts bundled with the Python loader keep the loader package scope
        // supplied by the native PluginLoader.
        if (boost::filesystem::is_directory(python_plugins)) {
            for (boost::filesystem::directory_iterator it(python_plugins), end; it != end; ++it) {
                const boost::filesystem::path plugin_path = it->path();
                if (boost::filesystem::is_regular_file(plugin_path) && plugin_path.extension() == ".py")
                    load_python_plugin(plugin_path, api, register_plugin_fn, register_package_plugin_fn,
                                       orchestrator, {});
            }
        }

        // Pure Python packages are siblings of native packages. Requiring both
        // normalized metadata files keeps runtime discovery aligned with cache
        // validation, while the absence of a library avoids double loading a
        // native package that also ships helper scripts.
        const boost::filesystem::path installed_packages = plugin_repository.parent_path();
        for (boost::filesystem::directory_iterator it(installed_packages), end; it != end; ++it) {
            const boost::filesystem::path package_root = it->path();
            if (!boost::filesystem::is_directory(package_root) || package_root == plugin_repository ||
                boost::filesystem::is_regular_file(package_root / native_plugin_filename()) ||
                !boost::filesystem::is_regular_file(package_root / "description.ini") ||
                !boost::filesystem::is_regular_file(package_root / "version.ini"))
                continue;
            const boost::filesystem::path plugin_path = external_python_entry(package_root);
            if (plugin_path.empty())
                continue;
            append_python_path(package_root);
            load_python_plugin_from_path(plugin_path, api, register_plugin_fn, register_package_plugin_fn,
                                         orchestrator, package_root);
        }
        Py_DECREF(api);
    }

    PyGILState_Release(gil_state);
}

} // namespace

SLIC3R_PLUGIN_DECLARE_ABI_VERSION()

extern "C" SLIC3R_PLUGIN_API void register_plugin(orchestrator_handle *orch)
{
    load_python_plugins(orch);
}
