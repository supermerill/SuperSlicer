#include <assert.h>
#include <math.h>
#include <float.h>
#include <string.h>
#include "scriptmath.h"

#ifdef __BORLANDC__
#include <cmath>

// The C++Builder RTL doesn't pull the *f functions into the global namespace per default.
using namespace std;

#if __BORLANDC__ < 0x580
// C++Builder 6 and earlier don't come with any *f variants of the math functions at all.
inline float cosf (float arg) { return std::cos (arg); }
inline float sinf (float arg) { return std::sin (arg); }
inline float tanf (float arg) { return std::tan (arg); }
inline float atan2f (float y, float x) { return std::atan2 (y, x); }
inline float logf (float arg) { return std::log (arg); }
inline float powf (float x, float y) { return std::pow (x, y); }
inline float sqrtf (float arg) { return std::sqrt (arg); }
#endif

// C++Builder doesn't define most of the non-standard float-specific math functions with
// "*f" suffix; instead it provides overloads for the standard math functions which take
// "float" arguments.
inline float acosf (float arg) { return std::acos (arg); }
inline float asinf (float arg) { return std::asin (arg); }
inline float atanf (float arg) { return std::atan (arg); }
inline float coshf (float arg) { return std::cosh (arg); }
inline float sinhf (float arg) { return std::sinh (arg); }
inline float tanhf (float arg) { return std::tanh (arg); }
inline float log10f (float arg) { return std::log10 (arg); }
inline float ceilf (float arg) { return std::ceil (arg); }
inline float fabsf (float arg) { return std::fabs (arg); }
inline float floorf (float arg) { return std::floor (arg); }

// C++Builder doesn't define a non-standard "modff" function but rather an overload of "modf"
// for float arguments. However, BCC's float overload of fmod() is broken (QC #74816; fixed
// in C++Builder 2010).
inline float modff (float x, float *y)
{
	double d;
	float f = (float) modf((double) x, &d);
	*y = (float) d;
	return f;
}
#endif

BEGIN_AS_NAMESPACE

// Determine whether the float version should be registered, or the double version
#ifndef AS_USE_FLOAT
#if !defined(_WIN32_WCE) // WinCE doesn't have the float versions of the math functions
#define AS_USE_FLOAT 1
#endif
#endif

// The modf function doesn't seem very intuitive, so I'm writing this 
// function that simply returns the fractional part of the float value
#if AS_USE_FLOAT
float fractionf(float v)
{
	float intPart;
	return modff(v, &intPart);
}
#else
double fraction(double v)
{
	double intPart;
	return modf(v, &intPart);
}
#endif

// As AngelScript doesn't allow bitwise manipulation of float types we'll provide a couple of
// functions for converting float values to IEEE 754 formatted values etc. This also allow us to 
// provide a platform agnostic representation to the script so the scripts don't have to worry
// about whether the CPU uses IEEE 754 floats or some other representation
float fpFromIEEE(asUINT raw)
{
	// TODO: Identify CPU family to provide proper conversion
	//        if the CPU doesn't natively use IEEE style floats
	return *reinterpret_cast<float*>(&raw);
}
asUINT fpToIEEE(float fp)
{
	return *reinterpret_cast<asUINT*>(&fp);
}
double fpFromIEEE(asQWORD raw)
{
	return *reinterpret_cast<double*>(&raw);
}
asQWORD fpToIEEE(double fp)
{
	return *reinterpret_cast<asQWORD*>(&fp);
}

// closeTo() is used to determine if the binary representation of two numbers are 
// relatively close to each other. Numerical errors due to rounding errors build
// up over many operations, so it is almost impossible to get exact numbers and
// this is where closeTo() comes in.
//
// It shouldn't be used to determine if two numbers are mathematically close to 
// each other.
//
// ref: http://www.cygnus-software.com/papers/comparingfloats/comparingfloats.htm
// ref: http://www.gamedev.net/topic/653449-scriptmath-and-closeto/
bool closeTo(float a, float b, float epsilon)
{
	// Equal numbers and infinity will return immediately
	if( a == b ) return true;

	// When very close to 0, we can use the absolute comparison
	float diff = fabsf(a - b);
	if( (a == 0 || b == 0) && (diff < epsilon) )
		return true;
	
	// Otherwise we need to use relative comparison to account for precision
	return diff / (fabs(a) + fabs(b)) < epsilon;
}

