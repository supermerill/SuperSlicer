#include "scripthandle.h"
#include <new>
#include <assert.h>
#include <string.h>

BEGIN_AS_NAMESPACE

static void Construct(CScriptHandle *self) { new(self) CScriptHandle(); }
static void Construct(CScriptHandle *self, const CScriptHandle &o) { new(self) CScriptHandle(o); }
// This one is not static because it needs to be friend with the CScriptHandle class
void Construct(CScriptHandle *self, void *ref, int typeId) { new(self) CScriptHandle(ref, typeId); }
static void Destruct(CScriptHandle *self) { self->~CScriptHandle(); }

CScriptHandle::CScriptHandle()
{
    m_ref  = 0;
    m_type = 0;
}

CScriptHandle::CScriptHandle(const CScriptHandle &other)
{
    m_ref  = other.m_ref;
    m_type = other.m_type;

    AddRefHandle();
}

CScriptHandle::CScriptHandle(void *ref, asITypeInfo *type)
{
    m_ref  = ref;
    m_type = type;

    AddRefHandle();
}

// This constructor shouldn't be called from the application
// directly as it requires an active script context
CScriptHandle::CScriptHandle(void *ref, int typeId)
{
    m_ref  = 0;
    m_type = 0;

    Assign(ref, typeId);
}

CScriptHandle::~CScriptHandle()
{
    ReleaseHandle();
}

void CScriptHandle::ReleaseHandle()
{
    if( m_ref && m_type )
    {
        asIScriptEngine *engine = m_type->GetEngine();
        engine->ReleaseScriptObject(m_ref, m_type);

        engine->Release();

        m_ref  = 0;
        m_type = 0;
    }
}

void CScriptHandle::AddRefHandle()
{
    if( m_ref && m_type )
    {
        asIScriptEngine *engine = m_type->GetEngine();
        engine->AddRefScriptObject(m_ref, m_type);

        // Hold on to the engine so it isn't destroyed while
        // a reference to a script object is still held
        engine->AddRef();
    }
}

CScriptHandle &CScriptHandle::operator =(const CScriptHandle &other)
{
    Set(other.m_ref, other.m_type);

    return *this;
}

void CScriptHandle::Set(void *ref, asITypeInfo *type)
{
    if( m_ref == ref ) return;

    ReleaseHandle();

    m_ref  = ref;
    m_type = type;

    AddRefHandle();
}

void *CScriptHandle::GetRef()
{
    return m_ref;
}

asITypeInfo *CScriptHandle::GetType() const
{
    return m_type;
}

int CScriptHandle::GetTypeId() const
{
    if( m_type == 0 ) return 0;

    return m_type->GetTypeId() | asTYPEID_OBJHANDLE;
}

// This method shouldn't be called from the application
// directly as it requires an active script context
CScriptHandle &CScriptHandle::Assign(void *ref, int typeId)
{
    // When receiving a null handle we just clear our memory
    if( typeId == 0 )
    {
        Set(0, 0);
        return *this;
    }

    // Dereference received handles to get the object
    if( typeId & asTYPEID_OBJHANDLE )
    {
        // Store the actual reference
        ref = *(void**)ref;
        typeId &= ~asTYPEID_OBJHANDLE;
    }

    // Get the object type
    asIScriptContext *ctx    = asGetActiveContext();
    asIScriptEngine  *engine = ctx->GetEngine();
    asITypeInfo      *type   = engine->GetTypeInfoById(typeId);

    // If the argument is another CScriptHandle, we should copy the content instead
    if( type && strcmp(type->GetName(), "ref") == 0 )
    {
        CScriptHandle *r = (CScriptHandle*)ref;
        ref  = r->m_ref;
        type = r->m_type;
    }

    Set(ref, type);

    return *this;
}

bool CScriptHandle::operator==(const CScriptHandle &o) const
{
    if( m_ref  == o.m_ref &&
        m_type == o.m_type )
        return true;

    // TODO: If type is not the same, we should attempt to do a dynamic cast,
    //       which may change the pointer for application registered classes

    return false;
}

