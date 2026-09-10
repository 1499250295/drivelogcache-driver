/*++

    proctree.c - 进程树跟踪与目标进程过滤

    应用层下发游戏主进程 PID 后, 驱动通过 PsSetCreateProcessNotifyRoutineEx2
    监听进程创建: 若父进程在目标集合内, 则子进程自动加入目标集合,
    从而覆盖游戏启动器 -> 游戏主进程 -> 反作弊/子进程的整棵进程树。

--*/

#include "driver.h"

static KSPIN_LOCK       g_ProcLock;
static PDLC_PROC_NODE   g_ProcBuckets[DC_PROC_BUCKETS];
static BOOLEAN          g_ProcNotifyRegistered = FALSE;

static ULONG DlcProcBucket(HANDLE Pid)
{
    ULONG_PTR v = (ULONG_PTR)Pid;
    return (ULONG)((v * 2654435761u) >> 12) % DC_PROC_BUCKETS;
}

NTSTATUS DlcProcInit(void)
{
    RtlZeroMemory(g_ProcBuckets, sizeof(g_ProcBuckets));
    KeInitializeSpinLock(&g_ProcLock);
    return STATUS_SUCCESS;
}

VOID DlcProcDestroy(void)
{
    KIRQL oldIrql;
    KeAcquireSpinLock(&g_ProcLock, &oldIrql);
    for (ULONG b = 0; b < DC_PROC_BUCKETS; b++) {
        PDLC_PROC_NODE node = g_ProcBuckets[b];
        while (node) {
            PDLC_PROC_NODE next = node->Next;
            ExFreePoolWithTag(node, DLC_POOL_TAG);
            node = next;
        }
        g_ProcBuckets[b] = NULL;
    }
    KeReleaseSpinLock(&g_ProcLock, oldIrql);
}

VOID DlcProcAdd(HANDLE Pid, HANDLE Ppid, BOOLEAN InTarget, PCUNICODE_STRING ImagePath)
{
    KIRQL oldIrql;
    ULONG bucket;
    PDLC_PROC_NODE node;

    if (Pid == NULL) return;
    bucket = DlcProcBucket(Pid);

    KeAcquireSpinLock(&g_ProcLock, &oldIrql);

    /* 已存在则更新 */
    for (node = g_ProcBuckets[bucket]; node != NULL; node = node->Next) {
        if (node->Pid == Pid) {
            node->Ppid = Ppid;
            if (InTarget) node->InTarget = TRUE;
            if (ImagePath != NULL) {
                DlcAnsiFromUnicodePath(node->Image, DLC_IMAGE_CHARS, ImagePath);
            }
            KeReleaseSpinLock(&g_ProcLock, oldIrql);
            return;
        }
    }

    node = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(DLC_PROC_NODE), DLC_POOL_TAG);
    if (node == NULL) {
        KeReleaseSpinLock(&g_ProcLock, oldIrql);
        return;
    }
    RtlZeroMemory(node, sizeof(*node));
    node->Pid = Pid;
    node->Ppid = Ppid;
    node->InTarget = InTarget;
    if (ImagePath != NULL) {
        DlcAnsiFromUnicodePath(node->Image, DLC_IMAGE_CHARS, ImagePath);
    }
    node->Next = g_ProcBuckets[bucket];
    g_ProcBuckets[bucket] = node;

    KeReleaseSpinLock(&g_ProcLock, oldIrql);
}

VOID DlcProcRemove(HANDLE Pid)
{
    KIRQL oldIrql;
    ULONG bucket = DlcProcBucket(Pid);
    PDLC_PROC_NODE *pp;

    KeAcquireSpinLock(&g_ProcLock, &oldIrql);
    pp = &g_ProcBuckets[bucket];
    while (*pp != NULL) {
        if ((*pp)->Pid == Pid) {
            PDLC_PROC_NODE dead = *pp;
            *pp = dead->Next;
            ExFreePoolWithTag(dead, DLC_POOL_TAG);
            break;
        }
        pp = &(*pp)->Next;
    }
    KeReleaseSpinLock(&g_ProcLock, oldIrql);
}

