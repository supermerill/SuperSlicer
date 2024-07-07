#include <new>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h> // sprintf

#include "scriptgrid.h"

using namespace std;

BEGIN_AS_NAMESPACE

// Set the default memory routines
// Use the angelscript engine's memory routines by default
static asALLOCFUNC_t userAlloc = asAllocMem;
static asFREEFUNC_t  userFree  = asFreeMem;

// Allows the application to set which memory routines should be used by the array object
void CScriptGrid::SetMemoryFunctions(asALLOCFUNC_t allocFunc, asFREEFUNC_t freeFunc)
{
    userAlloc = allocFunc;
    userFree = freeFunc;
}

static void RegisterScriptGrid_Native(asIScriptEngine *engine);

struct SGridBuffer
{
    asDWORD width;
    asDWORD height;
    asBYTE  data[1];
};

CScriptGrid *CScriptGrid::Create(asITypeInfo *ti)
{
    return CScriptGrid::Create(ti, 0, 0);
}

CScriptGrid *CScriptGrid::Create(asITypeInfo *ti, asUINT w, asUINT h)
{
    // Allocate the memory
    void *mem = userAlloc(sizeof(CScriptGrid));
    if( mem == 0 )
    {
        asIScriptContext *ctx = asGetActiveContext();
        if( ctx )
            ctx->SetException("Out of memory");

        return 0;
    }

    // Initialize the object
    CScriptGrid *a = new(mem) CScriptGrid(w, h, ti);

    return a;
}

CScriptGrid *CScriptGrid::Create(asITypeInfo *ti, void *initList)
{
    // Allocate the memory
    void *mem = userAlloc(sizeof(CScriptGrid));
    if( mem == 0 )
    {
        asIScriptContext *ctx = asGetActiveContext();
        if( ctx )
            ctx->SetException("Out of memory");

        return 0;
    }

    // Initialize the object
    CScriptGrid *a = new(mem) CScriptGrid(ti, initList);

    return a;
}

CScriptGrid *CScriptGrid::Create(asITypeInfo *ti, asUINT w, asUINT h, void *defVal)
{
    // Allocate the memory
    void *mem = userAlloc(sizeof(CScriptGrid));
    if( mem == 0 )
    {
        asIScriptContext *ctx = asGetActiveContext();
        if( ctx )
            ctx->SetException("Out of memory");

        return 0;
    }

    // Initialize the object
    CScriptGrid *a = new(mem) CScriptGrid(w, h, defVal, ti);

    return a;
}

