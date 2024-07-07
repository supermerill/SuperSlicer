//
// CSerializer
//
// This code was based on the CScriptReloader written by FDsagizi
// http://www.gamedev.net/topic/604890-dynamic-reloading-script/
//

#include <assert.h>
#include <string.h> // strstr
#include <stdio.h>  // sprintf
#include "serializer.h"

using namespace std;

BEGIN_AS_NAMESPACE

///////////////////////////////////////////////////////////////////////////////////

CSerializer::CSerializer()
{
    m_engine = 0;
}

CSerializer::~CSerializer()
{
    // Extra objects need to be released, since they are not stored in
    // the module and we cannot rely on the application releasing them
    for( size_t i = 0; i < m_extraObjects.size(); i++ )
    {
        SExtraObject &o = m_extraObjects[i];
        for( size_t i2 = 0; i2 < m_root.m_children.size(); i2++ )
        {
            if( m_root.m_children[i2]->m_originalPtr == o.originalObject && m_root.m_children[i2]->m_restorePtr )
                reinterpret_cast<asIScriptObject*>(m_root.m_children[i2]->m_restorePtr)->Release();
        }
    }

    // Clean the serialized values before we remove the user types
    m_root.Uninit();

    // Delete the user types
    std::map<std::string, CUserType*>::iterator it;
    for( it = m_userTypes.begin(); it != m_userTypes.end(); it++  )
        delete it->second;

    if( m_engine )
        m_engine->Release();
}

void CSerializer::AddUserType(CUserType *ref, const std::string &name)
{
    m_userTypes[name] = ref;
}

int CSerializer::Store(asIScriptModule *mod)
{
    m_mod = mod;

    // The engine must not be destroyed before we're completed, so we'll hold on to a reference
    mod->GetEngine()->AddRef();
    if( m_engine ) m_engine->Release();
    m_engine = mod->GetEngine();

    m_root.m_serializer = this;

    // First store global variables
    asUINT i;
    for( i = 0; i < mod->GetGlobalVarCount(); i++ )
    {
        const char *name, *nameSpace;
        int typeId;
        mod->GetGlobalVar(i, &name, &nameSpace, &typeId);
        m_root.m_children.push_back(new CSerializedValue(&m_root, name, nameSpace, mod->GetAddressOfGlobalVar(i), typeId));
    }

    // Second store extra objects
    for( i = 0; i < m_extraObjects.size(); i++ )
        m_root.m_children.push_back(new CSerializedValue(&m_root, "", "", m_extraObjects[i].originalObject, m_extraObjects[i].originalTypeId));

    // For the handles that were stored, we need to substitute the stored pointer
    // that is still pointing to the original object to an internal reference so
    // it can be restored later on.
    m_root.ReplaceHandles();

    return 0;
}

// Retrieve all global variables after reload script.
int CSerializer::Restore(asIScriptModule *mod)
{
    m_mod = mod;

    // The engine must not be destroyed before we're completed, so we'll hold on to a reference
    mod->GetEngine()->AddRef();
    if( m_engine ) m_engine->Release();
    m_engine = mod->GetEngine();

    // First restore extra objects, i.e. the ones that are not directly seen from the module's global variables
    asUINT i;
    for( i = 0; i < m_extraObjects.size(); i++ )
    {
        SExtraObject &o = m_extraObjects[i];
        asITypeInfo *type = m_mod->GetTypeInfoByName( o.originalClassName.c_str() );
        if( type )
        {
            for( size_t i2 = 0; i2 < m_root.m_children.size(); i2++ )
            {
                if( m_root.m_children[i2]->m_originalPtr == o.originalObject )
                {
                    // Create a new script object, but don't call its constructor as we will initialize the members.
                    // Calling the constructor may have unwanted side effects if for example the constructor changes
                    // any outside entities, such as setting global variables to point to new objects, etc.
                    void *newPtr = m_engine->CreateUninitializedScriptObject( type );
                    m_root.m_children[i2]->Restore( newPtr, type->GetTypeId() );
                }
            }
        }
    }

    // Second restore the global variables
    asUINT varCount = mod->GetGlobalVarCount();
    for( i = 0; i < varCount; i++ )
    {
        const char *name, *nameSpace;
        int typeId;
        mod->GetGlobalVar(i, &name, &nameSpace, &typeId);

        CSerializedValue *v = m_root.FindByName(name, nameSpace);
        if( v )
            v->Restore(mod->GetAddressOfGlobalVar(i), typeId);
    }

    // The handles that were restored needs to be
    // updated to point to their final objects.
    m_root.RestoreHandles();

    return 0;
}

