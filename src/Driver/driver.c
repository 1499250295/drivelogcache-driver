/*++

    driver.c - DriverEntry / minifilter 注册 / 卸载 / 日志工作线程

    DriveLogCache: NTFS 读缓存加速 minifilter
      - 读 IO 完全命中缓存块时直接由缓存数据完成 (加速)
      - 写 IO 命中缓存块时该块失效
      - 目标进程树 (游戏主进程及其所有子进程) 的原始 IO 记录批量上报应用层

--*/

#include "driver.h"

#pragma comment(lib, "fltKernel.lib")
#pragma comment(lib, "ntoskrnl.lib")

DLC_GLOBAL g_Dlc;

/*-------------------------------------------------------------------------- 
  Minifilter 注册
--------------------------------------------------------------------------*/

const FLT_OPERATION_REGISTRATION g_Callbacks[] =
{
    { IRP_MJ_CREATE,
      0,
      NULL,
      DlcPostCreate },

    { IRP_MJ_READ,
      0,
      DlcPreRead,
      NULL },

    { IRP_MJ_WRITE,
      0,
      DlcPreWrite,
      NULL },

    { IRP_MJ_CLOSE,
      0,
      NULL,
      DlcPostClose },

    { IRP_MJ_OPERATION_END }
};

const FLT_CONTEXT_REGISTRATION g_Contexts[] =
{
    { FLT_CONTEXT_END }
};

const FLT_REGISTRATION g_FilterRegistration =
{
    sizeof(FLT_REGISTRATION),               /* Size */
    FLT_REGISTRATION_VERSION,               /* Version */
    0,                                      /* Flags */
    (PFLT_CONTEXT_REGISTRATION)g_Contexts,  /* ContextRegistration */
    g_Callbacks,                            /* OperationRegistration */
    DlcDriverUnload,                        /* FilterUnloadCallback */
    NULL,                                   /* InstanceSetupCallback */
    NULL,                                   /* InstanceQueryTeardownCallback */
    NULL,                                   /* InstanceTeardownStartCallback */
    NULL,                                   /* InstanceTeardownCompleteCallback */
    NULL,                                   /* GenerateFileNameCallback */
    NULL,                                   /* NormalizeNameComponentCallback */
    NULL,                                   /* NormalizeNameContextCleanupCallback */
    NULL,                                   /* SectionNotificationCallback */
};

/*-------------------------------------------------------------------------- 
  日志工作线程: 批量取出记录, FltSendMessage 上报应用层
--------------------------------------------------------------------------*/

VOID DlcLogWorker(PVOID StartContext)
{
    UNREFERENCED_PARAMETER(StartContext);

    for (;;) {
        KeWaitForSingleObject(&g_Dlc.LogEvent, Executive, KernelMode, FALSE, NULL);

        if (g_Dlc.StopWorker) {
            break;
        }

        while (g_Dlc.ClientPort != NULL && !g_Dlc.StopWorker) {

            PLIST_ENTRY batchList[DLC_BATCH_RECORDS];
            ULONG i = 0;
            ULONG dropped;
            ULONG msgLen;
            PVOID msg = NULL;
            NTSTATUS status;
            PDLC_IO_BATCH batch;

            /* 取一批节点 */
            KeAcquireSpinLockAtDpcLevel(&g_Dlc.QueueLock);
            while (i < DLC_BATCH_RECORDS && !IsListEmpty(&g_Dlc.LogQueue)) {
                PLIST_ENTRY entry = RemoveHeadList(&g_Dlc.LogQueue);
                batchList[i] = entry;
                i++;
            }
            g_Dlc.QueuedCount -= i;
            dropped = g_Dlc.DroppedCount;
            g_Dlc.DroppedCount = 0;
            KeReleaseSpinLockFromDpcLevel(&g_Dlc.QueueLock);

            if (i == 0) {
                break;
            }

            msgLen = FIELD_OFFSET(DLC_IO_BATCH, Records) + sizeof(DLC_IO_RECORD) * i;
            msg = ExAllocatePool2(POOL_FLAG_NON_PAGED, msgLen, DLC_POOL_TAG);
            if (msg == NULL) {
                /* 内存不足, 丢弃本批 */
                for (ULONG j = 0; j < i; j++) {
                    CONTAINING_RECORD(batchList[j], DLC_LOG_NODE, List);
                    ExFreePoolWithTag(CONTAINING_RECORD(batchList[j], DLC_LOG_NODE, List), DLC_POOL_TAG);
                }
                continue;
            }

            RtlZeroMemory(msg, msgLen);
            batch = (PDLC_IO_BATCH)msg;
            batch->Header.MsgType = DLC_MSG_IO_BATCH;
            batch->Header.MsgLen = msgLen;
            batch->Count = i;
            batch->Dropped = dropped;

            for (ULONG j = 0; j < i; j++) {
                PDLC_LOG_NODE node = CONTAINING_RECORD(batchList[j], DLC_LOG_NODE, List);
                RtlCopyMemory(&batch->Records[j], &node->Record, sizeof(DLC_IO_RECORD));
                ExFreePoolWithTag(node, DLC_POOL_TAG);
            }

            status = DlcSendToApp(msg, msgLen);
            ExFreePoolWithTag(msg, DLC_POOL_TAG);

            if (!NT_SUCCESS(status)) {
                /* 发送失败 (端口断开等), 剩余记录留在队列等下次连接 */
                break;
            }
        }
    }

    PsTerminateSystemThread(STATUS_SUCCESS);
}

