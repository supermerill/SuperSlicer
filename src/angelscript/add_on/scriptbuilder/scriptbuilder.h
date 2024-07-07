#ifndef SCRIPTBUILDER_H
#define SCRIPTBUILDER_H

//---------------------------
// Compilation settings
//

// Set this flag to turn on/off metadata processing
//  0 = off
//  1 = on
#ifndef AS_PROCESS_METADATA
#define AS_PROCESS_METADATA 1
#endif

// TODO: Implement flags for turning on/off include directives and conditional programming



//---------------------------
// Declaration
//

#ifndef ANGELSCRIPT_H
// Avoid having to inform include path if header is already include before
#include <angelscript.h>
#endif


#if defined(_MSC_VER) && _MSC_VER <= 1200
// disable the annoying warnings on MSVC 6
#pragma warning (disable:4786)
#endif

#include <string>
#include <map>
#include <set>
#include <vector>
#include <string.h> // _strcmpi

BEGIN_AS_NAMESPACE

template<class T>
class PtrRelease
{
private:
    T* ptr = nullptr;
public:
    PtrRelease() {}
    PtrRelease(const PtrRelease&) = delete;
    PtrRelease& operator=(PtrRelease const&) = delete;
    ~PtrRelease() { if (nullptr != ptr) ptr->Release(); }

    /// @brief this reset release its ownership and re-acquire another one
    void reset(T* p) noexcept
    {
        assert((nullptr == p) || (ptr != p)); // auto-reset not allowed
        if (nullptr != ptr) ptr->Release();
        ptr = p;
    }
    // underlying pointer operations :
    inline T& operator*()  const noexcept
    {
        assert(nullptr != ptr);
        return *ptr;
    }
    inline T* operator->() const noexcept
    {
        assert(nullptr != ptr);
        return ptr;
    }
    inline T* get()  const noexcept
    {
        // no assert, can return nullptr
        return ptr;
    }
};

class CScriptBuilder;

// This callback will be called for each #include directive encountered by the
// builder. The callback should call the AddSectionFromFile or AddSectionFromMemory
// to add the included section to the script. If the include cannot be resolved
// then the function should return a negative value to abort the compilation.
typedef int (*INCLUDECALLBACK_t)(const char *include, const char *from, CScriptBuilder *builder, void *userParam);

// This callback will be called for each #pragma directive encountered by the builder.
// The application can interpret the pragmaText and decide what do to based on that.
// If the callback returns a negative value the builder will report an error and abort the compilation.
typedef int(*PRAGMACALLBACK_t)(const std::string &pragmaText, CScriptBuilder &builder, void *userParam);

// Helper class for loading and pre-processing script files to
// support include directives and metadata declarations
class CScriptBuilder
{
public:
    CScriptBuilder();

    // Start a new module
    int StartNewModule(asIScriptEngine *engine, const char *moduleName);

    // Load a script section from a file on disk
    // Returns  1 if the file was included
    //          0 if the file had already been included before
    //         <0 on error
    int AddSectionFromFile(const char *filename);

    // Load a script section from memory
    // Returns  1 if the section was included
    //          0 if a section with the same name had already been included before
    //         <0 on error
    int AddSectionFromMemory(const char *sectionName,
                             const char *scriptCode,
                             unsigned int scriptLength = 0,
                             int lineOffset = 0);

    // Build the added script sections
    int BuildModule();

    // Returns the engine
    asIScriptEngine *GetEngine();

    // Returns the current module
    asIScriptModule *GetModule();

    // Register the callback for resolving include directive
    void SetIncludeCallback(INCLUDECALLBACK_t callback, void *userParam);

    // Register the callback for resolving pragma directive
    void SetPragmaCallback(PRAGMACALLBACK_t callback, void *userParam);

    // Add a pre-processor define for conditional compilation
    void DefineWord(const char *word);

    // Enumerate included script sections
    unsigned int GetSectionCount() const;
    std::string  GetSectionName(unsigned int idx) const;

#if AS_PROCESS_METADATA == 1
    // Get metadata declared for classes, interfaces, and enums
    std::vector<std::string> GetMetadataForType(int typeId);