void *CSerializer::GetPointerToRestoredObject(void *ptr)
{
    return m_root.GetPointerToRestoredObject( ptr );
}

void CSerializer::AddExtraObjectToStore( asIScriptObject *object )
{
    if( !object )
        return;

    // Check if the object hasn't been included already
    for( size_t i=0; i < m_extraObjects.size(); i++ )
        if( m_extraObjects[i].originalObject == object )
            return;

    SExtraObject o;
    o.originalObject    = object;
    o.originalClassName = object->GetObjectType()->GetName();
    o.originalTypeId    = object->GetTypeId();

    m_extraObjects.push_back( o );
}


///////////////////////////////////////////////////////////////////////////////////

CSerializedValue::CSerializedValue()
{
    Init();
}

CSerializedValue::CSerializedValue(CSerializedValue *parent, const std::string &name, const std::string &nameSpace, void *ref, int typeId)
{
    Init();

    m_name       = name;
    m_nameSpace  = nameSpace;
    m_serializer = parent->m_serializer;
    Store(ref, typeId);
}

void CSerializedValue::Init()
{
    m_handlePtr   = 0;
    m_restorePtr  = 0;
    m_typeId      = 0;
    m_isInit      = false;
    m_serializer  = 0;
    m_userData    = 0;
    m_originalPtr = 0;
}

void CSerializedValue::Uninit()
{
    m_isInit = false;

    ClearChildren();

    if( m_userData )
    {
        CUserType *type = m_serializer->m_userTypes[m_typeName];
        if( type )
            type->CleanupUserData(this);
        m_userData = 0;
    }
}

void CSerializedValue::ClearChildren()
{
    // If this value is for an object handle that created an object during the restore
    // then it is necessary to release the handle here, so we won't get a memory leak
    if( (m_typeId & asTYPEID_OBJHANDLE) && m_children.size() == 1 && m_children[0]->m_restorePtr )
    {
        m_serializer->m_engine->ReleaseScriptObject(m_children[0]->m_restorePtr, m_serializer->m_engine->GetTypeInfoById(m_children[0]->m_typeId));
    }

    for( size_t n = 0; n < m_children.size(); n++ )
        delete m_children[n];
    m_children.clear();
}

CSerializedValue::~CSerializedValue()
{
    Uninit();
}

CSerializedValue *CSerializedValue::FindByName(const std::string &name, const std::string &nameSpace)
{
    for( size_t i = 0; i < m_children.size(); i++ )
        if( m_children[i]->m_name      == name &&
            m_children[i]->m_nameSpace == nameSpace )
            return m_children[i];

    return 0;
}

void  CSerializedValue::GetAllPointersOfChildren(std::vector<void*> *ptrs)
{
    ptrs->push_back(m_originalPtr);

    for( size_t i = 0; i < m_children.size(); ++i )
        m_children[i]->GetAllPointersOfChildren(ptrs);
}

CSerializedValue *CSerializedValue::FindByPtr(void *ptr)
{
    if( m_originalPtr == ptr )
        return this;

    for( size_t i = 0; i < m_children.size(); i++ )
    {
        CSerializedValue *find = m_children[i]->FindByPtr(ptr);
        if( find )
            return find;
    }

    return 0;
}

void *CSerializedValue::GetPointerToRestoredObject(void *ptr)
{
    if( m_originalPtr == ptr )
        return m_restorePtr;

    for( size_t i = 0; i < m_children.size(); ++i )
    {
        void *ret = m_children[i]->GetPointerToRestoredObject(ptr);
        if( ret )
            return ret;
    }

    return 0;
}