bool closeTo(double a, double b, double epsilon)
{
	if( a == b ) return true;

	double diff = fabs(a - b);
	if( (a == 0 || b == 0) && (diff < epsilon) )
		return true;
	
	return diff / (fabs(a) + fabs(b)) < epsilon;
}

void RegisterScriptMath_Native(asIScriptEngine *engine)
{

	// Conversion between floating point and IEEE bits representations
	engine->RegisterGlobalFunction("float fpFromIEEE(uint)", asFUNCTIONPR(fpFromIEEE, (asUINT), float), asCALL_CDECL); assert( r >= 0 );
	engine->RegisterGlobalFunction("uint fpToIEEE(float)", asFUNCTIONPR(fpToIEEE, (float), asUINT), asCALL_CDECL); assert( r >= 0 );
	engine->RegisterGlobalFunction("double fpFromIEEE(uint64)", asFUNCTIONPR(fpFromIEEE, (asQWORD), double), asCALL_CDECL); assert( r >= 0 );
	engine->RegisterGlobalFunction("uint64 fpToIEEE(double)", asFUNCTIONPR(fpToIEEE, (double), asQWORD), asCALL_CDECL); assert( r >= 0 );

	// Close to comparison with epsilon 
	engine->RegisterGlobalFunction("bool closeTo(float, float, float = 0.00001f)", asFUNCTIONPR(closeTo, (float, float, float), bool), asCALL_CDECL); assert( r >= 0 );
	engine->RegisterGlobalFunction("bool closeTo(double, double, double = 0.0000000001)", asFUNCTIONPR(closeTo, (double, double, double), bool), asCALL_CDECL); assert( r >= 0 );

#if AS_USE_FLOAT
	// Trigonometric functions
	engine->RegisterGlobalFunction("float cos(float)", asFUNCTIONPR(cosf, (float), float), asCALL_CDECL); assert( r >= 0 );
	engine->RegisterGlobalFunction("float sin(float)", asFUNCTIONPR(sinf, (float), float), asCALL_CDECL); assert( r >= 0 );
	engine->RegisterGlobalFunction("float tan(float)", asFUNCTIONPR(tanf, (float), float), asCALL_CDECL); assert( r >= 0 );

	engine->RegisterGlobalFunction("float acos(float)", asFUNCTIONPR(acosf, (float), float), asCALL_CDECL); assert( r >= 0 );
	engine->RegisterGlobalFunction("float asin(float)", asFUNCTIONPR(asinf, (float), float), asCALL_CDECL); assert( r >= 0 );
	engine->RegisterGlobalFunction("float atan(float)", asFUNCTIONPR(atanf, (float), float), asCALL_CDECL); assert( r >= 0 );
	engine->RegisterGlobalFunction("float atan2(float,float)", asFUNCTIONPR(atan2f, (float, float), float), asCALL_CDECL); assert( r >= 0 );

	// Hyberbolic functions
	engine->RegisterGlobalFunction("float cosh(float)", asFUNCTIONPR(coshf, (float), float), asCALL_CDECL); assert( r >= 0 );
	engine->RegisterGlobalFunction("float sinh(float)", asFUNCTIONPR(sinhf, (float), float), asCALL_CDECL); assert( r >= 0 );
	engine->RegisterGlobalFunction("float tanh(float)", asFUNCTIONPR(tanhf, (float), float), asCALL_CDECL); assert( r >= 0 );

	// Exponential and logarithmic functions
	engine->RegisterGlobalFunction("float log(float)", asFUNCTIONPR(logf, (float), float), asCALL_CDECL); assert( r >= 0 );
	engine->RegisterGlobalFunction("float log10(float)", asFUNCTIONPR(log10f, (float), float), asCALL_CDECL); assert( r >= 0 );

	// Power functions
	engine->RegisterGlobalFunction("float pow(float, float)", asFUNCTIONPR(powf, (float, float), float), asCALL_CDECL); assert( r >= 0 );
	engine->RegisterGlobalFunction("float sqrt(float)", asFUNCTIONPR(sqrtf, (float), float), asCALL_CDECL); assert( r >= 0 );

	// Nearest integer, absolute value, and remainder functions
	engine->RegisterGlobalFunction("float ceil(float)", asFUNCTIONPR(ceilf, (float), float), asCALL_CDECL); assert( r >= 0 );
	engine->RegisterGlobalFunction("float abs(float)", asFUNCTIONPR(fabsf, (float), float), asCALL_CDECL); assert( r >= 0 );
	engine->RegisterGlobalFunction("float floor(float)", asFUNCTIONPR(floorf, (float), float), asCALL_CDECL); assert( r >= 0 );
	engine->RegisterGlobalFunction("float fraction(float)", asFUNCTIONPR(fractionf, (float), float), asCALL_CDECL); assert( r >= 0 );

	// Don't register modf because AngelScript already supports the % operator
#else
	// double versions of the same
	engine->RegisterGlobalFunction("double cos(double)", asFUNCTIONPR(cos, (double), double), asCALL_CDECL); assert( r >= 0 );
	engine->RegisterGlobalFunction("double sin(double)", asFUNCTIONPR(sin, (double), double), asCALL_CDECL); assert( r >= 0 );
	engine->RegisterGlobalFunction("double tan(double)", asFUNCTIONPR(tan, (double), double), asCALL_CDECL); assert( r >= 0 );
	engine->RegisterGlobalFunction("double acos(double)", asFUNCTIONPR(acos, (double), double), asCALL_CDECL); assert( r >= 0 );
	engine->RegisterGlobalFunction("double asin(double)", asFUNCTIONPR(asin, (double), double), asCALL_CDECL); assert( r >= 0 );
	engine->RegisterGlobalFunction("double atan(double)", asFUNCTIONPR(atan, (double), double), asCALL_CDECL); assert( r >= 0 );
	engine->RegisterGlobalFunction("double atan2(double,double)", asFUNCTIONPR(atan2, (double, double), double), asCALL_CDECL); assert( r >= 0 );
	engine->RegisterGlobalFunction("double cosh(double)", asFUNCTIONPR(cosh, (double), double), asCALL_CDECL); assert( r >= 0 );
	engine->RegisterGlobalFunction("double sinh(double)", asFUNCTIONPR(sinh, (double), double), asCALL_CDECL); assert( r >= 0 );
	engine->RegisterGlobalFunction("double tanh(double)", asFUNCTIONPR(tanh, (double), double), asCALL_CDECL); assert( r >= 0 );
	engine->RegisterGlobalFunction("double log(double)", asFUNCTIONPR(log, (double), double), asCALL_CDECL); assert( r >= 0 );
	engine->RegisterGlobalFunction("double log10(double)", asFUNCTIONPR(log10, (double), double), asCALL_CDECL); assert( r >= 0 );
	engine->RegisterGlobalFunction("double pow(double, double)", asFUNCTIONPR(pow, (double, double), double), asCALL_CDECL); assert( r >= 0 );
	engine->RegisterGlobalFunction("double sqrt(double)", asFUNCTIONPR(sqrt, (double), double), asCALL_CDECL); assert( r >= 0 );
	engine->RegisterGlobalFunction("double ceil(double)", asFUNCTIONPR(ceil, (double), double), asCALL_CDECL); assert( r >= 0 );
	engine->RegisterGlobalFunction("double abs(double)", asFUNCTIONPR(fabs, (double), double), asCALL_CDECL); assert( r >= 0 );
	engine->RegisterGlobalFunction("double floor(double)", asFUNCTIONPR(floor, (double), double), asCALL_CDECL); assert( r >= 0 );
	engine->RegisterGlobalFunction("double fraction(double)", asFUNCTIONPR(fraction, (double), double), asCALL_CDECL); assert( r >= 0 );
#endif
}

