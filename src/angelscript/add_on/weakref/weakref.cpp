
// The CScriptWeakRef class was originally implemented by vroad in March 2013

#include "weakref.h"
#include <new>
#include <assert.h>
#include <string.h> // strstr()

BEGIN_AS_NAMESPACE

static void ScriptWeakRefConstruct(asITypeInfo *type, void *mem)
{
    new(mem) CScriptWeakRef(type);
}

static void ScriptWeakRefConstruct2(asITypeInfo *type, void *ref, void *mem)
{
    new(mem) CScriptWeakRef(ref, type);

    // It's possible the constructor raised a script exception, in which case we
    // need to call the destructor in order to cleanup the memory before returning
    asIScriptContext *ctx = asGetActiveContext();
    if( ctx && ctx->GetState() == asEXECUTION_EXCEPTION )
        reinterpret_cast<CScriptWeakRef*>(mem)->~CScriptWeakRef();
}

static void ScriptWeakRefDestruct(CScriptWeakRef *obj)
{
    obj->~CScriptWeakRef();
}

static bool ScriptWeakRefTemplateCallback(asITypeInfo *ti, bool &/*dontGarbageCollect*/)
{
    asITypeInfo *subType = ti->GetSubType();

    // Weak references only work for reference types
    if( subType == 0 ) return false;
    if( !(subType->GetFlags() & asOBJ_REF) ) return false;

    // The subtype shouldn't be a handle
    if( ti->GetSubTypeId() & asTYPEID_OBJHANDLE )
        return false;

    // Make sure the type really supports weak references
    asUINT cnt = subType->GetBehaviourCount();
    for( asUINT n = 0; n < cnt; n++ )
    {
        asEBehaviours beh;
        subType->GetBehaviourByIndex(n, &beh);
        if( beh == asBEHAVE_GET_WEAKREF_FLAG )
            return true;
    }

    ti->GetEngine()->WriteMessage("weakref", 0, 0, asMSGTYPE_ERROR, "The subtype doesn't support weak references");
    return false;
}

CScriptWeakRef::CScriptWeakRef(asITypeInfo *type)
{
    m_ref  = 0;
    m_type = type;
    m_type->AddRef();
    m_weakRefFlag = 0;
}

CScriptWeakRef::CScriptWeakRef(const CScriptWeakRef &other)
{
    m_ref  = other.m_ref;
    m_type = other.m_type;
    m_type->AddRef();
    m_weakRefFlag = other.m_weakRefFlag;
    if( m_weakRefFlag )
        m_weakRefFlag->AddRef();
}

CScriptWeakRef::CScriptWeakRef(void *ref, asITypeInfo *type)
{
    m_ref  = ref;
    m_type = type;
    m_type->AddRef();

    // The given type should be the weakref template instance
    assert( strcmp(type->GetName(), "weakref") == 0 ||
            strcmp(type->GetName(), "const_weakref") == 0 );

    // Get the shared flag that will tell us when the object has been destroyed
    // This is threadsafe as we hold a strong reference to the object
    m_weakRefFlag = m_type->GetEngine()->GetWeakRefFlagOfScriptObject(m_ref, m_type->GetSubType());
    if( m_weakRefFlag )
        m_weakRefFlag->AddRef();
}

CScriptWeakRef::~CScriptWeakRef()
{
    if( m_type )
        m_type->Release();
    if( m_weakRefFlag )
        m_weakRefFlag->Release();
}