// find variable by ptr but looking only at those in the references, which will create a new object
CSerializedValue *CSerializedValue::FindByPtrInHandles(void *ptr)
{
    // if this handle created object
    if( (m_typeId & asTYPEID_OBJHANDLE) && m_children.size() == 1 )
    {
        if( m_children[0]->m_originalPtr == ptr )
            return this;
    }

    if( !(m_typeId & asTYPEID_OBJHANDLE) )
    {
        for( size_t i = 0; i < m_children.size(); i++ )
        {
            CSerializedValue *find = m_children[i]->FindByPtrInHandles(ptr);
            if( find )
                return find;
        }
    }

    return 0;
}

void CSerializedValue::Store(void *ref, int typeId)
{
    m_isInit = true;
    SetType(typeId);
    m_originalPtr = ref;

    if( m_typeId & asTYPEID_OBJHANDLE )
    {
        m_handlePtr = *(void**)ref;
    }
    else if( m_typeId & asTYPEID_SCRIPTOBJECT )
    {
        asIScriptObject *obj = (asIScriptObject *)ref;
        asITypeInfo *type = obj->GetObjectType();
        SetType(type->GetTypeId());

        // Store children
        for( asUINT i = 0; i < type->GetPropertyCount(); i++ )
        {
            int childId;
            const char *childName;
            type->GetProperty(i, &childName, &childId);

            m_children.push_back(new CSerializedValue(this, childName, "", obj->GetAddressOfProperty(i), childId));
        }
    }
    else
    {
        int size = m_serializer->m_engine->GetSizeOfPrimitiveType(m_typeId);

        if( size == 0 )
        {
            // if it is user type( string, array, etc ... )
            if( m_serializer->m_userTypes[m_typeName] )
                m_serializer->m_userTypes[m_typeName]->Store(this, m_originalPtr);
            else
            {
                // POD-types can be stored without need for user type
                asITypeInfo *type = GetType();
                if( type && (type->GetFlags() & asOBJ_POD) )
                    size = GetType()->GetSize();

                // It is not necessary to report an error here if it is not a POD-type as that will be done when restoring
            }
        }

        if( size )
        {
            m_mem.resize(size);
            memcpy(&m_mem[0], ref, size);
        }
    }
}

void CSerializedValue::Restore(void *ref, int typeId)
{
    if( !this || !m_isInit || !ref )
        return;

    // Verify that the stored type matched the new type of the value being restored
    if( typeId <= asTYPEID_DOUBLE && typeId != m_typeId ) return; // TODO: We may try to do a type conversion for primitives
    if( (typeId & ~asTYPEID_MASK_SEQNBR) ^ (m_typeId & ~asTYPEID_MASK_SEQNBR) ) return;
    asITypeInfo *type = m_serializer->m_engine->GetTypeInfoById(typeId);
    if( type && m_typeName != type->GetName() ) return;

    // Set the new pointer and type
    m_restorePtr = ref;
    SetType(typeId);

    // Restore the value
    if( m_typeId & asTYPEID_OBJHANDLE )
    {
        // if need create objects
        if( m_children.size() == 1 )
        {
            asITypeInfo *ctype = m_children[0]->GetType();

            if( ctype->GetFactoryCount() == 0 )
            {
                // There are no factories, so assume the same pointer is going to be used
                m_children[0]->m_restorePtr = m_handlePtr;

                // Increase the refCount for the object as it will be released upon clean-up
                m_serializer->m_engine->AddRefScriptObject(m_handlePtr, ctype);
            }
            else
            {
                // Create a new script object, but don't call its constructor as we will initialize the members.
                // Calling the constructor may have unwanted side effects if for example the constructor changes
                // any outside entities, such as setting global variables to point to new objects, etc.
                void *newObject = m_serializer->m_engine->CreateUninitializedScriptObject(ctype);
                m_children[0]->Restore(newObject, ctype->GetTypeId());
            }
        }
    }
    else if( m_typeId & asTYPEID_SCRIPTOBJECT )
    {
        asIScriptObject *obj = (asIScriptObject *)ref;

        // Retrieve children
        for( asUINT i = 0; i < type->GetPropertyCount() ; i++ )
        {
            const char *nameProperty;
            int ptypeId;
            type->GetProperty(i, &nameProperty, &ptypeId);

            CSerializedValue *var = FindByName(nameProperty, "");
            if( var )
                var->Restore(obj->GetAddressOfProperty(i), ptypeId);
        }
    }
    else
    {
        if( m_mem.size() )
        {
            // POD values can be restored with direct copy
            memcpy(ref, &m_mem[0], m_mem.size());
        }
        else if( m_serializer->m_userTypes[m_typeName] )
        {
            // user type restore
            m_serializer->m_userTypes[m_typeName]->Restore(this, m_restorePtr);
        }
        else
        {
            std::string str = "Cannot restore type '";
            str += type->GetName();
            str += "'";
            m_serializer->m_engine->WriteMessage("", 0, 0, asMSGTYPE_ERROR, str.c_str());
        }
    }
}

