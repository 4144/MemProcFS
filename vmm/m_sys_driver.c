// m_sys_driver.c : implementation related to the sys/drivers built-in module.
//
// The 'sys/drivers' module lists various aspects of drivers from the windows
// kernel object manager.
//
// (c) Ulf Frisk, 2021
// Author: Ulf Frisk, pcileech@frizk.net
//
#include "vmm.h"
#include "util.h"
#include "pluginmanager.h"
#include "vmmwinobj.h"
#include "vmmwindef.h"

#define MSYSDRIVER_DRV_LINELENGTH                  128ULL
#define MSYSDRIVER_IRP_LINELENGTH                  88ULL
#define MSYSDRIVER_DRV_LINEHEADER     L"   #   Object Address Driver               Size Drv Range: Start-End              Service Key      Driver Name"
#define MSYSDRIVER_IRP_LINEHEADER     L"   # Driver            # IRP_MJ_*                          Address Target Module"

#define MSYSDRIVER_IRP_STR (LPWSTR[]){ \
    L"CREATE",                   \
    L"CREATE_NAMED_PIPE",        \
    L"CLOSE",                    \
    L"READ",                     \
    L"WRITE",                    \
    L"QUERY_INFORMATION",        \
    L"SET_INFORMATION",          \
    L"QUERY_EA",                 \
    L"SET_EA",                   \
    L"FLUSH_BUFFERS",            \
    L"QUERY_VOLUME_INFORMATION", \
    L"SET_VOLUME_INFORMATION",   \
    L"DIRECTORY_CONTROL",        \
    L"FILE_SYSTEM_CONTROL",      \
    L"DEVICE_CONTROL",           \
    L"INTERNAL_DEVICE_CONTROL",  \
    L"SHUTDOWN",                 \
    L"LOCK_CONTROL",             \
    L"CLEANUP",                  \
    L"CREATE_MAILSLOT",          \
    L"QUERY_SECURITY",           \
    L"SET_SECURITY",             \
    L"POWER",                    \
    L"SYSTEM_CONTROL",           \
    L"DEVICE_CHANGE",            \
    L"QUERY_QUOTA",              \
    L"SET_QUOTA",                \
    L"PNP" }

typedef struct tdMSYSDRIVER_IRP_CONTEXT {
    PVMMOB_MAP_PTE pPteMap;
    PVMMOB_MAP_KDRIVER pDrvMap;
} MSYSDRIVER_IRP_CONTEXT, *PMSYSDRIVER_IRP_CONTEXT;

/*
* Comparison function to efficiently locate a single PTE given address and map.
*/
int MSysDriver_PteCmpFind(_In_ PVOID va, _In_ PVMM_MAP_PTEENTRY pe)
{
    if((QWORD)va < pe->vaBase) { return -1; }
    if((QWORD)va > pe->vaBase + (pe->cPages << 12) - 1) { return 1; }
    return 0;
}

/*
* Line callback function to print a single driver/irp line.
*/
VOID MSysDriver_IrpReadLine_Callback(_In_ PMSYSDRIVER_IRP_CONTEXT ctx, _In_ DWORD cbLineLength, _In_ DWORD ie, _In_ PVOID pv, _Out_writes_(cbLineLength + 1) LPSTR szu8)
{
    PVMM_MAP_PTEENTRY pePte;
    PVMM_MAP_KDRIVERENTRY pe;
    QWORD vaIrp;
    DWORD iDrv, iIrp;
    LPWSTR wsz = L"?";
    iDrv = ie / 28;
    iIrp = ie % 28;
    pe = ctx->pDrvMap->pMap + iDrv;
    vaIrp = pe->MajorFunction[iIrp];
    if(vaIrp == ctxVmm->kernel.opt.vaIopInvalidDeviceRequest) {
        wsz = L"---";
    } else if((vaIrp >= pe->vaStart) && (vaIrp < pe->vaStart + pe->cbDriverSize)) {
        wsz = pe->wszName;
    } else if((pePte = Util_qfind((PVOID)vaIrp, ctx->pPteMap->cMap, ctx->pPteMap->pMap, sizeof(VMM_MAP_PTEENTRY), MSysDriver_PteCmpFind))) {
        wsz = pePte->wszText;
    }
    Util_snwprintf_u8ln(szu8, cbLineLength,
        L"%04x %-16.16s %2i %-24.24s %16llx %s",
        ie,
        pe->wszName,
        iIrp,
        MSYSDRIVER_IRP_STR[iIrp],
        vaIrp,
        wsz
    );
}