CScriptWeakRef &CScriptWeakRef::operator =(const CScriptWeakRef &other)
{
    // Don't do anything if it is the same reference
    // It is not enough to verify only the reference to the object, as the
    // address may be reused by another instance after the first has been freed.
    // By checking also the weakRefFlag we can be certain that it is the same
    // instance.
    if( m_ref == other.m_ref &&
        m_weakRefFlag == other.m_weakRefFlag )
        return *this;

    // Must not allow changing the type
    if( m_type != other.m_type )
    {
        // We can allow a weakref to be assigned to a const_weakref
        if( !(strcmp(m_type->GetName(), "const_weakref") == 0 &&
              strcmp(other.m_type->GetName(), "weakref") == 0 &&
              m_type->GetSubType() == other.m_type->GetSubType()) )
        {
              assert( false );
              return *this;
        }
    }

    m_ref = other.m_ref;

    if( m_weakRefFlag )
        m_weakRefFlag->Release();
    m_weakRefFlag = other.m_weakRefFlag;
    if( m_weakRefFlag )
        m_weakRefFlag->AddRef();

    return *this;
}

CScriptWeakRef &CScriptWeakRef::Set(void *newRef)
{
    // Release the previous weak ref
    if( m_weakRefFlag )
        m_weakRefFlag->Release();

    // Retrieve the new weak ref
    m_ref = newRef;
    if( newRef )
    {
        m_weakRefFlag = m_type->GetEngine()->GetWeakRefFlagOfScriptObject(newRef, m_type->GetSubType());
        m_weakRefFlag->AddRef();
    }
    else
        m_weakRefFlag = 0;

    // Release the newRef since we're only supposed to hold a weakref
    m_type->GetEngine()->ReleaseScriptObject(newRef, m_type->GetSubType());

    return *this;
}

asITypeInfo *CScriptWeakRef::GetRefType() const
{
    return m_type->GetSubType();
}

bool CScriptWeakRef::operator==(const CScriptWeakRef &o) const
{
    // It is not enough to compare just the address of the object, as it may
    // be reused by another instance after the first has been freed. By verifying
    // also the weakRefFlag we can guarantee that it is indeed the same instance.
    if( m_ref  == o.m_ref &&
        m_weakRefFlag == o.m_weakRefFlag &&
        m_type == o.m_type )
        return true;

    // TODO: If type is not the same, we should attempt to do a dynamic cast,
    //       which may change the pointer for application registered classes

    return false;
}

bool CScriptWeakRef::operator!=(const CScriptWeakRef &o) const
{
    return !(*this == o);
}

// AngelScript: used as '@obj = ref.get();'
void *CScriptWeakRef::Get() const
{
    // If we hold a null handle, then just return null
    if( m_ref == 0 || m_weakRefFlag == 0 )
        return 0;

    // Lock on the shared bool, so we can be certain it won't be changed to true
    // between the inspection of the flag and the increase of the ref count in the
    // owning object.
    m_weakRefFlag->Lock();
    if( !m_weakRefFlag->Get() )
    {
        m_type->GetEngine()->AddRefScriptObject(m_ref, m_type->GetSubType());
        m_weakRefFlag->Unlock();
        return m_ref;
    }
    m_weakRefFlag->Unlock();

    return 0;
}

bool CScriptWeakRef::Equals(void *ref) const
{
    if( m_ref != ref )
        return false;

    // It is not enough to compare just the address, as another instance may
    // get the same address after the first instance has been freed. Verify the
    // weakref flag too to make sure it is the same instance
    asILockableSharedBool *flag = m_type->GetEngine()->GetWeakRefFlagOfScriptObject(ref, m_type->GetSubType());
    if (m_weakRefFlag != flag)
        return false;

    return true;
}

