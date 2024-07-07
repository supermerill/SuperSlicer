//
// This generator creates a header file that implements automatic
// wrapper functions for the generic calling convention.
//
// Originally implemented by George Yohng from 4Front Technologies in 2009-03-11
// Modifications by Pierre Fortin in order to add constructor wrapper generation
//
// A completely new implementation of automatic wrapper functions was
// implemented by SiCrane at GameDev.net in 2011-12-18. The generator was
// adapted from Python to C++ by Andreas.
//
// ref: http://www.gamedev.net/topic/617111-more-angelscript-binding-wrappers/
//


#include <stdio.h>
#include <string>

// Generate templates for up to this number of function parameters
const int max_args = 4;

using namespace std;

void PrintTemplate(const char *base, const char *typeNameList, const char *retType, const char *objType, const char *isConst, const char *newExpr, const char *objExpr, const char *argList1, const char *argList2, const char *wrapName);
void PrintConstructor(const char *comma, const char *typeNameList, const char *typeList, const char *argList);

int main()
{
    printf("#ifndef AS_GEN_WRAPPER_H\n"
    "#define AS_GEN_WRAPPER_H\n"
    "\n"
    "#ifndef ANGELSCRIPT_H\n"
    "// Avoid having to inform include path if header is already include before\n"
    "#include <angelscript.h>\n"
    "#endif\n"
    "#include <new>\n"
    "\n"
    "namespace gw {\n"
    "\n"
    "template <typename T> class Proxy {\n"
    "    public:\n"
    "        T value;\n"
    "        Proxy(T value) : value(value) {}\n"
    "        static T cast(void * ptr) {\n"
    "            return reinterpret_cast<Proxy<T> *>(&ptr)->value;\n"
    "        }\n"
    "    private:\n"
    "        Proxy(const Proxy &);\n"
    "        Proxy & operator=(const Proxy &);\n"
    "};\n"
    "\n"
    "template <typename T> struct Wrapper {};\n"
    "template <typename T> struct ObjFirst {};\n"
    "template <typename T> struct ObjLast {};\n"
    "template <typename T> struct Constructor {};\n"
    "\n"
    "template <typename T>\n"
    "void destroy(AS_NAMESPACE_QUALIFIER asIScriptGeneric * gen) {\n"
    "    static_cast<T *>(gen->GetObject())->~T();\n"
    "}\n");

    string typename_list = "typename A0";
    string type_list     = "A0";
    string arg_list      = "\n                static_cast<Proxy <A0> *>(gen->GetAddressOfArg(0))->value";
    string new_exp       = "new (gen->GetAddressOfReturnLocation()) Proxy<R>";
    string obj_exp       = "static_cast<T *>(gen->GetObject())->*";
    string obj_arg_exp   = "\n                Proxy<T>::cast(gen->GetObject())";

    PrintTemplate("",                       "", "void", "",    "",       "",              "",              "void", "", "Wrapper");
    PrintTemplate("typename R",             "", "R",    "",    "",       new_exp.c_str(), "",              "void", "", "Wrapper");
    PrintTemplate("typename T",             "", "void", "T::", "",       "",              obj_exp.c_str(), "void", "", "Wrapper");
    PrintTemplate("typename T, typename R", "", "R",    "T::", "",       new_exp.c_str(), obj_exp.c_str(), "void", "", "Wrapper");
    PrintTemplate("typename T",             "", "void", "T::", " const", "",              obj_exp.c_str(), "void", "", "Wrapper");
    PrintTemplate("typename T, typename R", "", "R",    "T::", " const", new_exp.c_str(), obj_exp.c_str(), "void", "", "Wrapper");

    PrintTemplate("typename T",             "", "void", "",    "",       "",              "",      "T",    obj_arg_exp.c_str(), "ObjFirst");
    PrintTemplate("typename T, typename R", "", "R",    "",    "",       new_exp.c_str(), "",      "T",    obj_arg_exp.c_str(), "ObjFirst");
    PrintTemplate("typename T",             "", "void", "",    "",       "",              "",      "T",    obj_arg_exp.c_str(), "ObjLast");
    PrintTemplate("typename T, typename R", "", "R",    "",    "",       new_exp.c_str(), "",      "T",    obj_arg_exp.c_str(), "ObjLast");

    PrintConstructor("", "", "", "");

    for( int i = 0; i < max_args; i++ )
    {
        PrintTemplate("",                         typename_list.c_str(), "void", "",    "",       "",              "",              type_list.c_str(), arg_list.c_str(), "Wrapper");
        PrintTemplate("typename R, ",             typename_list.c_str(), "R",    "",    "",       new_exp.c_str(), "",              type_list.c_str(), arg_list.c_str(), "Wrapper");
        PrintTemplate("typename T, ",             typename_list.c_str(), "void", "T::", "",       "",              obj_exp.c_str(), type_list.c_str(), arg_list.c_str(), "Wrapper");
        PrintTemplate("typename T, typename R, ", typename_list.c_str(), "R",    "T::", "",       new_exp.c_str(), obj_exp.c_str(), type_list.c_str(), arg_list.c_str(), "Wrapper");
        PrintTemplate("typename T, ",             typename_list.c_str(), "void", "T::", " const", "",              obj_exp.c_str(), type_list.c_str(), arg_list.c_str(), "Wrapper");
        PrintTemplate("typename T, typename R, ", typename_list.c_str(), "R",    "T::", " const", new_exp.c_str(), obj_exp.c_str(), type_list.c_str(), arg_list.c_str(), "Wrapper");

        PrintTemplate("typename T, ",             typename_list.c_str(), "void", "", "", "",              "", ("T, " + type_list).c_str(), (obj_arg_exp + "," + arg_list).c_str(), "ObjFirst");
        PrintTemplate("typename T, typename R, ", typename_list.c_str(), "R",    "", "", new_exp.c_str(), "", ("T, " + type_list).c_str(), (obj_arg_exp + "," + arg_list).c_str(), "ObjFirst");
        PrintTemplate("typename T, ",             typename_list.c_str(), "void", "", "", "",              "", (type_list + ", T").c_str(), (arg_list + "," + obj_arg_exp).c_str(), "ObjLast");
        PrintTemplate("typename T, typename R, ", typename_list.c_str(), "R",    "", "", new_exp.c_str(), "", (type_list + ", T").c_str(), (arg_list + "," + obj_arg_exp).c_str(), "ObjLast");

        PrintConstructor(", ", typename_list.c_str(), type_list.c_str(), arg_list.c_str());

        char buf[5];
        sprintf(buf, "%d", i + 1);
        typename_list += ", typename A" + string(buf);
        type_list     += ", A" + string(buf);
        arg_list      += ",\n                static_cast<Proxy <A" + string(buf) + "> *>(gen->GetAddressOfArg(" + string(buf) + "))->value";
    }

    printf("template <typename T>\n"
    "struct Id {\n"
    "    template <T fn_ptr> AS_NAMESPACE_QUALIFIER asSFuncPtr  f(void) { return asFUNCTION(&Wrapper<T>::template f<fn_ptr>); }\n"
    "    template <T fn_ptr> AS_NAMESPACE_QUALIFIER asSFuncPtr of(void) { return asFUNCTION(&ObjFirst<T>::template f<fn_ptr>); }\n"
    "    template <T fn_ptr> AS_NAMESPACE_QUALIFIER asSFuncPtr ol(void) { return asFUNCTION(&ObjLast<T>::template f<fn_ptr>); }\n"
    "};\n"
    "\n"
    "template <typename T>\n"
    "Id<T> id(T fn_ptr) { return Id<T>(); }\n"
    "\n"
    "// On some versions of GNUC it is necessary to use the template keyword as disambiguator,\n"
    "// on others the template keyword gives an error, hence the need for the following define.\n"
    "// MSVC on the other hand seems to accept both with or without the template keyword.\n"
    "#if defined(__GNUC__) && (__GNUC__ < 4 || (__GNUC__ == 4 && __GNUC_MINOR__ < 4))\n"
    "    // GNUC 4.4.3 doesn't need the template keyword, and\n"
    "    // hopefully upcoming versions won't need it either\n"
    "    #define TMPL template\n"
    "#else\n"
    "    #define TMPL\n"
    "#endif\n"
    "\n"
    "#define WRAP_FN(name)             (::gw::id(name).TMPL f< name >())\n"
    "#define WRAP_MFN(ClassType, name) (::gw::id(&ClassType::name).TMPL f< &ClassType::name >())\n"
    "#define WRAP_OBJ_FIRST(name)      (::gw::id(name).TMPL of< name >())\n"
    "#define WRAP_OBJ_LAST(name)       (::gw::id(name).TMPL ol< name >())\n"
    "\n"
    "#define WRAP_FN_PR(name, Parameters, ReturnType)             asFUNCTION((::gw::Wrapper<ReturnType (*)Parameters>::TMPL f< name >))\n"
    "#define WRAP_MFN_PR(ClassType, name, Parameters, ReturnType) asFUNCTION((::gw::Wrapper<ReturnType (ClassType::*)Parameters>::TMPL f< &ClassType::name >))\n"
    "#define WRAP_OBJ_FIRST_PR(name, Parameters, ReturnType)      asFUNCTION((::gw::ObjFirst<ReturnType (*)Parameters>::TMPL f< name >))\n"
    "#define WRAP_OBJ_LAST_PR(name, Parameters, ReturnType)       asFUNCTION((::gw::ObjLast<ReturnType (*)Parameters>::TMPL f< name >))\n"
    "\n"
    "#define WRAP_CON(ClassType, Parameters) asFUNCTION((::gw::Constructor<ClassType Parameters>::f))\n"
    "#define WRAP_DES(ClassType)             asFUNCTION((::gw::destroy<ClassType>))\n"
    "\n"
    "} // end namespace gw\n"
    "\n"
    "#endif\n");

    return 0;
}

void PrintTemplate(const char *base, const char *typeNameList, const char *retType, const char *objType, const char *isConst, const char *newExpr, const char *objExpr, const char *argList1, const char *argList2, const char *wrapName)
{
    printf("template <%s%s>\n", base, typeNameList);
    printf("struct %s<%s (%s*)(%s)%s> {\n", wrapName, retType, objType, argList1, isConst);
    printf("    template <%s (%s*fp)(%s)%s>\n", retType, objType, argList1, isConst);
    printf("    static void f(AS_NAMESPACE_QUALIFIER asIScriptGeneric * gen) {\n");
    printf("        %s((%sfp)(%s));\n", newExpr, objExpr, argList2);
    printf("    }\n");
    printf("};\n");
}

void PrintConstructor(const char *comma, const char *typeNameList, const char *typeList, const char *argList)
{
    printf("template <typename T%s%s>\n", comma, typeNameList);
    printf("struct Constructor <T (%s)> {\n", typeList);
    printf("    static void f(AS_NAMESPACE_QUALIFIER asIScriptGeneric * gen) {\n");
    printf("        new (gen->GetObject()) T(%s);\n", argList);
    printf("    }\n");
    printf("};\n");
}