BOOLEAN DlcProcIsTarget(HANDLE Pid)
{
    KIRQL oldIrql;
    ULONG bucket = DlcProcBucket(Pid);
    BOOLEAN result = FALSE;
    PDLC_PROC_NODE node;

    KeAcquireSpinLock(&g_ProcLock, &oldIrql);
    for (node = g_ProcBuckets[bucket]; node != NULL; node = node->Next) {
        if (node->Pid == Pid) {
            result = node->InTarget;
            break;
        }
    }
    KeReleaseSpinLock(&g_ProcLock, oldIrql);
    return result;
}

VOID DlcProcSetTarget(HANDLE Pid, BOOLEAN InTarget)
{
    KIRQL oldIrql;
    ULONG bucket = DlcProcBucket(Pid);
    PDLC_PROC_NODE node;

    KeAcquireSpinLock(&g_ProcLock, &oldIrql);
    for (node = g_ProcBuckets[bucket]; node != NULL; node = node->Next) {
        if (node->Pid == Pid) {
            node->InTarget = InTarget;
            KeReleaseSpinLock(&g_ProcLock, oldIrql);
            return;
        }
    }
    KeReleaseSpinLock(&g_ProcLock, oldIrql);

    /* 不存在则插入一个 (PPID 未知, notify 触发时会补全) */
    DlcProcAdd(Pid, NULL, InTarget, NULL);
}

VOID DlcProcClearTargets(void)
{
    KIRQL oldIrql;
    KeAcquireSpinLock(&g_ProcLock, &oldIrql);
    for (ULONG b = 0; b < DC_PROC_BUCKETS; b++) {
        for (PDLC_PROC_NODE node = g_ProcBuckets[b]; node != NULL; node = node->Next) {
            node->InTarget = FALSE;
        }
    }
    KeReleaseSpinLock(&g_ProcLock, oldIrql);
}

/*-------------------------------------------------------------------------- 
  进程创建/退出通知
--------------------------------------------------------------------------*/

VOID DlcProcNotifyEx(
    _Inout_ PEPROCESS Process,
    _In_ HANDLE ProcessId,
    _Inout_opt_ PPS_CREATE_NOTIFY_INFO CreateInfo)
{
    UNREFERENCED_PARAMETER(Process);

    if (CreateInfo != NULL) {
        BOOLEAN childOfTarget = FALSE;

        /* 父进程在目标集合 -> 子进程自动加入 (游戏主进程的所有子进程) */
        if (CreateInfo->ParentProcessId != NULL &&
            CreateInfo->ParentProcessId != (HANDLE)0) {
            childOfTarget = DlcProcIsTarget(CreateInfo->ParentProcessId);
        }
        DlcProcAdd(ProcessId, CreateInfo->ParentProcessId,
                   childOfTarget, CreateInfo->ImageFileName);
    } else {
        DlcProcRemove(ProcessId);
    }
}

NTSTATUS DlcProcRegisterNotify(void)
{
    NTSTATUS status;

    if (g_ProcNotifyRegistered) return STATUS_SUCCESS;

    status = PsSetCreateProcessNotifyRoutineEx2(PsCreateProcessNotifyRoutineEx,
                                                (PVOID)DlcProcNotifyEx,
                                                NULL, 0);
    if (NT_SUCCESS(status)) {
        g_ProcNotifyRegistered = TRUE;
    }
    return status;
}

VOID DlcProcUnregisterNotify(void)
{
    if (g_ProcNotifyRegistered) {
        PsSetCreateProcessNotifyRoutineEx2(PsCreateProcessNotifyRoutineEx,
                                           (PVOID)DlcProcNotifyEx,
                                           NULL, 1);
        g_ProcNotifyRegistered = FALSE;
    }
}
