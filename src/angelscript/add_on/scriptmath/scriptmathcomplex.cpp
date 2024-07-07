#include <assert.h>
#include <string.h> // strstr
#include <new> // new()
#include <math.h>
#include "scriptmathcomplex.h"

#ifdef __BORLANDC__
// C++Builder doesn't define a non-standard "sqrtf" function but rather an overload of "sqrt"
// for float arguments.
inline float sqrtf (float x) { return sqrt (x); }
#endif

BEGIN_AS_NAMESPACE

Complex::Complex()
{
    r = 0;
    i = 0;
}

Complex::Complex(const Complex &other)
{
    r = other.r;
    i = other.i;
}

Complex::Complex(float _r, float _i)
{
    r = _r;
    i = _i;
}

bool Complex::operator==(const Complex &o) const
{
    return (r == o.r) && (i == o.i);
}

bool Complex::operator!=(const Complex &o) const
{
    return !(*this == o);
}

Complex &Complex::operator=(const Complex &other)
{
    r = other.r;
    i = other.i;
    return *this;
}

Complex &Complex::operator+=(const Complex &other)
{
    r += other.r;
    i += other.i;
    return *this;
}

Complex &Complex::operator-=(const Complex &other)
{
    r -= other.r;
    i -= other.i;
    return *this;
}

Complex &Complex::operator*=(const Complex &other)
{
    *this = *this * other;
    return *this;
}

Complex &Complex::operator/=(const Complex &other)
{
    *this = *this / other;
    return *this;
}

float Complex::squaredLength() const
{
    return r*r + i*i;
}

float Complex::length() const
{
    return sqrtf(squaredLength());
}

Complex Complex::operator+(const Complex &other) const
{
    return Complex(r + other.r, i + other.i);
}

Complex Complex::operator-(const Complex &other) const
{
    return Complex(r - other.r, i + other.i);
}

Complex Complex::operator*(const Complex &other) const
{
    return Complex(r*other.r - i*other.i, r*other.i + i*other.r);
}

Complex Complex::operator/(const Complex &other) const
{
    float squaredLen = other.squaredLength();
    if( squaredLen == 0 ) return Complex(0,0);

    return Complex((r*other.r + i*other.i)/squaredLen, (i*other.r - r*other.i)/squaredLen);
}

//-----------------------
// Swizzle operators
//-----------------------

Complex Complex::get_ri() const
{
    return *this;
}
Complex Complex::get_ir() const
{
    return Complex(r,i);
}
void Complex::set_ri(const Complex &o)
{
    *this = o;
}
void Complex::set_ir(const Complex &o)
{
    r = o.i;
    i = o.r;
}

//-----------------------
// AngelScript functions
//-----------------------

static void ComplexDefaultConstructor(Complex *self)
{
    new(self) Complex();
}

static void ComplexCopyConstructor(const Complex &other, Complex *self)
{
    new(self) Complex(other);
}

static void ComplexConvConstructor(float r, Complex *self)
{
    new(self) Complex(r);
}

static void ComplexInitConstructor(float r, float i, Complex *self)
{
    new(self) Complex(r,i);
}

static void ComplexListConstructor(float *list, Complex *self)
{
    new(self) Complex(list[0], list[1]);
}

//--------------------------------
// Registration
//-------------------------------------