    // Get metadata declared for functions
    std::vector<std::string> GetMetadataForFunc(asIScriptFunction *func);

    // Get metadata declared for global variables
    std::vector<std::string> GetMetadataForVar(int varIdx);

    // Get metadata declared for class variables
    std::vector<std::string> GetMetadataForTypeProperty(int typeId, int varIdx);

    // Get metadata declared for class methods
    std::vector<std::string> GetMetadataForTypeMethod(int typeId, asIScriptFunction *method);
#endif

protected:
    void ClearAll();
    int  Build();
    int  ProcessScriptSection(const char *script, unsigned int length, const char *sectionname, int lineOffset);
    int  LoadScriptSection(const char *filename);
    bool IncludeIfNotAlreadyIncluded(const char *filename);

    int  SkipStatement(int pos);

    int  ExcludeCode(int start);
    void OverwriteCode(int start, int len);

    asIScriptEngine           *engine;
    asIScriptModule           *module;
    std::string                modifiedScript;

    INCLUDECALLBACK_t  includeCallback;
    void              *includeParam;

    PRAGMACALLBACK_t  pragmaCallback;
    void             *pragmaParam;

#if AS_PROCESS_METADATA == 1
    int  ExtractMetadata(int pos, std::vector<std::string> &outMetadata);
    int  ExtractDeclaration(int pos, std::string &outName, std::string &outDeclaration, int &outType);

    enum METADATATYPE
    {
        MDT_TYPE = 1,
        MDT_FUNC = 2,
        MDT_VAR = 3,
        MDT_VIRTPROP = 4,
        MDT_FUNC_OR_VAR = 5
    };

    // Temporary structure for storing metadata and declaration
    struct SMetadataDecl
    {
        SMetadataDecl(std::vector<std::string> m, std::string n, std::string d, int t, std::string c, std::string ns) : metadata(m), name(n), declaration(d), type(t), parentClass(c), nameSpace(ns) {}
        std::vector<std::string> metadata;
        std::string              name;
        std::string              declaration;
        int                      type;
        std::string              parentClass;
        std::string              nameSpace;
    };
    std::vector<SMetadataDecl> foundDeclarations;
    std::string currentClass;
    std::string currentNamespace;

    // Storage of metadata for global declarations
    std::map<int, std::vector<std::string> > typeMetadataMap;
    std::map<int, std::vector<std::string> > funcMetadataMap;
    std::map<int, std::vector<std::string> > varMetadataMap;

    // Storage of metadata for class member declarations
    struct SClassMetadata
    {
        SClassMetadata(const std::string& aName) : className(aName) {}
        std::string className;
        std::map<int, std::vector<std::string> > funcMetadataMap;
        std::map<int, std::vector<std::string> > varMetadataMap;
    };
    std::map<int, SClassMetadata> classMetadataMap;

#endif

#ifdef _WIN32
    // On Windows the filenames are case insensitive so the comparisons to
    // avoid duplicate includes must also be case insensitive. True case insensitive
    // is not easy as it must be language aware, but a simple implementation such
    // as strcmpi should suffice in almost all cases.
    //
    // ref: http://www.gotw.ca/gotw/029.htm
    // ref: https://msdn.microsoft.com/en-us/library/windows/desktop/dd317761(v=vs.85).aspx
    // ref: http://site.icu-project.org/

    // TODO: Strings by default are treated as UTF8 encoded. If the application choses to
    //       use a different encoding, the comparison algorithm should be adjusted as well

    struct ci_less
    {
        bool operator()(const std::string &a, const std::string &b) const
        {
            return _stricmp(a.c_str(), b.c_str()) < 0;
        }
    };
    std::set<std::string, ci_less> includedScripts;
#else
    std::set<std::string>      includedScripts;
#endif

    std::set<std::string>      definedWords;
};

END_AS_NAMESPACE

#endif