void RegisterScriptWeakRef_Native(asIScriptEngine *engine)
{
    int r;

    // Register a type for non-const handles
    r = engine->RegisterObjectType("weakref<class T>", sizeof(CScriptWeakRef), asOBJ_VALUE | asOBJ_ASHANDLE | asOBJ_TEMPLATE | asOBJ_APP_CLASS_DAK); assert( r >= 0 );

    r = engine->RegisterObjectBehaviour("weakref<T>", asBEHAVE_CONSTRUCT, "void f(int&in)", asFUNCTION(ScriptWeakRefConstruct), asCALL_CDECL_OBJLAST); assert( r>= 0 );
    r = engine->RegisterObjectBehaviour("weakref<T>", asBEHAVE_CONSTRUCT, "void f(int&in, T@+)", asFUNCTION(ScriptWeakRefConstruct2), asCALL_CDECL_OBJLAST); assert( r>= 0 );
    r = engine->RegisterObjectBehaviour("weakref<T>", asBEHAVE_DESTRUCT, "void f()", asFUNCTION(ScriptWeakRefDestruct), asCALL_CDECL_OBJLAST); assert( r >= 0 );
    r = engine->RegisterObjectBehaviour("weakref<T>", asBEHAVE_TEMPLATE_CALLBACK, "bool f(int&in, bool&out)", asFUNCTION(ScriptWeakRefTemplateCallback), asCALL_CDECL); assert( r >= 0 );

    r = engine->RegisterObjectMethod("weakref<T>", "T@ opImplCast()", asMETHOD(CScriptWeakRef, Get), asCALL_THISCALL); assert(r >= 0);
    r = engine->RegisterObjectMethod("weakref<T>", "T@ get() const", asMETHODPR(CScriptWeakRef, Get, () const, void*), asCALL_THISCALL); assert( r >= 0 );
    r = engine->RegisterObjectMethod("weakref<T>", "weakref<T> &opHndlAssign(const weakref<T> &in)", asMETHOD(CScriptWeakRef, operator=), asCALL_THISCALL); assert( r >= 0 );
    r = engine->RegisterObjectMethod("weakref<T>", "weakref<T> &opAssign(const weakref<T> &in)", asMETHOD(CScriptWeakRef, operator=), asCALL_THISCALL); assert(r >= 0);
    r = engine->RegisterObjectMethod("weakref<T>", "bool opEquals(const weakref<T> &in) const", asMETHODPR(CScriptWeakRef, operator==, (const CScriptWeakRef &) const, bool), asCALL_THISCALL); assert( r >= 0 );
    r = engine->RegisterObjectMethod("weakref<T>", "weakref<T> &opHndlAssign(T@)", asMETHOD(CScriptWeakRef, Set), asCALL_THISCALL); assert( r >= 0 );
    r = engine->RegisterObjectMethod("weakref<T>", "bool opEquals(const T@+) const", asMETHOD(CScriptWeakRef, Equals), asCALL_THISCALL); assert(r >= 0);

    // Register another type for const handles
    r = engine->RegisterObjectType("const_weakref<class T>", sizeof(CScriptWeakRef), asOBJ_VALUE | asOBJ_ASHANDLE | asOBJ_TEMPLATE | asOBJ_APP_CLASS_DAK); assert( r >= 0 );

    r = engine->RegisterObjectBehaviour("const_weakref<T>", asBEHAVE_CONSTRUCT, "void f(int&in)", asFUNCTION(ScriptWeakRefConstruct), asCALL_CDECL_OBJLAST); assert( r>= 0 );
    r = engine->RegisterObjectBehaviour("const_weakref<T>", asBEHAVE_CONSTRUCT, "void f(int&in, const T@+)", asFUNCTION(ScriptWeakRefConstruct2), asCALL_CDECL_OBJLAST); assert( r>= 0 );
    r = engine->RegisterObjectBehaviour("const_weakref<T>", asBEHAVE_DESTRUCT, "void f()", asFUNCTION(ScriptWeakRefDestruct), asCALL_CDECL_OBJLAST); assert( r >= 0 );
    r = engine->RegisterObjectBehaviour("const_weakref<T>", asBEHAVE_TEMPLATE_CALLBACK, "bool f(int&in, bool&out)", asFUNCTION(ScriptWeakRefTemplateCallback), asCALL_CDECL); assert( r >= 0 );

    r = engine->RegisterObjectMethod("const_weakref<T>", "const T@ opImplCast() const", asMETHOD(CScriptWeakRef, Get), asCALL_THISCALL); assert(r >= 0);
    r = engine->RegisterObjectMethod("const_weakref<T>", "const T@ get() const", asMETHODPR(CScriptWeakRef, Get, () const, void*), asCALL_THISCALL); assert( r >= 0 );
    r = engine->RegisterObjectMethod("const_weakref<T>", "const_weakref<T> &opHndlAssign(const const_weakref<T> &in)", asMETHOD(CScriptWeakRef, operator=), asCALL_THISCALL); assert( r >= 0 );
    r = engine->RegisterObjectMethod("const_weakref<T>", "const_weakref<T> &opAssign(const const_weakref<T> &in)", asMETHOD(CScriptWeakRef, operator=), asCALL_THISCALL); assert(r >= 0);
    r = engine->RegisterObjectMethod("const_weakref<T>", "bool opEquals(const const_weakref<T> &in) const", asMETHODPR(CScriptWeakRef, operator==, (const CScriptWeakRef &) const, bool), asCALL_THISCALL); assert( r >= 0 );
    r = engine->RegisterObjectMethod("const_weakref<T>", "const_weakref<T> &opHndlAssign(const T@)", asMETHOD(CScriptWeakRef, Set), asCALL_THISCALL); assert( r >= 0 );
    r = engine->RegisterObjectMethod("const_weakref<T>", "bool opEquals(const T@+) const", asMETHOD(CScriptWeakRef, Equals), asCALL_THISCALL); assert(r >= 0);

    // Allow non-const weak references to be converted to const weak references
    r = engine->RegisterObjectMethod("const_weakref<T>", "const_weakref<T> &opHndlAssign(const weakref<T> &in)", asMETHOD(CScriptWeakRef, operator=), asCALL_THISCALL); assert( r >= 0 );
    r = engine->RegisterObjectMethod("const_weakref<T>", "bool opEquals(const weakref<T> &in) const", asMETHODPR(CScriptWeakRef, operator==, (const CScriptWeakRef &) const, bool), asCALL_THISCALL); assert( r >= 0 );
}

