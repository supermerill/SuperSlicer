//
// CSerializer
//
// This code was based on the CScriptReloader written by FDsagizi
// http://www.gamedev.net/topic/604890-dynamic-reloading-script/
//

#ifndef SERIALIZER_H
#define SERIALIZER_H

#ifndef ANGELSCRIPT_H
// Avoid having to inform include path if header is already include before
#include <angelscript.h>
#endif

#include <vector>
#include <string>
#include <map>

BEGIN_AS_NAMESPACE

class CSerializer;
class CSerializedValue;

// Need for register user types objects
// string, any, array... for all object
// user ref type.
struct CUserType
{
    virtual ~CUserType() {};
    virtual void Store(CSerializedValue *val, void *ptr) = 0;
    virtual void Restore(CSerializedValue *val, void *ptr) = 0;
    virtual void CleanupUserData(CSerializedValue * /*val*/) {}
};


class CSerializedValue
{
public:
    CSerializedValue();
    CSerializedValue(CSerializedValue *parent, const std::string &name, const std::string &nameSpace, void *ref, int typeId);
    ~CSerializedValue();

    // Save the object and its children
    void Store(void *ref, int refTypeId);

    // Restore the object and its children
    void Restore(void *ref, int refTypeId);

    // Set type of this var
    void SetType(int typeId);

    // Returns the object type for non-primitives
    asITypeInfo *GetType();

    // Get child by name variable
    CSerializedValue *FindByName(const std::string &name, const std::string &nameSpace);

    // Find variable by ptr
    CSerializedValue *FindByPtr(void *ptr);

    // User data
    void *GetUserData();
    void  SetUserData(void *data);

    // Children, e.g. properties of a script class, or elements
    // of an array, or object pointed to by a handle unless it
    // is already a variable)
    std::vector<CSerializedValue*> m_children;

protected:
    friend class CSerializer;

    void Init();
    void Uninit();

    // you first need to save all the objects before you can save references to objects
    void ReplaceHandles();

    // After the objects has been restored, the handles needs to
    // be updated to point to the right objects
    void RestoreHandles();

    // Recursively get all ptrs of the children
    void  GetAllPointersOfChildren(std::vector<void*> *ptrs);

    // may be that the two references refer to the same variable.
    // But this variable is not available in the global list.
    // According to this reference will be restores it.
    // And so two links are not created 2 variables,
    // it is necessary to cancel the creation of one of them.
    void CancelDuplicates(CSerializedValue *from);

    // Find variable by ptr but looking only at those in the references, which will create a new object
    CSerializedValue *FindByPtrInHandles(void *ptr);

    // ptr  - is a handle to class
    void *GetPointerToRestoredObject(void *ptr);

    // Cleanup children
    void ClearChildren();

    // The serializer object
    CSerializer *m_serializer;

    // The user data can be used by CUserType to store extra information
    void *m_userData;

    // The type id of the stored value
    int m_typeId;

    // For non-primitives the typeId may change if the module is reloaded so
    // it is necessary to store the type name to determine the new type id
    std::string m_typeName;

    // Name of variable or property
    std::string m_name;
    std::string m_nameSpace;

    // Is initialized
    bool m_isInit;

    // 'this' pointer to variable.
    // While storing, this points to the actual variable that was stored.
    // While restoring, it is just a unique identifier.
    void *m_originalPtr;

    // where handle references
    // While storing, this points to the actual object.
    // While restoring, it is just a unique identifier.
    void *m_handlePtr;

    // new address object, ie address the restoration
    // While storing this isn't used.
    // While restoring it will point to the actual variable/object that is restored.
    void *m_restorePtr;

    // Serialized data for primitives
    std::vector<char> m_mem;
};


// This class keeps a list of variables, then restores them after the script is rebuilt.
// But you have to be careful with the change of signature in classes, or
// changing the types of objects. You can remove or add variables, functions,
// methods, but you can not (yet) change the type of variables.
//
// You also need to understand that after a rebuild you should get
// new functions and typeids from the module.
class CSerializer
{
public:
    CSerializer();
    ~CSerializer();

    // Add implementation for serializing user types
    void AddUserType(CUserType *ref, const std::string &name);

    // Store all global variables in the module
    int Store(asIScriptModule *mod);

    // Restore all global variables after reloading script
    int Restore(asIScriptModule *mod);

    // Store extra objects that are not seen from the module's global variables
    void AddExtraObjectToStore(asIScriptObject *object);

    // Return new pointer to restored object
    void *GetPointerToRestoredObject(void *originalObject);

protected:
    friend class CSerializedValue;

    CSerializedValue  m_root;
    asIScriptEngine  *m_engine;
    asIScriptModule  *m_mod;

    std::map<std::string, CUserType*> m_userTypes;

    struct SExtraObject
    {
        asIScriptObject *originalObject;
        std::string      originalClassName;
        int              originalTypeId;
    };

    std::vector<SExtraObject> m_extraObjects;
};


END_AS_NAMESPACE

#endif