// This optional callback is called when the template type is first used by the compiler.
// It allows the application to validate if the template can be instantiated for the requested
// subtype at compile time, instead of at runtime. The output argument dontGarbageCollect
// allow the callback to tell the engine if the template instance type shouldn't be garbage collected,
// i.e. no asOBJ_GC flag.
static bool ScriptGridTemplateCallback(asITypeInfo *ti, bool &dontGarbageCollect)
{
    // Make sure the subtype can be instantiated with a default factory/constructor,
    // otherwise we won't be able to instantiate the elements.
    int typeId = ti->GetSubTypeId();
    if( typeId == asTYPEID_VOID )
        return false;
    if( (typeId & asTYPEID_MASK_OBJECT) && !(typeId & asTYPEID_OBJHANDLE) )
    {
        asITypeInfo *subtype = ti->GetEngine()->GetTypeInfoById(typeId);
        asDWORD flags = subtype->GetFlags();
        if( (flags & asOBJ_VALUE) && !(flags & asOBJ_POD) )
        {
            // Verify that there is a default constructor
            bool found = false;
            for( asUINT n = 0; n < subtype->GetBehaviourCount(); n++ )
            {
                asEBehaviours beh;
                asIScriptFunction *func = subtype->GetBehaviourByIndex(n, &beh);
                if( beh != asBEHAVE_CONSTRUCT ) continue;

                if( func->GetParamCount() == 0 )
                {
                    // Found the default constructor
                    found = true;
                    break;
                }
            }

            if( !found )
            {
                // There is no default constructor
                ti->GetEngine()->WriteMessage("array", 0, 0, asMSGTYPE_ERROR, "The subtype has no default constructor");
                return false;
            }
        }
        else if( (flags & asOBJ_REF) )
        {
            bool found = false;

            // If value assignment for ref type has been disabled then the array
            // can be created if the type has a default factory function
            if( !ti->GetEngine()->GetEngineProperty(asEP_DISALLOW_VALUE_ASSIGN_FOR_REF_TYPE) )
            {
                // Verify that there is a default factory
                for( asUINT n = 0; n < subtype->GetFactoryCount(); n++ )
                {
                    asIScriptFunction *func = subtype->GetFactoryByIndex(n);
                    if( func->GetParamCount() == 0 )
                    {
                        // Found the default factory
                        found = true;
                        break;
                    }
                }
            }

            if( !found )
            {
                // No default factory
                ti->GetEngine()->WriteMessage("array", 0, 0, asMSGTYPE_ERROR, "The subtype has no default factory");
                return false;
            }
        }

        // If the object type is not garbage collected then the array also doesn't need to be
        if( !(flags & asOBJ_GC) )
            dontGarbageCollect = true;
    }
    else if( !(typeId & asTYPEID_OBJHANDLE) )
    {
        // Arrays with primitives cannot form circular references,
        // thus there is no need to garbage collect them
        dontGarbageCollect = true;
    }
    else
    {
        assert( typeId & asTYPEID_OBJHANDLE );

        // It is not necessary to set the array as garbage collected for all handle types.
        // If it is possible to determine that the handle cannot refer to an object type
        // that can potentially form a circular reference with the array then it is not
        // necessary to make the array garbage collected.
        asITypeInfo *subtype = ti->GetEngine()->GetTypeInfoById(typeId);
        asDWORD flags = subtype->GetFlags();
        if( !(flags & asOBJ_GC) )
        {
            if( (flags & asOBJ_SCRIPT_OBJECT) )
            {
                // Even if a script class is by itself not garbage collected, it is possible
                // that classes that derive from it may be, so it is not possible to know
                // that no circular reference can occur.
                if( (flags & asOBJ_NOINHERIT) )
                {
                    // A script class declared as final cannot be inherited from, thus
                    // we can be certain that the object cannot be garbage collected.
                    dontGarbageCollect = true;
                }
            }
            else
            {
                // For application registered classes we assume the application knows
                // what it is doing and don't mark the array as garbage collected unless
                // the type is also garbage collected.
                dontGarbageCollect = true;
            }
        }
    }

    // The type is ok
    return true;
}

// Registers the template array type
void RegisterScriptGrid(asIScriptEngine *engine)
{
    // TODO: Implement the generic calling convention
    RegisterScriptGrid_Native(engine);
}