#if AS_USE_FLOAT
// This macro creates simple generic wrappers for functions of type 'float func(float)'
#define GENERICff(x) \
void x##_generic(asIScriptGeneric *gen) \
{ \
	float f = *(float*)gen->GetAddressOfArg(0); \
	*(float*)gen->GetAddressOfReturnLocation() = x(f); \
}

GENERICff(cosf)
GENERICff(sinf)
GENERICff(tanf)
GENERICff(acosf)
GENERICff(asinf)
GENERICff(atanf)
GENERICff(coshf)
GENERICff(sinhf)
GENERICff(tanhf)
GENERICff(logf)
GENERICff(log10f)
GENERICff(sqrtf)
GENERICff(ceilf)
GENERICff(fabsf)
GENERICff(floorf)
GENERICff(fractionf)

void powf_generic(asIScriptGeneric *gen)
{
	float f1 = *(float*)gen->GetAddressOfArg(0);
	float f2 = *(float*)gen->GetAddressOfArg(1);
	*(float*)gen->GetAddressOfReturnLocation() = powf(f1, f2);
}
void atan2f_generic(asIScriptGeneric *gen)
{
	float f1 = *(float*)gen->GetAddressOfArg(0);
	float f2 = *(float*)gen->GetAddressOfArg(1);
	*(float*)gen->GetAddressOfReturnLocation() = atan2f(f1, f2);
}

