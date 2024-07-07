#ifndef SCRIPTHELPER_H
#define SCRIPTHELPER_H

#include <sstream>
#include <string>

#ifndef ANGELSCRIPT_H
// Avoid having to inform include path if header is already include before
#include <angelscript.h>
#endif


BEGIN_AS_NAMESPACE

// Compare relation between two objects of the same type
int CompareRelation(asIScriptEngine *engine, void *lobj, void *robj, int typeId, int &result);

// Compare equality between two objects of the same type
int CompareEquality(asIScriptEngine *engine, void *lobj, void *robj, int typeId, bool &result);

// Compile and execute simple statements
// The module is optional. If given the statements can access the entities compiled in the module.
// The caller can optionally provide its own context, for example if a context should be reused.
int ExecuteString(asIScriptEngine *engine, const char *code, asIScriptModule *mod = 0, asIScriptContext *ctx = 0);

// Compile and execute simple statements with option of return value
// The module is optional. If given the statements can access the entitites compiled in the module.
// The caller can optionally provide its own context, for example if a context should be reused.
int ExecuteString(asIScriptEngine *engine, const char *code, void *ret, int retTypeId, asIScriptModule *mod = 0, asIScriptContext *ctx = 0);

// Write the registered application interface to a file for an offline compiler.
// The format is compatible with the offline compiler in /sdk/samples/asbuild/.
int WriteConfigToFile(asIScriptEngine *engine, const char *filename);

// Write the registered application interface to a text stream.
int WriteConfigToStream(asIScriptEngine *engine, std::ostream &strm);

// Loads an interface from a text stream and configures the engine with it. This will not
// set the correct function pointers, so it is not possible to use this engine to execute
// scripts, but it can be used to compile scripts and save the byte code.
int ConfigEngineFromStream(asIScriptEngine *engine, std::istream &strm, const char *nameOfStream = "config", asIStringFactory *stringFactory = 0);

// Format the details of the script exception into a human readable text
std::string GetExceptionInfo(asIScriptContext *ctx, bool showStack = false);

// Register the exception routines
//  'void throw(const string &msg)'
//  'string getExceptionInfo()'
void RegisterExceptionRoutines(asIScriptEngine *engine);

END_AS_NAMESPACE

#endif