VOID MSysDriver_DrvReadLine_Callback(_Inout_opt_ PVOID ctx, _In_ DWORD cbLineLength, _In_ DWORD ie, _In_ PVMM_MAP_KDRIVERENTRY pe, _Out_writes_(cbLineLength + 1) LPSTR szu8)
{
    Util_snwprintf_u8ln(szu8, cbLineLength,
        L"%04x %16llx %-16.16s %8llx %16llx-%16llx %-16.16s %s",
        ie,
        pe->va,
        pe->wszName,
        pe->cbDriverSize,
        pe->vaStart,
        pe->cbDriverSize ? (pe->vaStart + pe->cbDriverSize - 1) : pe->vaStart,
        pe->wszServiceKeyName,
        pe->wszPath
    );
}

_Success_(return)
BOOL MSysDriver_EntryFromPath(_In_ LPWSTR wszPath, _In_ PVMMOB_MAP_KDRIVER pDrvMap, _Out_ PVMM_MAP_KDRIVERENTRY * ppDrvMapEntry, _Out_opt_ LPWSTR *pwszPath)
{
    DWORD i, dwHash;
    WCHAR wsz[MAX_PATH];
    if(_wcsnicmp(wszPath, L"by-name\\", 8)) { return FALSE; }
    Util_PathSplit2_ExWCHAR(wszPath + 8, wsz, MAX_PATH);
    dwHash = Util_HashNameW_Registry(wsz, 0);
    for(i = 0; i < pDrvMap->cMap; i++) {
        if(dwHash == pDrvMap->pMap[i].dwHash) {
            if(pwszPath) { *pwszPath = wszPath + 8; }
            *ppDrvMapEntry = pDrvMap->pMap + i;
            return TRUE;
        }
    }
    return FALSE;
}

NTSTATUS MSysDriver_Read(_In_ PVMMDLL_PLUGIN_CONTEXT ctx, _Out_writes_to_(cb, *pcbRead) PBYTE pb, _In_ DWORD cb, _Out_ PDWORD pcbRead, _In_ QWORD cbOffset)
{
    NTSTATUS nt = VMMDLL_STATUS_FILE_INVALID;
    PVMM_MAP_KDRIVERENTRY pe;
    PVMMOB_MAP_PTE pObPteMap = NULL;
    PVMMOB_MAP_KDRIVER pObDrvMap = NULL;
    PVMM_PROCESS pObSystemProcess = NULL;
    MSYSDRIVER_IRP_CONTEXT IrpCtx = { 0 };
    if(!VmmMap_GetKDriver(&pObDrvMap)) { goto cleanup; }
    if(!_wcsicmp(ctx->wszPath, L"drivers.txt")) {
        nt = Util_VfsLineFixed_Read(
            MSysDriver_DrvReadLine_Callback, NULL, MSYSDRIVER_DRV_LINELENGTH, MSYSDRIVER_DRV_LINEHEADER,
            pObDrvMap->pMap, pObDrvMap->cMap, sizeof(VMM_MAP_KDRIVERENTRY),
            pb, cb, pcbRead, cbOffset
        );
        goto cleanup;
    }
    if(!_wcsicmp(ctx->wszPath, L"driver_irp.txt")) {
        if(!(pObSystemProcess = VmmProcessGet(4))) { goto cleanup; }
        if(!VmmMap_GetPte(pObSystemProcess, &pObPteMap, TRUE)) { goto cleanup; }
        IrpCtx.pDrvMap = pObDrvMap;
        IrpCtx.pPteMap = pObPteMap;
        nt = Util_VfsLineFixed_Read(
            MSysDriver_IrpReadLine_Callback, &IrpCtx, MSYSDRIVER_IRP_LINELENGTH, MSYSDRIVER_IRP_LINEHEADER,
            pObDrvMap->pMap, pObDrvMap->cMap * 28ULL, 0,
            pb, cb, pcbRead, cbOffset
        );
        goto cleanup;
    }
    if(!_wcsnicmp(L"by-name\\", ctx->wszPath, 8)) {
        if(MSysDriver_EntryFromPath(ctx->wszPath, pObDrvMap, &pe, NULL)) {
            nt = VmmWinObjDisplay_VfsRead(ctx->wszPath, ctxVmm->ObjectTypeTable.tpDriver, pe->va, pb, cb, pcbRead, cbOffset);
            goto cleanup;
        }
    }
cleanup:
    Ob_DECREF(pObDrvMap);
    Ob_DECREF(pObPteMap);
    Ob_DECREF(pObSystemProcess);
    return nt;
}

