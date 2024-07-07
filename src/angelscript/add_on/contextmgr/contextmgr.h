#ifndef CONTEXTMGR_H
#define CONTEXTMGR_H

// The context manager simplifies the management of multiple concurrent scripts

// More than one context manager can be used, if you wish to control different
// groups of scripts separately, e.g. game object scripts, and GUI scripts.

// OBSERVATION: This class is currently not thread safe.

#ifndef ANGELSCRIPT_H
// Avoid having to inform include path if header is already include before
#include <angelscript.h>
#endif

#include <vector>

BEGIN_AS_NAMESPACE

class CScriptDictionary;

// The internal structure for holding contexts
struct SContextInfo;

// The signature of the get time callback function
typedef asUINT (*TIMEFUNC_t)();

class CContextMgr
{
public:
    CContextMgr();
    ~CContextMgr();

    // Set the function that the manager will use to obtain the time in milliseconds
    void SetGetTimeCallback(TIMEFUNC_t func);

    // Registers the following:
    //
    //  void sleep(uint milliseconds)
    //
    // The application must set the get time callback for this to work
    void RegisterThreadSupport(asIScriptEngine *engine);

    // Registers the following:
    //
    //  funcdef void coroutine(dictionary@)
    //  void createCoRoutine(coroutine @func, dictionary @args)
    //  void yield()
    void RegisterCoRoutineSupport(asIScriptEngine *engine);

    // Create a new context, prepare it with the function id, then return
    // it so that the application can pass the argument values. The context
    // will be released by the manager after the execution has completed.
    // Set keepCtxAfterExecution to true if the application needs to retrieve
    // information from the context after it the script has finished.
    asIScriptContext *AddContext(asIScriptEngine *engine, asIScriptFunction *func, bool keepCtxAfterExecution = false);

    // If the context was kept after the execution, this method must be
    // called when the application is done with the context so it can be
    // returned to the pool for reuse.
    void DoneWithContext(asIScriptContext *ctx);

    // Create a new context, prepare it with the function id, then return
    // it so that the application can pass the argument values. The context
    // will be added as a co-routine in the same thread as the currCtx.
    asIScriptContext *AddContextForCoRoutine(asIScriptContext *currCtx, asIScriptFunction *func);

    // Execute each script that is not currently sleeping. The function returns after
    // each script has been executed once. The application should call this function
    // for each iteration of the message pump, or game loop, or whatever.
    // Returns the number of scripts still in execution.
    int ExecuteScripts();

    // Put a script to sleep for a while
    void SetSleeping(asIScriptContext *ctx, asUINT milliSeconds);

    // Switch the execution to the next co-routine in the group.
    // Returns true if the switch was successful.
    void NextCoRoutine();

    // Abort all scripts
    void AbortAll();

protected:
    std::vector<SContextInfo*> m_threads;
    std::vector<SContextInfo*> m_freeThreads;
    asUINT                     m_currentThread;
    TIMEFUNC_t                 m_getTimeFunc;

    // Statistics for Garbage Collection
    asUINT   m_numExecutions;
    asUINT   m_numGCObjectsCreated;
    asUINT   m_numGCObjectsDestroyed;
};


END_AS_NAMESPACE

#endif