bool CScriptHandle::operator!=(const CScriptHandle &o) const
{
    return !(*this == o);
}

bool CScriptHandle::Equals(void *ref, int typeId) const
{
    // Null handles are received as reference to a null handle
    if( typeId == 0 )
        ref = 0;

    // Dereference handles to get the object
    if( typeId & asTYPEID_OBJHANDLE )
    {
        // Compare the actual reference
        ref = *(void**)ref;
        typeId &= ~asTYPEID_OBJHANDLE;
    }

    // TODO: If typeId is not the same, we should attempt to do a dynamic cast,
    //       which may change the pointer for application registered classes

    if( ref == m_ref ) return true;

    return false;
}

// AngelScript: used as '@obj = cast<obj>(ref);'
void CScriptHandle::Cast(void **outRef, int typeId)
{
    // If we hold a null handle, then just return null
    if( m_type == 0 )
    {
        *outRef = 0;
        return;
    }

    // It is expected that the outRef is always a handle
    assert( typeId & asTYPEID_OBJHANDLE );

    // Compare the type id of the actual object
    typeId &= ~asTYPEID_OBJHANDLE;
    asIScriptEngine  *engine = m_type->GetEngine();
    asITypeInfo      *type   = engine->GetTypeInfoById(typeId);

    *outRef = 0;

    // RefCastObject will increment the refCount of the returned object if successful
    engine->RefCastObject(m_ref, m_type, type, outRef);
}

void CScriptHandle::EnumReferences(asIScriptEngine *inEngine)
{
    // If we're holding a reference, we'll notify the garbage collector of it
    if (m_ref)
        inEngine->GCEnumCallback(m_ref);

    // The object type itself is also garbage collected
    if( m_type)
        inEngine->GCEnumCallback(m_type);
}

void CScriptHandle::ReleaseReferences(asIScriptEngine *inEngine)
{
    // Simply clear the content to release the references
    Set(0, 0);
}

void RegisterScriptHandle_Native(asIScriptEngine *engine)
{
    int r;

#if AS_CAN_USE_CPP11
    // With C++11 it is possible to use asGetTypeTraits to automatically determine the flags that represent the C++ class
    r = engine->RegisterObjectType("ref", sizeof(CScriptHandle), asOBJ_VALUE | asOBJ_ASHANDLE | asOBJ_GC | asGetTypeTraits<CScriptHandle>()); assert( r >= 0 );
#else
    r = engine->RegisterObjectType("ref", sizeof(CScriptHandle), asOBJ_VALUE | asOBJ_ASHANDLE | asOBJ_GC | asOBJ_APP_CLASS_CDAK); assert( r >= 0 );
#endif
    r = engine->RegisterObjectBehaviour("ref", asBEHAVE_CONSTRUCT, "void f()", asFUNCTIONPR(Construct, (CScriptHandle *), void), asCALL_CDECL_OBJFIRST); assert( r >= 0 );
    r = engine->RegisterObjectBehaviour("ref", asBEHAVE_CONSTRUCT, "void f(const ref &in)", asFUNCTIONPR(Construct, (CScriptHandle *, const CScriptHandle &), void), asCALL_CDECL_OBJFIRST); assert( r >= 0 );
    r = engine->RegisterObjectBehaviour("ref", asBEHAVE_CONSTRUCT, "void f(const ?&in)", asFUNCTIONPR(Construct, (CScriptHandle *, void *, int), void), asCALL_CDECL_OBJFIRST); assert( r >= 0 );
    r = engine->RegisterObjectBehaviour("ref", asBEHAVE_DESTRUCT, "void f()", asFUNCTIONPR(Destruct, (CScriptHandle *), void), asCALL_CDECL_OBJFIRST); assert( r >= 0 );
    r = engine->RegisterObjectBehaviour("ref", asBEHAVE_ENUMREFS, "void f(int&in)", asMETHOD(CScriptHandle,EnumReferences), asCALL_THISCALL); assert(r >= 0);
    r = engine->RegisterObjectBehaviour("ref", asBEHAVE_RELEASEREFS, "void f(int&in)", asMETHOD(CScriptHandle, ReleaseReferences), asCALL_THISCALL); assert(r >= 0);
    r = engine->RegisterObjectMethod("ref", "void opCast(?&out)", asMETHODPR(CScriptHandle, Cast, (void **, int), void), asCALL_THISCALL); assert( r >= 0 );
    r = engine->RegisterObjectMethod("ref", "ref &opHndlAssign(const ref &in)", asMETHOD(CScriptHandle, operator=), asCALL_THISCALL); assert( r >= 0 );
    r = engine->RegisterObjectMethod("ref", "ref &opHndlAssign(const ?&in)", asMETHOD(CScriptHandle, Assign), asCALL_THISCALL); assert( r >= 0 );
    r = engine->RegisterObjectMethod("ref", "bool opEquals(const ref &in) const", asMETHODPR(CScriptHandle, operator==, (const CScriptHandle &) const, bool), asCALL_THISCALL); assert( r >= 0 );
    r = engine->RegisterObjectMethod("ref", "bool opEquals(const ?&in) const", asMETHODPR(CScriptHandle, Equals, (void*, int) const, bool), asCALL_THISCALL); assert( r >= 0 );
}

