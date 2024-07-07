//
// CScriptFile
//
// This class encapsulates a FILE pointer in a reference counted class for
// use within AngelScript.
//

#ifndef SCRIPTFILE_H
#define SCRIPTFILE_H

//---------------------------
// Compilation settings
//

// Set this flag to turn on/off write support
//  0 = off
//  1 = on

#ifndef AS_WRITE_OPS
#define AS_WRITE_OPS 1
#endif




//---------------------------
// Declaration
//

#ifndef ANGELSCRIPT_H
// Avoid having to inform include path if header is already include before
#include <angelscript.h>
#endif

#include <string>
#include <stdio.h>

BEGIN_AS_NAMESPACE

class CScriptFile
{
public:
    CScriptFile();

    void AddRef() const;
    void Release() const;

    // TODO: Implement the "r+", "w+" and "a+" modes
    // mode = "r" -> open the file for reading
    //        "w" -> open the file for writing (overwrites existing file)
    //        "a" -> open the file for appending
    int  Open(const std::string &filename, const std::string &mode);
    int  Close();
    int  GetSize() const;
    bool IsEOF() const;

    // Reading
    std::string ReadString(unsigned int length);
    std::string ReadLine();
    asINT64     ReadInt(asUINT bytes);
    asQWORD     ReadUInt(asUINT bytes);
    float       ReadFloat();
    double      ReadDouble();

    // Writing
    int WriteString(const std::string &str);
    int WriteInt(asINT64 v, asUINT bytes);
    int WriteUInt(asQWORD v, asUINT bytes);
    int WriteFloat(float v);
    int WriteDouble(double v);

    // Cursor
    int GetPos() const;
    int SetPos(int pos);
    int MovePos(int delta);

    // Big-endian = most significant byte first
    bool mostSignificantByteFirst;

protected:
    ~CScriptFile();

    mutable int refCount;
    FILE       *file;
};

// This function will determine the configuration of the engine
// and use one of the two functions below to register the file type
void RegisterScriptFile(asIScriptEngine *engine);

// Call this function to register the file type
// using native calling conventions
void RegisterScriptFile_Native(asIScriptEngine *engine);

// Use this one instead if native calling conventions
// are not supported on the target platform
void RegisterScriptFile_Generic(asIScriptEngine *engine);

END_AS_NAMESPACE

#endif
