#ifndef SCRIPTDICTIONARY_H
#define SCRIPTDICTIONARY_H

// The dictionary class relies on the script string object, thus the script
// string type must be registered with the engine before registering the
// dictionary type

#ifndef ANGELSCRIPT_H
// Avoid having to inform include path if header is already include before
#include <angelscript.h>
#endif

// By default the CScriptDictionary use the std::string for the keys.
// If the application uses a custom string type, then this typedef
// can be changed accordingly.
#include <string>
typedef std::string dictKey_t;

// Forward declare CScriptDictValue so we can typedef the internal map type
BEGIN_AS_NAMESPACE
class CScriptDictValue;
END_AS_NAMESPACE

// C++11 introduced the std::unordered_map which is a hash map which is
// is generally more performatic for lookups than the std::map which is a
// binary tree.
// TODO: memory: The map allocator should use the asAllocMem and asFreeMem
#if AS_CAN_USE_CPP11
#include <unordered_map>
typedef std::unordered_map<dictKey_t, AS_NAMESPACE_QUALIFIER CScriptDictValue> dictMap_t;
#else
#include <map>
typedef std::map<dictKey_t, AS_NAMESPACE_QUALIFIER CScriptDictValue> dictMap_t;
#endif


#ifdef _MSC_VER
// Turn off annoying warnings about truncated symbol names
#pragma warning (disable:4786)
#endif




// Sometimes it may be desired to use the same method names as used by C++ STL.
// This may for example reduce time when converting code from script to C++ or
// back.
//
//  0 = off
//  1 = on

#ifndef AS_USE_STLNAMES
#define AS_USE_STLNAMES 0
#endif


BEGIN_AS_NAMESPACE

class CScriptArray;
class CScriptDictionary;

class CScriptDictValue
{
public:
    // This class must not be declared as local variable in C++, because it needs
    // to receive the script engine pointer in all operations. The engine pointer
    // is not kept as member in order to keep the size down
    CScriptDictValue();
    CScriptDictValue(asIScriptEngine *engine, void *value, int typeId);

    // Destructor must not be called without first calling FreeValue, otherwise a memory leak will occur
    ~CScriptDictValue();

    // Replace the stored value
    void Set(asIScriptEngine *engine, void *value, int typeId);
    void Set(asIScriptEngine *engine, const asINT64 &value);
    void Set(asIScriptEngine *engine, const double &value);
    void Set(asIScriptEngine *engine, CScriptDictValue &value);

    // Gets the stored value. Returns false if the value isn't compatible with the informed typeId
    bool Get(asIScriptEngine *engine, void *value, int typeId) const;
    bool Get(asIScriptEngine *engine, asINT64 &value) const;
    bool Get(asIScriptEngine *engine, double &value) const;

    // Returns the address of the stored value for inspection
    const void *GetAddressOfValue() const;

    // Returns the type id of the stored value
    int  GetTypeId() const;

    // Free the stored value
    void FreeValue(asIScriptEngine *engine);

    // GC callback
    void EnumReferences(asIScriptEngine *engine);

protected:
    friend class CScriptDictionary;

    union
    {
        asINT64 m_valueInt;
        double  m_valueFlt;
        void   *m_valueObj;
    };
    int m_typeId;
};

class CScriptDictionary
{
public:
    // Factory functions
    static CScriptDictionary *Create(asIScriptEngine *engine);

    // Called from the script to instantiate a dictionary from an initialization list
    static CScriptDictionary *Create(asBYTE *buffer);

    // Reference counting
    void AddRef() const;
    void Release() const;

    // Reassign the dictionary
    CScriptDictionary &operator =(const CScriptDictionary &other);

    // Sets a key/value pair
    void Set(const dictKey_t &key, void *value, int typeId);
    void Set(const dictKey_t &key, const asINT64 &value);
    void Set(const dictKey_t &key, const double &value);

    // Gets the stored value. Returns false if the value isn't compatible with the informed typeId
    bool Get(const dictKey_t &key, void *value, int typeId) const;
    bool Get(const dictKey_t &key, asINT64 &value) const;
    bool Get(const dictKey_t &key, double &value) const;

    // Index accessors. If the dictionary is not const it inserts the value if it doesn't already exist
    // If the dictionary is const then a script exception is set if it doesn't exist and a null pointer is returned
    CScriptDictValue *operator[](const dictKey_t &key);
    const CScriptDictValue *operator[](const dictKey_t &key) const;

    // Returns the type id of the stored value, or negative if it doesn't exist
    int GetTypeId(const dictKey_t &key) const;

    // Returns true if the key is set
    bool Exists(const dictKey_t &key) const;

    // Returns true if there are no key/value pairs in the dictionary
    bool IsEmpty() const;

    // Returns the number of key/value pairs in the dictionary
    asUINT GetSize() const;

    // Deletes the key
    bool Delete(const dictKey_t &key);

    // Deletes all keys
    void DeleteAll();

    // Get an array of all keys
    CScriptArray *GetKeys() const;

    // STL style iterator
    class CIterator
    {
    public:
        void operator++();    // Pre-increment
        void operator++(int); // Post-increment

        // This is needed to support C++11 range-for
        CIterator &operator*();

        bool operator==(const CIterator &other) const;
        bool operator!=(const CIterator &other) const;

        // Accessors
        const dictKey_t &GetKey() const;
        int              GetTypeId() const;
        bool             GetValue(asINT64 &value) const;
        bool             GetValue(double &value) const;
        bool             GetValue(void *value, int typeId) const;
        const void *     GetAddressOfValue() const;

    protected:
        friend class CScriptDictionary;

        CIterator();
        CIterator(const CScriptDictionary &dict,
                  dictMap_t::const_iterator it);

        CIterator &operator=(const CIterator &) {return *this;} // Not used

        dictMap_t::const_iterator m_it;
        const CScriptDictionary &m_dict;
    };

    CIterator begin() const;
    CIterator end() const;
    CIterator find(const dictKey_t &key) const;

    // Garbage collections behaviours
    int GetRefCount();
    void SetGCFlag();
    bool GetGCFlag();
    void EnumReferences(asIScriptEngine *engine);
    void ReleaseAllReferences(asIScriptEngine *engine);

protected:
    // Since the dictionary uses the asAllocMem and asFreeMem functions to allocate memory
    // the constructors are made protected so that the application cannot allocate it
    // manually in a different way
    CScriptDictionary(asIScriptEngine *engine);
    CScriptDictionary(asBYTE *buffer);

    // We don't want anyone to call the destructor directly, it should be called through the Release method
    virtual ~CScriptDictionary();

    // Cache the object types needed
    void Init(asIScriptEngine *engine);

    // Our properties
    asIScriptEngine *engine;
    mutable int      refCount;
    mutable bool     gcFlag;
    dictMap_t        dict;
};

// This function will determine the configuration of the engine
// and use one of the two functions below to register the dictionary object
void RegisterScriptDictionary(asIScriptEngine *engine);

// Call this function to register the math functions
// using native calling conventions
void RegisterScriptDictionary_Native(asIScriptEngine *engine);

// Use this one instead if native calling conventions
// are not supported on the target platform
void RegisterScriptDictionary_Generic(asIScriptEngine *engine);

END_AS_NAMESPACE

#endif
