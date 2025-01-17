// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
//*****************************************************************************
// File: DebuggerModule.cpp
//

//
// Stuff for tracking DebuggerModules.
//
//*****************************************************************************

#include "stdafx.h"
#include "../inc/common.h"
#include "eeconfig.h" // This is here even for retail & free builds...
#include "vars.hpp"
#include <limits.h>
#include "ilformatter.h"
#include "debuginfostore.h"


/* ------------------------------------------------------------------------ *
 * Debugger Module routines
 * ------------------------------------------------------------------------ */

void DebuggerModule::SetCanChangeJitFlags(bool fCanChangeJitFlags)
{
    m_fCanChangeJitFlags = fCanChangeJitFlags;
}

#ifndef DACCESS_COMPILE


DebuggerModuleTable::DebuggerModuleTable() : CHashTableAndData<CNewZeroData>(101)
{
    WRAPPER_NO_CONTRACT;

    SUPPRESS_ALLOCATION_ASSERTS_IN_THIS_SCOPE;
    NewInit(101, sizeof(DebuggerModuleEntry), 101);
}

DebuggerModuleTable::~DebuggerModuleTable()
{
    WRAPPER_NO_CONTRACT;

    _ASSERTE(ThreadHoldsLock());
    Clear();
}


#ifdef _DEBUG
bool DebuggerModuleTable::ThreadHoldsLock()
{
    // In shutdown (IsAtProcessExit()), the shutdown thread implicitly holds all locks.
    return IsAtProcessExit() || g_pDebugger->HasDebuggerDataLock();
}
#endif

//
// RemoveModules removes any module loaded into the given appdomain from the hash.  This is used when we send an
// ExitAppdomain event to ensure that there are no leftover modules in the hash. This can happen when we have shared
// modules that aren't properly accounted for in the CLR. We miss sending UnloadModule events for those modules, so
// we clean them up with this method.
//
void DebuggerModuleTable::RemoveModules(AppDomain *pAppDomain)
{
    CONTRACTL
    {
        NOTHROW;
        GC_NOTRIGGER;
    }
    CONTRACTL_END;

    LOG((LF_CORDB, LL_INFO1000, "DMT::RM removing all modules from AD 0x%08x\n", pAppDomain));

    _ASSERTE(ThreadHoldsLock());

    HASHFIND hf;
    DebuggerModuleEntry *pDME = (DebuggerModuleEntry *) FindFirstEntry(&hf);

    while (pDME != NULL)
    {
        DebuggerModule *pDM = pDME->module;

        if (pDM->GetAppDomain() == pAppDomain)
        {
            LOG((LF_CORDB, LL_INFO1000, "DMT::RM removing DebuggerModule 0x%08x\n", pDM));

            // Defer to the normal logic in RemoveModule for the actual removal. This accurately simulates what
            // happens when we process an UnloadModule event.
            RemoveModule(pDM->GetRuntimeModule(), pAppDomain);

            // Start back at the first entry since we just modified the hash.
            pDME = (DebuggerModuleEntry *) FindFirstEntry(&hf);
        }
        else
        {
            pDME = (DebuggerModuleEntry *) FindNextEntry(&hf);
        }
    }

    LOG((LF_CORDB, LL_INFO1000, "DMT::RM done removing all modules from AD 0x%08x\n", pAppDomain));
}

void DebuggerModuleTable::Clear()
{
    CONTRACTL
    {
        NOTHROW;
        GC_NOTRIGGER;
    }
    CONTRACTL_END;

    _ASSERTE(ThreadHoldsLock());

    HASHFIND hf;
    DebuggerModuleEntry *pDME;

    pDME = (DebuggerModuleEntry *) FindFirstEntry(&hf);

    while (pDME)
    {
        DebuggerModule *pDM = pDME->module;
        Module         *pEEM = pDM->GetRuntimeModule();

        TRACE_FREE(pDME->module);
        DeleteInteropSafe(pDM);
        Delete(HASH(pEEM), (HASHENTRY *) pDME);

        pDME = (DebuggerModuleEntry *) FindFirstEntry(&hf);
    }

    CHashTableAndData<CNewZeroData>::Clear();
}

void DebuggerModuleTable::AddModule(DebuggerModule *pModule)
{
    CONTRACTL
    {
        THROWS;
        GC_NOTRIGGER;
    }
    CONTRACTL_END;

    _ASSERTE(ThreadHoldsLock());

    _ASSERTE(pModule != NULL);

    LOG((LF_CORDB, LL_EVERYTHING, "DMT::AM: DebuggerMod:0x%x Module:0x%x AD:0x%x\n",
        pModule, pModule->GetRuntimeModule(), pModule->GetAppDomain()));

    DebuggerModuleEntry * pEntry = (DebuggerModuleEntry *) Add(HASH(pModule->GetRuntimeModule()));
    if (pEntry == NULL)
    {
        ThrowOutOfMemory();
    }

    pEntry->module = pModule;
}