static void ScriptWeakRefConstruct_Generic(asIScriptGeneric *gen)
{
    asITypeInfo *ti = *reinterpret_cast<asITypeInfo**>(gen->GetAddressOfArg(0));

    ScriptWeakRefConstruct(ti, gen->GetObject());
}

static void ScriptWeakRefConstruct2_Generic(asIScriptGeneric *gen)
{
    asITypeInfo *ti = *reinterpret_cast<asITypeInfo**>(gen->GetAddressOfArg(0));
    void *ref = gen->GetArgAddress(1);

    ScriptWeakRefConstruct2(ti, ref, gen->GetObject());
}

static void ScriptWeakRefDestruct_Generic(asIScriptGeneric *gen)
{
    CScriptWeakRef *self = reinterpret_cast<CScriptWeakRef*>(gen->GetObject());
    self->~CScriptWeakRef();
}

void CScriptWeakRef_Get_Generic(asIScriptGeneric *gen)
{
    CScriptWeakRef *self = reinterpret_cast<CScriptWeakRef*>(gen->GetObject());
    gen->SetReturnAddress(self->Get());
}

void CScriptWeakRef_Assign_Generic(asIScriptGeneric *gen)
{
    CScriptWeakRef *other = reinterpret_cast<CScriptWeakRef*>(gen->GetArgAddress(0));
    CScriptWeakRef *self = reinterpret_cast<CScriptWeakRef*>(gen->GetObject());
    *self = *other;
    gen->SetReturnAddress(self);
}

void CScriptWeakRef_Assign2_Generic(asIScriptGeneric *gen)
{
    void *other = gen->GetArgAddress(0);
    CScriptWeakRef *self = reinterpret_cast<CScriptWeakRef*>(gen->GetObject());
    self->Set(other);
    gen->SetReturnAddress(self);
}

void CScriptWeakRef_Equals_Generic(asIScriptGeneric *gen)
{
    CScriptWeakRef *other = reinterpret_cast<CScriptWeakRef*>(gen->GetArgAddress(0));
    CScriptWeakRef *self = reinterpret_cast<CScriptWeakRef*>(gen->GetObject());
    gen->SetReturnByte(*self == *other);
}