static void RegisterScriptGrid_Native(asIScriptEngine *engine)
{
    int r;

    // Register the grid type as a template
    r = engine->RegisterObjectType("grid<class T>", 0, asOBJ_REF | asOBJ_GC | asOBJ_TEMPLATE); assert( r >= 0 );

    // Register a callback for validating the subtype before it is used
    r = engine->RegisterObjectBehaviour("grid<T>", asBEHAVE_TEMPLATE_CALLBACK, "bool f(int&in, bool&out)", asFUNCTION(ScriptGridTemplateCallback), asCALL_CDECL); assert( r >= 0 );

    // Templates receive the object type as the first parameter. To the script writer this is hidden
    r = engine->RegisterObjectBehaviour("grid<T>", asBEHAVE_FACTORY, "grid<T>@ f(int&in)", asFUNCTIONPR(CScriptGrid::Create, (asITypeInfo*), CScriptGrid*), asCALL_CDECL); assert( r >= 0 );
    r = engine->RegisterObjectBehaviour("grid<T>", asBEHAVE_FACTORY, "grid<T>@ f(int&in, uint, uint)", asFUNCTIONPR(CScriptGrid::Create, (asITypeInfo*, asUINT, asUINT), CScriptGrid*), asCALL_CDECL); assert( r >= 0 );
    r = engine->RegisterObjectBehaviour("grid<T>", asBEHAVE_FACTORY, "grid<T>@ f(int&in, uint, uint, const T &in)", asFUNCTIONPR(CScriptGrid::Create, (asITypeInfo*, asUINT, asUINT, void *), CScriptGrid*), asCALL_CDECL); assert( r >= 0 );

    // Register the factory that will be used for initialization lists
    r = engine->RegisterObjectBehaviour("grid<T>", asBEHAVE_LIST_FACTORY, "grid<T>@ f(int&in type, int&in list) {repeat {repeat_same T}}", asFUNCTIONPR(CScriptGrid::Create, (asITypeInfo*, void*), CScriptGrid*), asCALL_CDECL); assert( r >= 0 );

    // The memory management methods
    r = engine->RegisterObjectBehaviour("grid<T>", asBEHAVE_ADDREF, "void f()", asMETHOD(CScriptGrid,AddRef), asCALL_THISCALL); assert( r >= 0 );
    r = engine->RegisterObjectBehaviour("grid<T>", asBEHAVE_RELEASE, "void f()", asMETHOD(CScriptGrid,Release), asCALL_THISCALL); assert( r >= 0 );

    // The index operator returns the template subtype
    r = engine->RegisterObjectMethod("grid<T>", "T &opIndex(uint, uint)", asMETHODPR(CScriptGrid, At, (asUINT, asUINT), void*), asCALL_THISCALL); assert( r >= 0 );
    r = engine->RegisterObjectMethod("grid<T>", "const T &opIndex(uint, uint) const", asMETHODPR(CScriptGrid, At, (asUINT, asUINT) const, const void*), asCALL_THISCALL); assert( r >= 0 );

    // Other methods
    r = engine->RegisterObjectMethod("grid<T>", "void resize(uint width, uint height)", asMETHODPR(CScriptGrid, Resize, (asUINT, asUINT), void), asCALL_THISCALL); assert( r >= 0 );
    r = engine->RegisterObjectMethod("grid<T>", "uint width() const", asMETHOD(CScriptGrid, GetWidth), asCALL_THISCALL); assert( r >= 0 );
    r = engine->RegisterObjectMethod("grid<T>", "uint height() const", asMETHOD(CScriptGrid, GetHeight), asCALL_THISCALL); assert( r >= 0 );

    // Register GC behaviours in case the array needs to be garbage collected
    r = engine->RegisterObjectBehaviour("grid<T>", asBEHAVE_GETREFCOUNT, "int f()", asMETHOD(CScriptGrid, GetRefCount), asCALL_THISCALL); assert( r >= 0 );
    r = engine->RegisterObjectBehaviour("grid<T>", asBEHAVE_SETGCFLAG, "void f()", asMETHOD(CScriptGrid, SetFlag), asCALL_THISCALL); assert( r >= 0 );
    r = engine->RegisterObjectBehaviour("grid<T>", asBEHAVE_GETGCFLAG, "bool f()", asMETHOD(CScriptGrid, GetFlag), asCALL_THISCALL); assert( r >= 0 );
    r = engine->RegisterObjectBehaviour("grid<T>", asBEHAVE_ENUMREFS, "void f(int&in)", asMETHOD(CScriptGrid, EnumReferences), asCALL_THISCALL); assert( r >= 0 );
    r = engine->RegisterObjectBehaviour("grid<T>", asBEHAVE_RELEASEREFS, "void f(int&in)", asMETHOD(CScriptGrid, ReleaseAllHandles), asCALL_THISCALL); assert( r >= 0 );
}