//-----------------------------------------------------------------------------
// Remove a DebuggerModule from the module table when it gets notified.
// This occurs in response to the finalization of an unloaded AssemblyLoadContext.
//-----------------------------------------------------------------------------
void DebuggerModuleTable::RemoveModule(Module* pModule, AppDomain *pAppDomain)
{
    CONTRACTL
    {
        NOTHROW;
        GC_NOTRIGGER;
    }
    CONTRACTL_END;

    LOG((LF_CORDB, LL_INFO1000, "DMT::RM Attempting to remove Module:0x%x AD:0x%x\n", pModule, pAppDomain));

    _ASSERTE(ThreadHoldsLock());
    _ASSERTE(pModule != NULL);

    HASHFIND hf;

    for (DebuggerModuleEntry *pDME = (DebuggerModuleEntry *) FindFirstEntry(&hf);
         pDME != NULL;
         pDME = (DebuggerModuleEntry*) FindNextEntry(&hf))
    {
        DebuggerModule *pDM = pDME->module;
        Module* pRuntimeModule = pDM->GetRuntimeModule();

        if ((pRuntimeModule == pModule) && (pDM->GetAppDomain() == pAppDomain))
        {
            LOG((LF_CORDB, LL_INFO1000, "DMT::RM Removing DebuggerMod:0x%x - Module:0x%x DF:0x%x AD:0x%x\n",
                pDM, pModule, pDM->GetDomainAssembly(), pAppDomain));
            TRACE_FREE(pDM);
            DeleteInteropSafe(pDM);
            Delete(HASH(pRuntimeModule), (HASHENTRY *) pDME);
            _ASSERTE(GetModule(pModule, pAppDomain) == NULL);
            return;
        }
    }

    LOG((LF_CORDB, LL_INFO1000, "DMT::RM  No debugger module found for Module:0x%x AD:0x%x\n", pModule, pAppDomain));
}


#endif // DACCESS_COMPILE

DebuggerModule *DebuggerModuleTable::GetModule(Module* module)
{
    CONTRACTL
    {
        NOTHROW;
        GC_NOTRIGGER;
    }
    CONTRACTL_END;

    _ASSERTE(module != NULL);
    _ASSERTE(ThreadHoldsLock());

    DebuggerModuleEntry *entry
      = (DebuggerModuleEntry *) Find(HASH(module), KEY(module));
    if (entry == NULL)
        return NULL;
    else
        return entry->module;
}

// We should never look for a NULL Module *
DebuggerModule *DebuggerModuleTable::GetModule(Module* module, AppDomain* pAppDomain)
{
    CONTRACTL
    {
        NOTHROW;
        GC_NOTRIGGER;
    }
    CONTRACTL_END;

    _ASSERTE(module != NULL);
    _ASSERTE(ThreadHoldsLock());


    HASHFIND findmodule;
    DebuggerModuleEntry *moduleentry;

    for (moduleentry =  (DebuggerModuleEntry*) FindFirstEntry(&findmodule);
         moduleentry != NULL;
         moduleentry =  (DebuggerModuleEntry*) FindNextEntry(&findmodule))
    {
        DebuggerModule *pModule = moduleentry->module;

        if ((pModule->GetRuntimeModule() == module) &&
            (pModule->GetAppDomain() == pAppDomain))
            return pModule;
    }

    // didn't find any match! So return a matching module for any app domain
    return NULL;
}

DebuggerModule *DebuggerModuleTable::GetFirstModule(HASHFIND *info)
{
    CONTRACTL
    {
        NOTHROW;
        GC_NOTRIGGER;
    }
    CONTRACTL_END;

    _ASSERTE(ThreadHoldsLock());

    DebuggerModuleEntry *entry = (DebuggerModuleEntry *) FindFirstEntry(info);
    if (entry == NULL)
        return NULL;
    else
        return entry->module;
}

DebuggerModule *DebuggerModuleTable::GetNextModule(HASHFIND *info)
{
    CONTRACTL
    {
        NOTHROW;
        GC_NOTRIGGER;
    }
    CONTRACTL_END;

    _ASSERTE(ThreadHoldsLock());

    DebuggerModuleEntry *entry = (DebuggerModuleEntry *) FindNextEntry(info);
    if (entry == NULL)
        return NULL;
    else
        return entry->module;
}