void CScriptWeakRef_Equals2_Generic(asIScriptGeneric *gen)
{
    void *other = gen->GetArgAddress(0);
    CScriptWeakRef *self = reinterpret_cast<CScriptWeakRef*>(gen->GetObject());

    gen->SetReturnByte(self->Equals(other));
}

static void ScriptWeakRefTemplateCallback_Generic(asIScriptGeneric *gen)
{
    asITypeInfo *ti = *reinterpret_cast<asITypeInfo**>(gen->GetAddressOfArg(0));
    bool *dontGarbageCollect = *reinterpret_cast<bool**>(gen->GetAddressOfArg(1));
    *reinterpret_cast<bool*>(gen->GetAddressOfReturnLocation()) = ScriptWeakRefTemplateCallback(ti, *dontGarbageCollect);
}

void RegisterScriptWeakRef_Generic(asIScriptEngine *engine)
{
    int r;

    // Register a type for non-const handles
    r = engine->RegisterObjectType("weakref<class T>", sizeof(CScriptWeakRef), asOBJ_VALUE | asOBJ_ASHANDLE | asOBJ_TEMPLATE | asOBJ_APP_CLASS_DAK); assert( r >= 0 );

    r = engine->RegisterObjectBehaviour("weakref<T>", asBEHAVE_CONSTRUCT, "void f(int&in)", asFUNCTION(ScriptWeakRefConstruct_Generic), asCALL_GENERIC); assert( r>= 0 );
    r = engine->RegisterObjectBehaviour("weakref<T>", asBEHAVE_CONSTRUCT, "void f(int&in, T@+)", asFUNCTION(ScriptWeakRefConstruct2_Generic), asCALL_GENERIC); assert( r>= 0 );
    r = engine->RegisterObjectBehaviour("weakref<T>", asBEHAVE_TEMPLATE_CALLBACK, "bool f(int&in, bool&out)", asFUNCTION(ScriptWeakRefTemplateCallback_Generic), asCALL_GENERIC); assert( r >= 0 );
    r = engine->RegisterObjectBehaviour("weakref<T>", asBEHAVE_DESTRUCT, "void f()", asFUNCTION(ScriptWeakRefDestruct_Generic), asCALL_GENERIC); assert( r >= 0 );

    r = engine->RegisterObjectMethod("weakref<T>", "T@ opImplCast()", asFUNCTION(CScriptWeakRef_Get_Generic), asCALL_GENERIC); assert(r >= 0);
    r = engine->RegisterObjectMethod("weakref<T>", "T@ get() const", asFUNCTION(CScriptWeakRef_Get_Generic), asCALL_GENERIC); assert( r >= 0 );
    r = engine->RegisterObjectMethod("weakref<T>", "weakref<T> &opHndlAssign(const weakref<T> &in)", asFUNCTION(CScriptWeakRef_Assign_Generic), asCALL_GENERIC); assert( r >= 0 );
    r = engine->RegisterObjectMethod("weakref<T>", "weakref<T> &opAssign(const weakref<T> &in)", asFUNCTION(CScriptWeakRef_Assign_Generic), asCALL_GENERIC); assert(r >= 0);
    r = engine->RegisterObjectMethod("weakref<T>", "bool opEquals(const weakref<T> &in) const", asFUNCTION(CScriptWeakRef_Equals_Generic), asCALL_GENERIC); assert( r >= 0 );
    r = engine->RegisterObjectMethod("weakref<T>", "weakref<T> &opHndlAssign(T@)", asFUNCTION(CScriptWeakRef_Assign2_Generic), asCALL_GENERIC); assert( r >= 0 );
    r = engine->RegisterObjectMethod("weakref<T>", "bool opEquals(const T@+) const", asFUNCTION(CScriptWeakRef_Equals2_Generic), asCALL_GENERIC); assert(r >= 0);

    // Register another type for const handles
    r = engine->RegisterObjectType("const_weakref<class T>", sizeof(CScriptWeakRef), asOBJ_VALUE | asOBJ_ASHANDLE | asOBJ_TEMPLATE | asOBJ_APP_CLASS_DAK); assert( r >= 0 );

    r = engine->RegisterObjectBehaviour("const_weakref<T>", asBEHAVE_CONSTRUCT, "void f(int&in)", asFUNCTION(ScriptWeakRefConstruct_Generic), asCALL_GENERIC); assert( r>= 0 );
    r = engine->RegisterObjectBehaviour("const_weakref<T>", asBEHAVE_CONSTRUCT, "void f(int&in, const T@+)", asFUNCTION(ScriptWeakRefConstruct2_Generic), asCALL_GENERIC); assert( r>= 0 );
    r = engine->RegisterObjectBehaviour("const_weakref<T>", asBEHAVE_TEMPLATE_CALLBACK, "bool f(int&in, bool&out)", asFUNCTION(ScriptWeakRefTemplateCallback_Generic), asCALL_GENERIC); assert( r >= 0 );
    r = engine->RegisterObjectBehaviour("const_weakref<T>", asBEHAVE_DESTRUCT, "void f()", asFUNCTION(ScriptWeakRefDestruct_Generic), asCALL_GENERIC); assert( r >= 0 );

    r = engine->RegisterObjectMethod("const_weakref<T>", "const T@ opImplCast() const", asFUNCTION(CScriptWeakRef_Get_Generic), asCALL_GENERIC); assert(r >= 0);
    r = engine->RegisterObjectMethod("const_weakref<T>", "const T@ get() const", asFUNCTION(CScriptWeakRef_Get_Generic), asCALL_GENERIC); assert( r >= 0 );
    r = engine->RegisterObjectMethod("const_weakref<T>", "const_weakref<T> &opHndlAssign(const const_weakref<T> &in)", asFUNCTION(CScriptWeakRef_Assign_Generic), asCALL_GENERIC); assert( r >= 0 );
    r = engine->RegisterObjectMethod("const_weakref<T>", "const_weakref<T> &opAssign(const const_weakref<T> &in)", asFUNCTION(CScriptWeakRef_Assign_Generic), asCALL_GENERIC); assert(r >= 0);
    r = engine->RegisterObjectMethod("const_weakref<T>", "bool opEquals(const const_weakref<T> &in) const", asFUNCTION(CScriptWeakRef_Equals_Generic), asCALL_GENERIC); assert( r >= 0 );
    r = engine->RegisterObjectMethod("const_weakref<T>", "const_weakref<T> &opHndlAssign(const T@)", asFUNCTION(CScriptWeakRef_Assign2_Generic), asCALL_GENERIC); assert( r >= 0 );
    r = engine->RegisterObjectMethod("const_weakref<T>", "bool opEquals(const T@+) const", asFUNCTION(CScriptWeakRef_Equals2_Generic), asCALL_GENERIC); assert(r >= 0);

    // Allow non-const weak references to be converted to const weak references
    r = engine->RegisterObjectMethod("const_weakref<T>", "const_weakref<T> &opHndlAssign(const weakref<T> &in)", asFUNCTION(CScriptWeakRef_Assign_Generic), asCALL_GENERIC); assert( r >= 0 );
    r = engine->RegisterObjectMethod("const_weakref<T>", "bool opEquals(const weakref<T> &in) const", asFUNCTION(CScriptWeakRef_Equals_Generic), asCALL_GENERIC); assert( r >= 0 );
}

void RegisterScriptWeakRef(asIScriptEngine *engine)
{
    if( strstr(asGetLibraryOptions(), "AS_MAX_PORTABILITY") )
        RegisterScriptWeakRef_Generic(engine);
    else
        RegisterScriptWeakRef_Native(engine);
}


END_AS_NAMESPACE