CScriptGrid::CScriptGrid(asITypeInfo *ti, void *buf)
{
    refCount = 1;
    gcFlag = false;
    objType = ti;
    objType->AddRef();
    buffer = 0;
    subTypeId = objType->GetSubTypeId();

    asIScriptEngine *engine = ti->GetEngine();

    // Determine element size
    if( subTypeId & asTYPEID_MASK_OBJECT )
        elementSize = sizeof(asPWORD);
    else
        elementSize = engine->GetSizeOfPrimitiveType(subTypeId);

    // Determine the initial size from the buffer
    asUINT height = *(asUINT*)buf;
    asUINT width = height ? *(asUINT*)((char*)(buf)+4) : 0;

    // Make sure the grid size isn't too large for us to handle
    if( !CheckMaxSize(width, height) )
    {
        // Don't continue with the initialization
        return;
    }

    // Skip the height value at the start of the buffer
    buf = (asUINT*)(buf)+1;

    // Copy the values of the grid elements from the buffer
    if( (ti->GetSubTypeId() & asTYPEID_MASK_OBJECT) == 0 )
    {
        CreateBuffer(&buffer, width, height);

        // Copy the values of the primitive type into the internal buffer
        for( asUINT y = 0; y < height; y++ )
        {
            // Skip the length value at the start of each row
            buf = (asUINT*)(buf)+1;

            // Copy the line
            if( width > 0 )
                memcpy(At(0,y), buf, width*elementSize);

            // Move to next line
            buf = (char*)(buf) + width*elementSize;

            // Align to 4 byte boundary
            if( asPWORD(buf) & 0x3 )
                buf = (char*)(buf) + 4 - (asPWORD(buf) & 0x3);
        }
    }
    else if( ti->GetSubTypeId() & asTYPEID_OBJHANDLE )
    {
        CreateBuffer(&buffer, width, height);

        // Copy the handles into the internal buffer
        for( asUINT y = 0; y < height; y++ )
        {
            // Skip the length value at the start of each row
            buf = (asUINT*)(buf)+1;

            // Copy the line
            if( width > 0 )
                memcpy(At(0,y), buf, width*elementSize);

            // With object handles it is safe to clear the memory in the received buffer
            // instead of increasing the ref count. It will save time both by avoiding the
            // call the increase ref, and also relieve the engine from having to release
            // its references too
            memset(buf, 0, width*elementSize);

            // Move to next line
            buf = (char*)(buf) + width*elementSize;

            // Align to 4 byte boundary
            if( asPWORD(buf) & 0x3 )
                buf = (char*)(buf) + 4 - (asPWORD(buf) & 0x3);
        }
    }
    else if( ti->GetSubType()->GetFlags() & asOBJ_REF )
    {
        // Only allocate the buffer, but not the objects
        subTypeId |= asTYPEID_OBJHANDLE;
        CreateBuffer(&buffer, width, height);
        subTypeId &= ~asTYPEID_OBJHANDLE;

        // Copy the handles into the internal buffer
        for( asUINT y = 0; y < height; y++ )
        {
            // Skip the length value at the start of each row
            buf = (asUINT*)(buf)+1;

            // Copy the line
            if( width > 0 )
                memcpy(At(0,y), buf, width*elementSize);

            // With object handles it is safe to clear the memory in the received buffer
            // instead of increasing the ref count. It will save time both by avoiding the
            // call the increase ref, and also relieve the engine from having to release
            // its references too
            memset(buf, 0, width*elementSize);

            // Move to next line
            buf = (char*)(buf) + width*elementSize;

            // Align to 4 byte boundary
            if( asPWORD(buf) & 0x3 )
                buf = (char*)(buf) + 4 - (asPWORD(buf) & 0x3);
        }
    }
    else
    {
        // TODO: Optimize by calling the copy constructor of the object instead of
        //       constructing with the default constructor and then assigning the value
        // TODO: With C++11 ideally we should be calling the move constructor, instead
        //       of the copy constructor as the engine will just discard the objects in the
        //       buffer afterwards.
        CreateBuffer(&buffer, width, height);

        // For value types we need to call the opAssign for each individual object
        asITypeInfo *subType = ti->GetSubType();
        asUINT subTypeSize = subType->GetSize();
        for( asUINT y = 0;y < height; y++ )
        {
            // Skip the length value at the start of each row
            buf = (asUINT*)(buf)+1;

            // Call opAssign for each of the objects on the row
            for( asUINT x = 0; x < width; x++ )
            {
                void *obj = At(x,y);
                asBYTE *srcObj = (asBYTE*)(buf) + x*subTypeSize;
                engine->AssignScriptObject(obj, srcObj, subType);
            }

            // Move to next line
            buf = (char*)(buf) + width*subTypeSize;

            // Align to 4 byte boundary
            if( asPWORD(buf) & 0x3 )
                buf = (char*)(buf) + 4 - (asPWORD(buf) & 0x3);
        }
    }

    // Notify the GC of the successful creation
    if( objType->GetFlags() & asOBJ_GC )
        objType->GetEngine()->NotifyGarbageCollectorOfNewObject(this, objType);
}

