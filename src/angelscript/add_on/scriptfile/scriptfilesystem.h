#ifndef AS_SCRIPTFILESYSTEM_H
#define AS_SCRIPTFILESYSTEM_H

#ifndef ANGELSCRIPT_H
// Avoid having to inform include path if header is already include before
#include <angelscript.h>
#endif

#include <string>
#include <stdio.h>

#include "../scriptarray/scriptarray.h"
#include "../datetime/datetime.h"

BEGIN_AS_NAMESPACE

class CScriptFileSystem
{
public:
    CScriptFileSystem();

    void AddRef() const;
    void Release() const;

    // Sets the current path that should be used in other calls when using relative paths
    // It can use relative paths too, so moving up a directory is used by passing in ".."
    bool ChangeCurrentPath(const std::string &path);
    std::string GetCurrentPath() const;

    // Returns true if the path is a directory. Input can be either a full path or a relative path.
    // This method does not return the dirs '.' and '..'
    bool IsDir(const std::string &path) const;

    // Returns true if the path is a link. Input can be either a full path or a relative path
    bool IsLink(const std::string &path) const;

    // Returns the size of file. Input can be either a full path or a relative path
    asINT64 GetSize(const std::string &path) const;

    // Returns a list of the files in the current path
    CScriptArray *GetFiles() const;

    // Returns a list of the directories in the current path
    CScriptArray *GetDirs() const;

    // Creates a new directory. Returns 0 on success
    int MakeDir(const std::string &path);

    // Removes a directory. Will only remove the directory if it is empty. Returns 0 on success
    int RemoveDir(const std::string &path);

    // Deletes a file. Returns 0 on success
    int DeleteFile(const std::string &path);

    // Copies a file. Returns 0 on success
    int CopyFile(const std::string &source, const std::string &target);

    // Moves or renames a file or directory. Returns 0 on success
    int Move(const std::string &source, const std::string &target);

    // Gets the date and time of the file/dir creation
    CDateTime GetCreateDateTime(const std::string &path) const;

    // Gets the date and time of the file/dir modification
    CDateTime GetModifyDateTime(const std::string &path) const;

protected:
    ~CScriptFileSystem();

    mutable int refCount;
    std::string currentPath;
};

void RegisterScriptFileSystem(asIScriptEngine *engine);

END_AS_NAMESPACE

#endif