void CScriptHandle_Construct_Generic(asIScriptGeneric *gen)
{
    CScriptHandle *self = reinterpret_cast<CScriptHandle*>(gen->GetObject());
    new(self) CScriptHandle();
}

void CScriptHandle_ConstructCopy_Generic(asIScriptGeneric *gen)
{
    CScriptHandle *other = reinterpret_cast<CScriptHandle*>(gen->GetArgAddress(0));
    CScriptHandle *self = reinterpret_cast<CScriptHandle*>(gen->GetObject());
    new(self) CScriptHandle(*other);
}

void CScriptHandle_ConstructVar_Generic(asIScriptGeneric *gen)
{
    void *ref = gen->GetArgAddress(0);
    int typeId = gen->GetArgTypeId(0);
    CScriptHandle *self = reinterpret_cast<CScriptHandle*>(gen->GetObject());
    Construct(self, ref, typeId);
}

void CScriptHandle_Destruct_Generic(asIScriptGeneric *gen)
{
    CScriptHandle *self = reinterpret_cast<CScriptHandle*>(gen->GetObject());
    self->~CScriptHandle();
}

void CScriptHandle_Cast_Generic(asIScriptGeneric *gen)
{
    void **ref = reinterpret_cast<void**>(gen->GetArgAddress(0));
    int typeId = gen->GetArgTypeId(0);
    CScriptHandle *self = reinterpret_cast<CScriptHandle*>(gen->GetObject());
    self->Cast(ref, typeId);
}

void CScriptHandle_Assign_Generic(asIScriptGeneric *gen)
{
    CScriptHandle *other = reinterpret_cast<CScriptHandle*>(gen->GetArgAddress(0));
    CScriptHandle *self = reinterpret_cast<CScriptHandle*>(gen->GetObject());
    *self = *other;
    gen->SetReturnAddress(self);
}

void CScriptHandle_AssignVar_Generic(asIScriptGeneric *gen)
{
    void *ref = gen->GetArgAddress(0);
    int typeId = gen->GetArgTypeId(0);
    CScriptHandle *self = reinterpret_cast<CScriptHandle*>(gen->GetObject());
    self->Assign(ref, typeId);
    gen->SetReturnAddress(self);
}

void CScriptHandle_Equals_Generic(asIScriptGeneric *gen)
{
    CScriptHandle *other = reinterpret_cast<CScriptHandle*>(gen->GetArgAddress(0));
    CScriptHandle *self = reinterpret_cast<CScriptHandle*>(gen->GetObject());
    gen->SetReturnByte(*self == *other);
}