CScriptGrid::CScriptGrid(asUINT width, asUINT height, asITypeInfo *ti)
{
    refCount = 1;
    gcFlag = false;
    objType = ti;
    objType->AddRef();
    buffer = 0;
    subTypeId = objType->GetSubTypeId();

    // Determine element size
    if( subTypeId & asTYPEID_MASK_OBJECT )
        elementSize = sizeof(asPWORD);
    else
        elementSize = objType->GetEngine()->GetSizeOfPrimitiveType(subTypeId);

    // Make sure the array size isn't too large for us to handle
    if( !CheckMaxSize(width, height) )
    {
        // Don't continue with the initialization
        return;
    }

    CreateBuffer(&buffer, width, height);

    // Notify the GC of the successful creation
    if( objType->GetFlags() & asOBJ_GC )
        objType->GetEngine()->NotifyGarbageCollectorOfNewObject(this, objType);
}

void CScriptGrid::Resize(asUINT width, asUINT height)
{
    // Make sure the size isn't too large for us to handle
    if( !CheckMaxSize(width, height) )
        return;

    // Create a new buffer
    SGridBuffer *tmpBuffer = 0;
    CreateBuffer(&tmpBuffer, width, height);
    if( tmpBuffer == 0 )
        return;

    if( buffer )
    {
        // Copy the existing values to the new buffer
        asUINT w = width > buffer->width ? buffer->width : width;
        asUINT h = height > buffer->height ? buffer->height : height;
        for( asUINT y = 0; y < h; y++ )
            for( asUINT x = 0; x < w; x++ )
                SetValue(tmpBuffer, x, y, At(buffer, x, y));

        // Replace the internal buffer
        DeleteBuffer(buffer);
    }

    buffer = tmpBuffer;
}

CScriptGrid::CScriptGrid(asUINT width, asUINT height, void *defVal, asITypeInfo *ti)
{
    refCount = 1;
    gcFlag = false;
    objType = ti;
    objType->AddRef();
    buffer = 0;
    subTypeId = objType->GetSubTypeId();

    // Determine element size
    if( subTypeId & asTYPEID_MASK_OBJECT )
        elementSize = sizeof(asPWORD);
    else
        elementSize = objType->GetEngine()->GetSizeOfPrimitiveType(subTypeId);

    // Make sure the array size isn't too large for us to handle
    if( !CheckMaxSize(width, height) )
    {
        // Don't continue with the initialization
        return;
    }

    CreateBuffer(&buffer, width, height);

    // Notify the GC of the successful creation
    if( objType->GetFlags() & asOBJ_GC )
        objType->GetEngine()->NotifyGarbageCollectorOfNewObject(this, objType);

    // Initialize the elements with the default value
    for( asUINT y = 0; y < GetHeight(); y++ )
        for( asUINT x = 0; x < GetWidth(); x++ )
            SetValue(x, y, defVal);
}

void CScriptGrid::SetValue(asUINT x, asUINT y, void *value)
{
    SetValue(buffer, x, y, value);
}