BOOL MSysDriver_List(_In_ PVMMDLL_PLUGIN_CONTEXT ctx, _Inout_ PHANDLE pFileList)
{
    DWORD i;
    LPWSTR wszPath;
    PVMM_MAP_KDRIVERENTRY pe;
    PVMMOB_MAP_KDRIVER pObDrvMap = NULL;
    if(!VmmMap_GetKDriver(&pObDrvMap)) { goto finish; }
    if(!ctx->wszPath[0]) {
        VMMDLL_VfsList_AddDirectory(pFileList, L"by-name", NULL);
        VMMDLL_VfsList_AddFile(pFileList, L"drivers.txt", UTIL_VFSLINEFIXED_LINECOUNT(pObDrvMap->cMap) * MSYSDRIVER_DRV_LINELENGTH, NULL);
        VMMDLL_VfsList_AddFile(pFileList, L"driver_irp.txt", UTIL_VFSLINEFIXED_LINECOUNT(pObDrvMap->cMap * 28ULL) * MSYSDRIVER_IRP_LINELENGTH, NULL);
        goto finish;
    }
    if(!_wcsicmp(L"by-name", ctx->wszPath)) {
        for(i = 0; i < pObDrvMap->cMap; i++) {
            VMMDLL_VfsList_AddDirectory(pFileList, pObDrvMap->pMap[i].wszName, NULL);
        }
        goto finish;
    }
    if(MSysDriver_EntryFromPath(ctx->wszPath, pObDrvMap, &pe, &wszPath) && wszPath[0]) {
        VmmWinObjDisplay_VfsList(ctxVmm->ObjectTypeTable.tpDriver, pe->va, pFileList);
        goto finish;
    }
finish:
    Ob_DECREF(pObDrvMap);
    return TRUE;
}

VOID M_SysDriver_Initialize(_Inout_ PVMMDLL_PLUGIN_REGINFO pRI)
{
    if((pRI->magic != VMMDLL_PLUGIN_REGINFO_MAGIC) || (pRI->wVersion != VMMDLL_PLUGIN_REGINFO_VERSION)) { return; }
    if((pRI->tpSystem != VMM_SYSTEM_WINDOWS_X64) && (pRI->tpSystem != VMM_SYSTEM_WINDOWS_X86)) { return; }
    if(pRI->sysinfo.dwVersionBuild < 7600) { return; }              // WIN7+ required
    wcscpy_s(pRI->reg_info.wszPathName, 128, L"\\sys\\drivers");    // module name
    pRI->reg_info.fRootModule = TRUE;                               // module shows in root directory
    pRI->reg_fn.pfnList = MSysDriver_List;                          // List function supported
    pRI->reg_fn.pfnRead = MSysDriver_Read;                          // Read function supported
    pRI->pfnPluginManager_Register(pRI);
}