NTSTATUS DlcWorkerStart(void)
{
    OBJECT_ATTRIBUTES oa;
    NTSTATUS status;

    InitializeObjectAttributes(&oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    status = PsCreateSystemThread(&g_Dlc.LogThreadHandle, THREAD_ALL_ACCESS,
                                  &oa, NULL, NULL, DlcLogWorker, NULL);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    status = ObReferenceObjectByHandle(g_Dlc.LogThreadHandle, THREAD_ALL_ACCESS,
                                       NULL, KernelMode, &g_Dlc.LogThreadObj, NULL);
    if (!NT_SUCCESS(status)) {
        g_Dlc.LogThreadObj = NULL;
        return status;
    }
    return STATUS_SUCCESS;
}

VOID DlcWorkerStop(void)
{
    g_Dlc.StopWorker = 1;
    KeSetEvent(&g_Dlc.LogEvent, IO_NO_INCREMENT, FALSE);

    if (g_Dlc.LogThreadObj != NULL) {
        KeWaitForSingleObject(g_Dlc.LogThreadObj, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(g_Dlc.LogThreadObj);
        g_Dlc.LogThreadObj = NULL;
    }
    if (g_Dlc.LogThreadHandle != NULL) {
        ZwClose(g_Dlc.LogThreadHandle);
        g_Dlc.LogThreadHandle = NULL;
    }

    /* 清空残留日志队列 */
    KIRQL oldIrql;
    KeAcquireSpinLock(&g_Dlc.QueueLock, &oldIrql);
    while (!IsListEmpty(&g_Dlc.LogQueue)) {
        PLIST_ENTRY entry = RemoveHeadList(&g_Dlc.LogQueue);
        PDLC_LOG_NODE node = CONTAINING_RECORD(entry, DLC_LOG_NODE, List);
        ExFreePoolWithTag(node, DLC_POOL_TAG);
    }
    g_Dlc.QueuedCount = 0;
    KeReleaseSpinLock(&g_Dlc.QueueLock, oldIrql);
}

/*-------------------------------------------------------------------------- 
  卸载
--------------------------------------------------------------------------*/

NTSTATUS DlcDriverUnload(FLT_FILTER_UNLOAD_FLAGS Flags)
{
    UNREFERENCED_PARAMETER(Flags);

    DlcProcUnregisterNotify();
    DlcCommClose();

    if (g_Dlc.FilterHandle != NULL) {
        FltUnregisterFilter(g_Dlc.FilterHandle);
        g_Dlc.FilterHandle = NULL;
    }

    DlcWorkerStop();
    DlcCacheClose();
    DlcBlockDestroy();
    DlcFileDestroy();
    DlcProcDestroy();

    return STATUS_SUCCESS;
}

VOID DlcUnload(PDRIVER_OBJECT DriverObject)
{
    /* minifilter 不使用 DriverObject->DriverUnload; 由 FltUnregister 触发 DlcDriverUnload */
    UNREFERENCED_PARAMETER(DriverObject);
}

/*-------------------------------------------------------------------------- 
  DriverEntry
--------------------------------------------------------------------------*/

NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    NTSTATUS status;

    UNREFERENCED_PARAMETER(RegistryPath);
    UNREFERENCED_PARAMETER(DriverObject);

    RtlZeroMemory(&g_Dlc, sizeof(g_Dlc));
    KeInitializeSpinLock(&g_Dlc.QueueLock);
    InitializeListHead(&g_Dlc.LogQueue);
    /* 必须用 SynchronizationEvent (自动复位): 工作线程等待被满足时事件自动复位,
       队列排空后再次等待会真正睡眠; 若用 NotificationEvent 且不手动复位,
       首次入队后事件永久触发, 工作线程在队列空时也会忙等, 打满一个 CPU 核心。 */
    KeInitializeEvent(&g_Dlc.LogEvent, SynchronizationEvent, FALSE);
    g_Dlc.LoggingEnabled = 0;

    status = DlcProcInit();
    if (!NT_SUCCESS(status)) goto cleanup;
    status = DlcFileInit();
    if (!NT_SUCCESS(status)) goto cleanup;
    status = DlcBlockInit();
    if (!NT_SUCCESS(status)) goto cleanup;

    status = FltRegisterFilter(DriverObject, (PFLT_REGISTRATION)&g_FilterRegistration,
                               &g_Dlc.FilterHandle);
    if (!NT_SUCCESS(status)) {
        goto cleanup;
    }

    status = DlcCommCreate(g_Dlc.FilterHandle);
    if (!NT_SUCCESS(status)) {
        goto cleanup;
    }

    status = DlcWorkerStart();
    if (!NT_SUCCESS(status)) {
        goto cleanup;
    }

    status = DlcProcRegisterNotify();
    if (!NT_SUCCESS(status)) {
        goto cleanup;
    }

    status = FltStartFiltering(g_Dlc.FilterHandle);
    if (!NT_SUCCESS(status)) {
        goto cleanup;
    }

    return STATUS_SUCCESS;

cleanup:
    DlcProcUnregisterNotify();
    if (g_Dlc.LogThreadObj != NULL || g_Dlc.LogThreadHandle != NULL) {
        DlcWorkerStop();
    }
    DlcCommClose();
    if (g_Dlc.FilterHandle != NULL) {
        FltUnregisterFilter(g_Dlc.FilterHandle);
        g_Dlc.FilterHandle = NULL;
    }
    DlcCacheClose();
    DlcBlockDestroy();
    DlcFileDestroy();
    DlcProcDestroy();
    return status;
}

/*-------------------------------------------------------------------------- 
  工具函数
--------------------------------------------------------------------------*/

ULONG DlcHashBytes(_In_reads_bytes_(Len) const VOID *Data, ULONG Len)
{
    const UCHAR *p = (const UCHAR *)Data;
    ULONG hash = 2166136261u;
    for (ULONG i = 0; i < Len; i++) {
        hash ^= p[i];
        hash *= 16777619u;
    }
    return hash;
}

/* 从形如 "\Device\HarddiskVolume3\path\game.exe" 的 NT 路径取文件名并转 ASCII */
VOID DlcAnsiFromUnicodePath(_Out_writes_z_(DstCount) PCHAR Dst, ULONG DstCount,
                            _In_ PCUNICODE_STRING Src)
{
    ULONG i;
    ULONG lastSep = 0;
    ULONG maxChars;

    if (DstCount == 0) return;
    Dst[0] = '\0';
    if (Src == NULL || Src->Buffer == NULL || Src->Length == 0) return;

    for (i = 0; i < Src->Length / sizeof(WCHAR); i++) {
        if (Src->Buffer[i] == L'\\' || Src->Buffer[i] == L'/') {
            lastSep = i + 1;
        }
    }

    maxChars = (Src->Length / sizeof(WCHAR)) - lastSep;
    if (maxChars > DstCount - 1) maxChars = DstCount - 1;

    for (i = 0; i < maxChars; i++) {
        WCHAR w = Src->Buffer[lastSep + i];
        Dst[i] = (w < 128) ? (CHAR)w : '_';
    }
    Dst[maxChars] = '\0';
}