void CScriptGrid::SetValue(SGridBuffer *buf, asUINT x, asUINT y, void *value)
{
    // At() will take care of the out-of-bounds checking, though
    // if called from the application then nothing will be done
    void *ptr = At(buf, x, y);
    if( ptr == 0 ) return;

    if( (subTypeId & ~asTYPEID_MASK_SEQNBR) && !(subTypeId & asTYPEID_OBJHANDLE) )
        objType->GetEngine()->AssignScriptObject(ptr, value, objType->GetSubType());
    else if( subTypeId & asTYPEID_OBJHANDLE )
    {
        void *tmp = *(void**)ptr;
        *(void**)ptr = *(void**)value;
        objType->GetEngine()->AddRefScriptObject(*(void**)value, objType->GetSubType());
        if( tmp )
            objType->GetEngine()->ReleaseScriptObject(tmp, objType->GetSubType());
    }
    else if( subTypeId == asTYPEID_BOOL ||
             subTypeId == asTYPEID_INT8 ||
             subTypeId == asTYPEID_UINT8 )
        *(char*)ptr = *(char*)value;
    else if( subTypeId == asTYPEID_INT16 ||
             subTypeId == asTYPEID_UINT16 )
        *(short*)ptr = *(short*)value;
    else if( subTypeId == asTYPEID_INT32 ||
             subTypeId == asTYPEID_UINT32 ||
             subTypeId == asTYPEID_FLOAT ||
             subTypeId > asTYPEID_DOUBLE ) // enums have a type id larger than doubles
        *(int*)ptr = *(int*)value;
    else if( subTypeId == asTYPEID_INT64 ||
             subTypeId == asTYPEID_UINT64 ||
             subTypeId == asTYPEID_DOUBLE )
        *(double*)ptr = *(double*)value;
}

CScriptGrid::~CScriptGrid()
{
    if( buffer )
    {
        DeleteBuffer(buffer);
        buffer = 0;
    }
    if( objType ) objType->Release();
}

asUINT CScriptGrid::GetWidth() const
{
    if( buffer )
        return buffer->width;

    return 0;
}

asUINT CScriptGrid::GetHeight() const
{
    if( buffer )
        return buffer->height;

    return 0;
}

// internal
bool CScriptGrid::CheckMaxSize(asUINT width, asUINT height)
{
    // This code makes sure the size of the buffer that is allocated
    // for the array doesn't overflow and becomes smaller than requested

    asUINT maxSize = 0xFFFFFFFFul - sizeof(SGridBuffer) + 1;
    if( elementSize > 0 )
        maxSize /= elementSize;

    asINT64 numElements  = width * height;

    if( (numElements >> 32) || numElements > maxSize )
    {
        asIScriptContext *ctx = asGetActiveContext();
        if( ctx )
            ctx->SetException("Too large grid size");

        return false;
    }

    // OK
    return true;
}

asITypeInfo *CScriptGrid::GetGridObjectType() const
{
    return objType;
}

int CScriptGrid::GetGridTypeId() const
{
    return objType->GetTypeId();
}

int CScriptGrid::GetElementTypeId() const
{
    return subTypeId;
}

void *CScriptGrid::At(asUINT x, asUINT y)
{
    return At(buffer, x, y);
}

// Return a pointer to the array element. Returns 0 if the index is out of bounds
void *CScriptGrid::At(SGridBuffer *buf, asUINT x, asUINT y)
{
    if( buf == 0 || x >= buf->width || y >= buf->height )
    {
        // If this is called from a script we raise a script exception
        asIScriptContext *ctx = asGetActiveContext();
        if( ctx )
            ctx->SetException("Index out of bounds");
        return 0;
    }

    asUINT index = x+y*buf->width;
    if( (subTypeId & asTYPEID_MASK_OBJECT) && !(subTypeId & asTYPEID_OBJHANDLE) )
        return *(void**)(buf->data + elementSize*index);
    else
        return buf->data + elementSize*index;
}
const void *CScriptGrid::At(asUINT x, asUINT y) const
{
    return const_cast<CScriptGrid*>(this)->At(const_cast<SGridBuffer*>(buffer), x, y);
}


// internal
void CScriptGrid::CreateBuffer(SGridBuffer **buf, asUINT w, asUINT h)
{
    asUINT numElements = w * h;

    *buf = reinterpret_cast<SGridBuffer*>(userAlloc(sizeof(SGridBuffer)-1+elementSize*numElements));

    if( *buf )
    {
        (*buf)->width  = w;
        (*buf)->height = h;
        Construct(*buf);
    }
    else
    {
        // Oops, out of memory
        asIScriptContext *ctx = asGetActiveContext();
        if( ctx )
            ctx->SetException("Out of memory");
    }
}