void CSerializedValue::CancelDuplicates(CSerializedValue *from)
{
    std::vector<void*> ptrs;
    from->GetAllPointersOfChildren(&ptrs);

    for( size_t i = 0; i < ptrs.size(); ++i )
    {
        CSerializedValue *find = m_serializer->m_root.FindByPtrInHandles(ptrs[i]);

        while( find )
        {
            // cancel create object
            find->ClearChildren();

            // Find next link to this ptr
            find = m_serializer->m_root.FindByPtrInHandles(ptrs[i]);
        }
    }
}

void CSerializedValue::ReplaceHandles()
{
    if( m_handlePtr )
    {
        // Find the object that the handle is referring to
        CSerializedValue *handle_to = m_serializer->m_root.FindByPtr(m_handlePtr);

        // If the object hasn't been stored yet...
        if( handle_to == 0 )
        {
            // Store the object now
            asITypeInfo *type = GetType();
            CSerializedValue *need_create = new CSerializedValue(this, m_name, m_nameSpace, m_handlePtr, type->GetTypeId());

            // Make sure all other handles that point to the same object
            // are updated, so we don't end up creating duplicates
            CancelDuplicates(need_create);

            m_children.push_back(need_create);
        }
    }

    // Replace the handles in the children too
    for( size_t i = 0; i < m_children.size(); ++i )
        m_children[i]->ReplaceHandles();
}

void CSerializedValue::RestoreHandles()
{
    if( m_typeId & asTYPEID_OBJHANDLE )
    {
        if( m_handlePtr )
        {
            // Find the object the handle is supposed to point to
            CSerializedValue *handleTo = m_serializer->m_root.FindByPtr(m_handlePtr);

            if( m_restorePtr && handleTo && handleTo->m_restorePtr )
            {
                asITypeInfo *type = m_serializer->m_engine->GetTypeInfoById(m_typeId);

                // If the handle is already pointing to something it must be released first
                if( *(void**)m_restorePtr )
                    m_serializer->m_engine->ReleaseScriptObject(*(void**)m_restorePtr, type);

                // Update the internal pointer
                *(void**)m_restorePtr = handleTo->m_restorePtr;

                // Increase the reference
                m_serializer->m_engine->AddRefScriptObject(handleTo->m_restorePtr, type);
            }
        }
        else
        {
            // If the handle is pointing to something, we must release it to restore the null pointer
            if( m_restorePtr && *(void**)m_restorePtr )
            {
                m_serializer->m_engine->ReleaseScriptObject(*(void**)m_restorePtr, m_serializer->m_engine->GetTypeInfoById(m_typeId));
                *(void**)m_restorePtr = 0;
            }
        }
    }

    // Do the same for the children
    for( size_t i = 0; i < m_children.size(); ++i )
        m_children[i]->RestoreHandles();
}

void CSerializedValue::SetType(int typeId)
{
    m_typeId = typeId;

    asITypeInfo *type = m_serializer->m_engine->GetTypeInfoById(typeId);

    if( type )
        m_typeName = type->GetName();
}

asITypeInfo *CSerializedValue::GetType()
{
    if( !m_typeName.empty() )
    {
        int newTypeId = m_serializer->m_mod->GetTypeIdByDecl(m_typeName.c_str());
        return m_serializer->m_engine->GetTypeInfoById(newTypeId);
    }

    return 0;
}

void CSerializedValue::SetUserData(void *data)
{
    m_userData = data;
}

void *CSerializedValue::GetUserData()
{
    return m_userData;
}

END_AS_NAMESPACE