#else
// This macro creates simple generic wrappers for functions of type 'double func(double)'
#define GENERICdd(x) \
void x##_generic(asIScriptGeneric *gen) \
{ \
	double f = *(double*)gen->GetAddressOfArg(0); \
	*(double*)gen->GetAddressOfReturnLocation() = x(f); \
}

GENERICdd(cos)
GENERICdd(sin)
GENERICdd(tan)
GENERICdd(acos)
GENERICdd(asin)
GENERICdd(atan)
GENERICdd(cosh)
GENERICdd(sinh)
GENERICdd(tanh)
GENERICdd(log)
GENERICdd(log10)
GENERICdd(sqrt)
GENERICdd(ceil)
GENERICdd(fabs)
GENERICdd(floor)
GENERICdd(fraction)

void pow_generic(asIScriptGeneric *gen)
{
	double f1 = *(double*)gen->GetAddressOfArg(0);
	double f2 = *(double*)gen->GetAddressOfArg(1);
	*(double*)gen->GetAddressOfReturnLocation() = pow(f1, f2);
}
void atan2_generic(asIScriptGeneric *gen)
{
	double f1 = *(double*)gen->GetAddressOfArg(0);
	double f2 = *(double*)gen->GetAddressOfArg(1);
	*(double*)gen->GetAddressOfReturnLocation() = atan2(f1, f2);
}
#endif
void RegisterScriptMath_Generic(asIScriptEngine *engine)
{

#if AS_USE_FLOAT
	// Trigonometric functions
	engine->RegisterGlobalFunction("float cos(float)", asFUNCTION(cosf_generic), asCALL_GENERIC); assert( r >= 0 );
	engine->RegisterGlobalFunction("float sin(float)", asFUNCTION(sinf_generic), asCALL_GENERIC); assert( r >= 0 );
	engine->RegisterGlobalFunction("float tan(float)", asFUNCTION(tanf_generic), asCALL_GENERIC); assert( r >= 0 );

	engine->RegisterGlobalFunction("float acos(float)", asFUNCTION(acosf_generic), asCALL_GENERIC); assert( r >= 0 );
	engine->RegisterGlobalFunction("float asin(float)", asFUNCTION(asinf_generic), asCALL_GENERIC); assert( r >= 0 );
	engine->RegisterGlobalFunction("float atan(float)", asFUNCTION(atanf_generic), asCALL_GENERIC); assert( r >= 0 );
	engine->RegisterGlobalFunction("float atan2(float,float)", asFUNCTION(atan2f_generic), asCALL_GENERIC); assert( r >= 0 );

	// Hyberbolic functions
	engine->RegisterGlobalFunction("float cosh(float)", asFUNCTION(coshf_generic), asCALL_GENERIC); assert( r >= 0 );
	engine->RegisterGlobalFunction("float sinh(float)", asFUNCTION(sinhf_generic), asCALL_GENERIC); assert( r >= 0 );
	engine->RegisterGlobalFunction("float tanh(float)", asFUNCTION(tanhf_generic), asCALL_GENERIC); assert( r >= 0 );

	// Exponential and logarithmic functions
	engine->RegisterGlobalFunction("float log(float)", asFUNCTION(logf_generic), asCALL_GENERIC); assert( r >= 0 );
	engine->RegisterGlobalFunction("float log10(float)", asFUNCTION(log10f_generic), asCALL_GENERIC); assert( r >= 0 );

	// Power functions
	engine->RegisterGlobalFunction("float pow(float, float)", asFUNCTION(powf_generic), asCALL_GENERIC); assert( r >= 0 );
	engine->RegisterGlobalFunction("float sqrt(float)", asFUNCTION(sqrtf_generic), asCALL_GENERIC); assert( r >= 0 );

	// Nearest integer, absolute value, and remainder functions
	engine->RegisterGlobalFunction("float ceil(float)", asFUNCTION(ceilf_generic), asCALL_GENERIC); assert( r >= 0 );
	engine->RegisterGlobalFunction("float abs(float)", asFUNCTION(fabsf_generic), asCALL_GENERIC); assert( r >= 0 );
	engine->RegisterGlobalFunction("float floor(float)", asFUNCTION(floorf_generic), asCALL_GENERIC); assert( r >= 0 );
	engine->RegisterGlobalFunction("float fraction(float)", asFUNCTION(fractionf_generic), asCALL_GENERIC); assert( r >= 0 );

	// Don't register modf because AngelScript already supports the % operator
#else
	// double versions of the same
	engine->RegisterGlobalFunction("double cos(double)", asFUNCTION(cos_generic), asCALL_GENERIC); assert( r >= 0 );
	engine->RegisterGlobalFunction("double sin(double)", asFUNCTION(sin_generic), asCALL_GENERIC); assert( r >= 0 );
	engine->RegisterGlobalFunction("double tan(double)", asFUNCTION(tan_generic), asCALL_GENERIC); assert( r >= 0 );
	engine->RegisterGlobalFunction("double acos(double)", asFUNCTION(acos_generic), asCALL_GENERIC); assert( r >= 0 );
	engine->RegisterGlobalFunction("double asin(double)", asFUNCTION(asin_generic), asCALL_GENERIC); assert( r >= 0 );
	engine->RegisterGlobalFunction("double atan(double)", asFUNCTION(atan_generic), asCALL_GENERIC); assert( r >= 0 );
	engine->RegisterGlobalFunction("double atan2(double,double)", asFUNCTION(atan2_generic), asCALL_GENERIC); assert( r >= 0 );
	engine->RegisterGlobalFunction("double cosh(double)", asFUNCTION(cosh_generic), asCALL_GENERIC); assert( r >= 0 );
	engine->RegisterGlobalFunction("double sinh(double)", asFUNCTION(sinh_generic), asCALL_GENERIC); assert( r >= 0 );
	engine->RegisterGlobalFunction("double tanh(double)", asFUNCTION(tanh_generic), asCALL_GENERIC); assert( r >= 0 );
	engine->RegisterGlobalFunction("double log(double)", asFUNCTION(log_generic), asCALL_GENERIC); assert( r >= 0 );
	engine->RegisterGlobalFunction("double log10(double)", asFUNCTION(log10_generic), asCALL_GENERIC); assert( r >= 0 );
	engine->RegisterGlobalFunction("double pow(double, double)", asFUNCTION(pow_generic), asCALL_GENERIC); assert( r >= 0 );
	engine->RegisterGlobalFunction("double sqrt(double)", asFUNCTION(sqrt_generic), asCALL_GENERIC); assert( r >= 0 );
	engine->RegisterGlobalFunction("double ceil(double)", asFUNCTION(ceil_generic), asCALL_GENERIC); assert( r >= 0 );
	engine->RegisterGlobalFunction("double abs(double)", asFUNCTION(fabs_generic), asCALL_GENERIC); assert( r >= 0 );
	engine->RegisterGlobalFunction("double floor(double)", asFUNCTION(floor_generic), asCALL_GENERIC); assert( r >= 0 );
	engine->RegisterGlobalFunction("double fraction(double)", asFUNCTION(fraction_generic), asCALL_GENERIC); assert( r >= 0 );
#endif
}

void RegisterScriptMath(asIScriptEngine *engine)
{
	if( strstr(asGetLibraryOptions(), "AS_MAX_PORTABILITY") )
		RegisterScriptMath_Generic(engine);
	else
		RegisterScriptMath_Native(engine);
}

END_AS_NAMESPACE