void CScriptHandle_EqualsVar_Generic(asIScriptGeneric *gen)
{
    void *ref = gen->GetArgAddress(0);
    int typeId = gen->GetArgTypeId(0);
    CScriptHandle *self = reinterpret_cast<CScriptHandle*>(gen->GetObject());
    gen->SetReturnByte(self->Equals(ref, typeId));
}

void CScriptHandle_EnumReferences_Generic(asIScriptGeneric *gen)
{
    CScriptHandle *self = reinterpret_cast<CScriptHandle*>(gen->GetObject());
    self->EnumReferences(gen->GetEngine());
}

void CScriptHandle_ReleaseReferences_Generic(asIScriptGeneric *gen)
{
    CScriptHandle *self = reinterpret_cast<CScriptHandle*>(gen->GetObject());
    self->ReleaseReferences(gen->GetEngine());
}

void RegisterScriptHandle_Generic(asIScriptEngine *engine)
{
    int r;

    r = engine->RegisterObjectType("ref", sizeof(CScriptHandle), asOBJ_VALUE | asOBJ_ASHANDLE | asOBJ_GC | asOBJ_APP_CLASS_CDAK); assert( r >= 0 );
    r = engine->RegisterObjectBehaviour("ref", asBEHAVE_CONSTRUCT, "void f()", asFUNCTION(CScriptHandle_Construct_Generic), asCALL_GENERIC); assert( r >= 0 );
    r = engine->RegisterObjectBehaviour("ref", asBEHAVE_CONSTRUCT, "void f(const ref &in)", asFUNCTION(CScriptHandle_ConstructCopy_Generic), asCALL_GENERIC); assert( r >= 0 );
    r = engine->RegisterObjectBehaviour("ref", asBEHAVE_CONSTRUCT, "void f(const ?&in)", asFUNCTION(CScriptHandle_ConstructVar_Generic), asCALL_GENERIC); assert( r >= 0 );
    r = engine->RegisterObjectBehaviour("ref", asBEHAVE_DESTRUCT, "void f()", asFUNCTION(CScriptHandle_Destruct_Generic), asCALL_GENERIC); assert( r >= 0 );
    r = engine->RegisterObjectBehaviour("ref", asBEHAVE_ENUMREFS, "void f(int&in)", asFUNCTION(CScriptHandle_EnumReferences_Generic), asCALL_GENERIC); assert(r >= 0);
    r = engine->RegisterObjectBehaviour("ref", asBEHAVE_RELEASEREFS, "void f(int&in)", asFUNCTION(CScriptHandle_ReleaseReferences_Generic), asCALL_GENERIC); assert(r >= 0);
    r = engine->RegisterObjectMethod("ref", "void opCast(?&out)", asFUNCTION(CScriptHandle_Cast_Generic), asCALL_GENERIC); assert( r >= 0 );
    r = engine->RegisterObjectMethod("ref", "ref &opHndlAssign(const ref &in)", asFUNCTION(CScriptHandle_Assign_Generic), asCALL_GENERIC); assert( r >= 0 );
    r = engine->RegisterObjectMethod("ref", "ref &opHndlAssign(const ?&in)", asFUNCTION(CScriptHandle_AssignVar_Generic), asCALL_GENERIC); assert( r >= 0 );
    r = engine->RegisterObjectMethod("ref", "bool opEquals(const ref &in) const", asFUNCTION(CScriptHandle_Equals_Generic), asCALL_GENERIC); assert( r >= 0 );
    r = engine->RegisterObjectMethod("ref", "bool opEquals(const ?&in) const", asFUNCTION(CScriptHandle_EqualsVar_Generic), asCALL_GENERIC); assert( r >= 0 );
}

void RegisterScriptHandle(asIScriptEngine *engine)
{
    if( strstr(asGetLibraryOptions(), "AS_MAX_PORTABILITY") )
        RegisterScriptHandle_Generic(engine);
    else
        RegisterScriptHandle_Native(engine);
}


END_AS_NAMESPACE
