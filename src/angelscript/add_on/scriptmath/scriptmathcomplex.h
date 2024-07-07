#ifndef SCRIPTMATHCOMPLEX_H
#define SCRIPTMATHCOMPLEX_H

#ifndef ANGELSCRIPT_H
// Avoid having to inform include path if header is already include before
#include <angelscript.h>
#endif


BEGIN_AS_NAMESPACE

// This class implements complex numbers and the common
// operations that can be done with it.
//
// Ref: http://mathworld.wolfram.com/ComplexNumber.html

struct Complex
{
    Complex();
    Complex(const Complex &other);
    Complex(float r, float i = 0);

    // Assignment operator
    Complex &operator=(const Complex &other);

    // Compound assigment operators
    Complex &operator+=(const Complex &other);
    Complex &operator-=(const Complex &other);
    Complex &operator*=(const Complex &other);
    Complex &operator/=(const Complex &other);

    float length() const;
    float squaredLength() const;

    // Swizzle operators
    Complex get_ri() const;
    void    set_ri(const Complex &in);
    Complex get_ir() const;
    void    set_ir(const Complex &in);

    // Comparison
    bool operator==(const Complex &other) const;
    bool operator!=(const Complex &other) const;

    // Math operators
    Complex operator+(const Complex &other) const;
    Complex operator-(const Complex &other) const;
    Complex operator*(const Complex &other) const;
    Complex operator/(const Complex &other) const;

    float r;
    float i;
};

// This function will determine the configuration of the engine
// and use one of the two functions below to register the string type
void RegisterScriptMathComplex(asIScriptEngine *engine);

END_AS_NAMESPACE

#endif