static void RegisterScriptMathComplex_Native(asIScriptEngine *engine)
{
    int r;

    // Register the type
#if AS_CAN_USE_CPP11
    // With C++11 it is possible to use asGetTypeTraits to determine the correct flags to represent the C++ class, except for the asOBJ_APP_CLASS_ALLFLOATS
    r = engine->RegisterObjectType("complex", sizeof(Complex), asOBJ_VALUE | asOBJ_POD | asGetTypeTraits<Complex>() | asOBJ_APP_CLASS_ALLFLOATS); assert( r >= 0 );
#else
    r = engine->RegisterObjectType("complex", sizeof(Complex), asOBJ_VALUE | asOBJ_POD | asOBJ_APP_CLASS_CAK | asOBJ_APP_CLASS_ALLFLOATS); assert( r >= 0 );
#endif

    // Register the object properties
    r = engine->RegisterObjectProperty("complex", "float r", asOFFSET(Complex, r)); assert( r >= 0 );
    r = engine->RegisterObjectProperty("complex", "float i", asOFFSET(Complex, i)); assert( r >= 0 );

    // Register the constructors
    r = engine->RegisterObjectBehaviour("complex", asBEHAVE_CONSTRUCT,      "void f()",                             asFUNCTION(ComplexDefaultConstructor), asCALL_CDECL_OBJLAST); assert( r >= 0 );
    r = engine->RegisterObjectBehaviour("complex", asBEHAVE_CONSTRUCT,      "void f(const complex &in)",            asFUNCTION(ComplexCopyConstructor), asCALL_CDECL_OBJLAST); assert( r >= 0 );
    r = engine->RegisterObjectBehaviour("complex", asBEHAVE_CONSTRUCT,      "void f(float)",                        asFUNCTION(ComplexConvConstructor), asCALL_CDECL_OBJLAST); assert( r >= 0 );
    r = engine->RegisterObjectBehaviour("complex", asBEHAVE_CONSTRUCT,      "void f(float, float)",                 asFUNCTION(ComplexInitConstructor), asCALL_CDECL_OBJLAST); assert( r >= 0 );
    r = engine->RegisterObjectBehaviour("complex", asBEHAVE_LIST_CONSTRUCT, "void f(const int &in) {float, float}", asFUNCTION(ComplexListConstructor), asCALL_CDECL_OBJLAST); assert( r >= 0 );

    // Register the operator overloads
    r = engine->RegisterObjectMethod("complex", "complex &opAddAssign(const complex &in)", asMETHODPR(Complex, operator+=, (const Complex &), Complex&), asCALL_THISCALL); assert( r >= 0 );
    r = engine->RegisterObjectMethod("complex", "complex &opSubAssign(const complex &in)", asMETHODPR(Complex, operator-=, (const Complex &), Complex&), asCALL_THISCALL); assert( r >= 0 );
    r = engine->RegisterObjectMethod("complex", "complex &opMulAssign(const complex &in)", asMETHODPR(Complex, operator*=, (const Complex &), Complex&), asCALL_THISCALL); assert( r >= 0 );
    r = engine->RegisterObjectMethod("complex", "complex &opDivAssign(const complex &in)", asMETHODPR(Complex, operator/=, (const Complex &), Complex&), asCALL_THISCALL); assert( r >= 0 );
    r = engine->RegisterObjectMethod("complex", "bool opEquals(const complex &in) const", asMETHODPR(Complex, operator==, (const Complex &) const, bool), asCALL_THISCALL); assert( r >= 0 );
    r = engine->RegisterObjectMethod("complex", "complex opAdd(const complex &in) const", asMETHODPR(Complex, operator+, (const Complex &) const, Complex), asCALL_THISCALL); assert( r >= 0 );
    r = engine->RegisterObjectMethod("complex", "complex opSub(const complex &in) const", asMETHODPR(Complex, operator-, (const Complex &) const, Complex), asCALL_THISCALL); assert( r >= 0 );
    r = engine->RegisterObjectMethod("complex", "complex opMul(const complex &in) const", asMETHODPR(Complex, operator*, (const Complex &) const, Complex), asCALL_THISCALL); assert( r >= 0 );
    r = engine->RegisterObjectMethod("complex", "complex opDiv(const complex &in) const", asMETHODPR(Complex, operator/, (const Complex &) const, Complex), asCALL_THISCALL); assert( r >= 0 );

    // Register the object methods
    r = engine->RegisterObjectMethod("complex", "float abs() const", asMETHOD(Complex,length), asCALL_THISCALL); assert( r >= 0 );

    // Register the swizzle operators
    r = engine->RegisterObjectMethod("complex", "complex get_ri() const property", asMETHOD(Complex, get_ri), asCALL_THISCALL); assert( r >= 0 );
    r = engine->RegisterObjectMethod("complex", "complex get_ir() const property", asMETHOD(Complex, get_ir), asCALL_THISCALL); assert( r >= 0 );
    r = engine->RegisterObjectMethod("complex", "void set_ri(const complex &in) property", asMETHOD(Complex, set_ri), asCALL_THISCALL); assert( r >= 0 );
    r = engine->RegisterObjectMethod("complex", "void set_ir(const complex &in) property", asMETHOD(Complex, set_ir), asCALL_THISCALL); assert( r >= 0 );
}

void RegisterScriptMathComplex(asIScriptEngine *engine)
{
    if( strstr(asGetLibraryOptions(), "AS_MAX_PORTABILITY") )
    {
        assert( false );
        // TODO: implement support for generic calling convention
        // RegisterScriptMathComplex_Generic(engine);
    }
    else
        RegisterScriptMathComplex_Native(engine);
}

END_AS_NAMESPACE