// internal
void CScriptGrid::DeleteBuffer(SGridBuffer *buf)
{
    assert( buf );

    Destruct(buf);

    // Free the buffer
    userFree(buf);
}

// internal
void CScriptGrid::Construct(SGridBuffer *buf)
{
    assert( buf );

    if( subTypeId & asTYPEID_OBJHANDLE )
    {
        // Set all object handles to null
        void *d = (void*)(buf->data);
        memset(d, 0, (buf->width*buf->height)*sizeof(void*));
    }
    else if( subTypeId & asTYPEID_MASK_OBJECT )
    {
        void **max = (void**)(buf->data + (buf->width*buf->height) * sizeof(void*));
        void **d = (void**)(buf->data);

        asIScriptEngine *engine = objType->GetEngine();
        asITypeInfo *subType = objType->GetSubType();

        for( ; d < max; d++ )
        {
            *d = (void*)engine->CreateScriptObject(subType);
            if( *d == 0 )
            {
                // Set the remaining entries to null so the destructor
                // won't attempt to destroy invalid objects later
                memset(d, 0, sizeof(void*)*(max-d));

                // There is no need to set an exception on the context,
                // as CreateScriptObject has already done that
                return;
            }
        }
    }
}

// internal
void CScriptGrid::Destruct(SGridBuffer *buf)
{
    assert( buf );

    if( subTypeId & asTYPEID_MASK_OBJECT )
    {
        asIScriptEngine *engine = objType->GetEngine();

        void **max = (void**)(buf->data + (buf->width*buf->height) * sizeof(void*));
        void **d   = (void**)(buf->data);

        for( ; d < max; d++ )
        {
            if( *d )
                engine->ReleaseScriptObject(*d, objType->GetSubType());
        }
    }
}

// GC behaviour
void CScriptGrid::EnumReferences(asIScriptEngine *engine)
{
    if( buffer == 0 ) return;

    // If the grid is holding handles, then we need to notify the GC of them
    if (subTypeId & asTYPEID_MASK_OBJECT)
    {
        asUINT numElements = buffer->width * buffer->height;
        void **d = (void**)buffer->data;
        asITypeInfo *subType = engine->GetTypeInfoById(subTypeId);
        if ((subType->GetFlags() & asOBJ_REF))
        {
            // For reference types we need to notify the GC of each instance
            for (asUINT n = 0; n < numElements; n++)
            {
                if (d[n])
                    engine->GCEnumCallback(d[n]);
            }
        }
        else if ((subType->GetFlags() & asOBJ_VALUE) && (subType->GetFlags() & asOBJ_GC))
        {
            // For value types we need to forward the enum callback
            // to the object so it can decide what to do
            for (asUINT n = 0; n < numElements; n++)
            {
                if (d[n])
                    engine->ForwardGCEnumReferences(d[n], subType);
            }
        }
    }
}

// GC behaviour
void CScriptGrid::ReleaseAllHandles(asIScriptEngine*)
{
    if( buffer == 0 ) return;

    DeleteBuffer(buffer);
    buffer = 0;
}

void CScriptGrid::AddRef() const
{
    // Clear the GC flag then increase the counter
    gcFlag = false;
    asAtomicInc(refCount);
}

void CScriptGrid::Release() const
{
    // Clearing the GC flag then descrease the counter
    gcFlag = false;
    if( asAtomicDec(refCount) == 0 )
    {
        // When reaching 0 no more references to this instance
        // exists and the object should be destroyed
        this->~CScriptGrid();
        userFree(const_cast<CScriptGrid*>(this));
    }
}

// GC behaviour
int CScriptGrid::GetRefCount()
{
    return refCount;
}

// GC behaviour
void CScriptGrid::SetFlag()
{
    gcFlag = true;
}

// GC behaviour
bool CScriptGrid::GetFlag()
{
    return gcFlag;
}

END_AS_NAMESPACE
