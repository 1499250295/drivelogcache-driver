/*++

    io.c - 读命中加速 / 写失效 / IO 原始日志

    日志采集 (二选一模式, 由 DLC_MSG_SET_LOG_MODE 切换):
      - 游戏模式 (LogMode=0): 仅记录目标进程树 (主进程+所有子进程) 的 IO
      - 全盘模式 (LogMode=1): 记录指定卷上所有进程的读写 IO
    驱动先把记录放入内存队列缓冲, 由工作线程批量上报应用层落盘。

    缓存加速 (启动加速后, 对所有进程生效):
      PreRead : 读 IO 覆盖的所有缓存块全部在块表中 ("完全匹配缓存") 时,
                直接用 cache.dat 映射内存填充缓冲区并 FLT_PREOP_COMPLETE 完成;
                部分匹配则不加速, 原样放行到 NTFS。
      PreWrite: 写范围覆盖到的缓存块全部从块表删除 (失效), 写始终放行。

--*/

#include "driver.h"

#define DLC_MAX_SERVE_BLOCKS   512
#define DLC_MAX_INVALIDATE_BLOCKS 4096

/* 是否应对本条 IO 记录日志 */
static BOOLEAN DlcShouldLog(DLC_FILE_INFO *Info)
{
    HANDLE pid;

    if (!g_Dlc.LoggingEnabled || Info == NULL) {
        return FALSE;
    }
    if (g_Dlc.LogMode == 1) {
        /* 全盘模式: 按卷序列号采集该卷上的全部 IO */
        return Info->VolSerial == g_Dlc.LogVolSerial;
    }
    /* 游戏模式: 仅目标进程树 */
    pid = PsGetCurrentProcessId();
    return DlcProcIsTarget(pid);
}

/*-------------------------------------------------------------------------- 
  日志入队
--------------------------------------------------------------------------*/

VOID DlcLogEnqueue(UCHAR Op, UCHAR Flags, PFLT_CALLBACK_DATA Data, DLC_FILE_INFO *Info)
{
    PDLC_LOG_NODE node;
    KIRQL oldIrql;

    if (!g_Dlc.LoggingEnabled) {
        return;
    }

    node = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(DLC_LOG_NODE), DLC_POOL_TAG);
    if (node == NULL) {
        KeAcquireSpinLock(&g_Dlc.QueueLock, &oldIrql);
        g_Dlc.DroppedCount++;
        KeReleaseSpinLock(&g_Dlc.QueueLock, oldIrql);
        return;
    }

    RtlZeroMemory(node, sizeof(*node));

    KeQuerySystemTime((PLARGE_INTEGER)&node->Record.Time);
    node->Record.Pid  = HandleToUlong(PsGetCurrentProcessId());
    node->Record.Tid  = HandleToUlong(PsGetCurrentThreadId());
    node->Record.Op   = Op;
    node->Record.Flags = Flags;

    {
        PCHAR img = PsGetProcessImageFileName(PsGetCurrentProcess());
        if (img != NULL) {
            ULONG i;
            for (i = 0; i < DLC_IMAGE_CHARS - 1 && img[i] != '\0'; i++) {
                node->Record.Image[i] = img[i];
            }
        }
    }

    node->Record.Offset = Data->Iopb->Parameters.Read.ByteOffset.QuadPart;
    node->Record.Length = Data->Iopb->Parameters.Read.Length;

    if (Info != NULL) {
        node->Record.VolSerial = Info->VolSerial;
        node->Record.FileId    = Info->FileId;
        RtlCopyMemory(node->Record.Path, Info->Path,
                      DLC_PATH_CHARS * sizeof(WCHAR));
    }

    KeAcquireSpinLock(&g_Dlc.QueueLock, &oldIrql);
    if (g_Dlc.QueuedCount >= DLC_MAX_QUEUED) {
        g_Dlc.DroppedCount++;
        KeReleaseSpinLock(&g_Dlc.QueueLock, oldIrql);
        ExFreePoolWithTag(node, DLC_POOL_TAG);
        return;
    }
    InsertTailList(&g_Dlc.LogQueue, &node->List);
    g_Dlc.QueuedCount++;
    KeReleaseSpinLock(&g_Dlc.QueueLock, oldIrql);

    KeSetEvent(&g_Dlc.LogEvent, IO_NO_INCREMENT, FALSE);
}

/*-------------------------------------------------------------------------- 
  PreRead
--------------------------------------------------------------------------*/

FLT_PREOP_CALLBACK_STATUS DlcPreRead(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID *CompletionContext)
{
    DLC_FILE_INFO *info;
    LONGLONG ioOffset;
    ULONG ioLength;
    ULONG blockSize;
    UCHAR logFlags = 0;
    BOOLEAN doLog;

    UNREFERENCED_PARAMETER(CompletionContext);

    if (Data->Iopb->IrpFlags & IRP_PAGING_IO) {
        logFlags |= DLC_IOFLAG_PAGING;
    }
    if (Data->Iopb->IrpFlags & IRP_NOCACHE) {
        logFlags |= DLC_IOFLAG_NOCACHE;
    }

    info = DlcFileLookup(FltObjects->FileObject);
    if (info == NULL) {
        /* 无文件标识 (表中无此 FileObject), 放行 */
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    doLog = DlcShouldLog(info);

    ioOffset = Data->Iopb->Parameters.Read.ByteOffset.QuadPart;
    ioLength = Data->Iopb->Parameters.Read.Length;

    if (ioLength == 0 || ioOffset < 0 || !FLT_IS_IRP_OPERATION(Data)) {
        if (doLog) {
            DlcLogEnqueue(1, logFlags, Data, info);
        }
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    blockSize = g_Dlc.BlockSize;

    /* 缓存命中加速: 索引已加载时对所有进程的读 IO 做匹配 */
    if (g_Dlc.CacheBase != NULL && blockSize >= 4096 &&
        (blockSize % 4096) == 0) {

        LONGLONG firstBlock = ioOffset / blockSize;
        LONGLONG lastBlock  = (ioOffset + (LONGLONG)ioLength - 1) / blockSize;
        ULONG span = (ULONG)(lastBlock - firstBlock + 1);

        if (span <= DLC_MAX_SERVE_BLOCKS) {

            unsigned long long cacheOffsets[DLC_MAX_SERVE_BLOCKS];
            BOOLEAN allHit = TRUE;

            RtlZeroMemory(cacheOffsets, sizeof(cacheOffsets));

            for (ULONG i = 0; i < span; i++) {
                if (!DlcBlockLookup(info->VolSerial, info->FileId,
                                    (unsigned long long)(firstBlock + i),
                                    &cacheOffsets[i])) {
                    allHit = FALSE;
                    break;
                }
            }

            if (allHit) {
                PVOID dest = NULL;
                BOOLEAN mdlPath = (Data->Iopb->Parameters.Read.MdlAddress != NULL);

                if (mdlPath) {
                    dest = MmGetSystemAddressForMdlSafe(
                               Data->Iopb->Parameters.Read.MdlAddress,
                               NormalPagePriority);
                } else if (KeGetCurrentIrql() == PASSIVE_LEVEL) {
                    dest = Data->Iopb->Parameters.Read.UserBuffer;
                    __try {
                        ProbeForWrite(dest, ioLength, 1);
                    } __except (EXCEPTION_EXECUTE_HANDLER) {
                        dest = NULL;
                    }
                }

                if (dest != NULL) {
                    BOOLEAN copyOk = TRUE;

                    __try {
                        for (ULONG i = 0; i < span; i++) {
                            LONGLONG blockStart = (firstBlock + i) * blockSize;
                            LONGLONG copyStart  = max(ioOffset, blockStart);
                            LONGLONG copyEnd    = min(ioOffset + (LONGLONG)ioLength,
                                                      blockStart + blockSize);
                            ULONG chunk = (ULONG)(copyEnd - copyStart);
                            PUCHAR src = (PUCHAR)g_Dlc.CacheBase +
                                         cacheOffsets[i] +
                                         (copyStart - blockStart);
                            PUCHAR dst = (PUCHAR)dest + (copyStart - ioOffset);
                            RtlCopyMemory(dst, src, chunk);
                        }
                    } __except (EXCEPTION_EXECUTE_HANDLER) {
                        copyOk = FALSE;
                    }

                    if (copyOk) {
                        Data->IoStatus.Status = STATUS_SUCCESS;
                        Data->IoStatus.Information = ioLength;
                        FltSetCallbackDataDirty(Data);
                        if (doLog) {
                            DlcLogEnqueue(1, logFlags | DLC_IOFLAG_HIT, Data, info);
                        }
                        return FLT_PREOP_COMPLETE;
                    }
                }
            }
        }
    }

    /* 未命中 (含部分匹配): 放行到 NTFS */
    if (doLog) {
        DlcLogEnqueue(1, logFlags, Data, info);
    }
    return FLT_PREOP_SUCCESS_NO_CALLBACK;
}

/*-------------------------------------------------------------------------- 
  PreWrite
--------------------------------------------------------------------------*/

FLT_PREOP_CALLBACK_STATUS DlcPreWrite(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID *CompletionContext)
{
    DLC_FILE_INFO *info;
    LONGLONG ioOffset;
    ULONG ioLength;
    ULONG blockSize;
    UCHAR logFlags = 0;
    BOOLEAN invalidated = FALSE;
    BOOLEAN doLog;

    UNREFERENCED_PARAMETER(CompletionContext);

    if (Data->Iopb->IrpFlags & IRP_PAGING_IO) {
        logFlags |= DLC_IOFLAG_PAGING;
    }
    if (Data->Iopb->IrpFlags & IRP_NOCACHE) {
        logFlags |= DLC_IOFLAG_NOCACHE;
    }

    info = DlcFileLookup(FltObjects->FileObject);
    if (info == NULL) {
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    doLog = DlcShouldLog(info);

    ioOffset = Data->Iopb->Parameters.Write.ByteOffset.QuadPart;
    ioLength = Data->Iopb->Parameters.Write.Length;

    /* 写失效: 任何进程写 IO 涉及的缓存块一律失效 */
    blockSize = g_Dlc.BlockSize;
    if (g_Dlc.CacheBase != NULL && blockSize >= 4096 &&
        (blockSize % 4096) == 0 && ioLength > 0 && ioOffset >= 0) {

        LONGLONG firstBlock = ioOffset / blockSize;
        LONGLONG lastBlock  = (ioOffset + (LONGLONG)ioLength - 1) / blockSize;
        ULONG span = (ULONG)(lastBlock - firstBlock + 1);
        ULONG limit = (span < DLC_MAX_INVALIDATE_BLOCKS) ? span
                                                        : DLC_MAX_INVALIDATE_BLOCKS;

        for (ULONG i = 0; i < limit; i++) {
            if (DlcBlockRemove(info->VolSerial, info->FileId,
                               (unsigned long long)(firstBlock + i))) {
                invalidated = TRUE;
            }
        }
    }

    if (invalidated) {
        logFlags |= DLC_IOFLAG_INVALIDATE;
    }
    if (doLog) {
        DlcLogEnqueue(2, logFlags, Data, info);
    }

    return FLT_PREOP_SUCCESS_NO_CALLBACK;
}

/*-------------------------------------------------------------------------- 
  PostCreate / PostClose
--------------------------------------------------------------------------*/

FLT_POSTOP_CALLBACK_STATUS DlcPostCreate(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_opt_ PVOID CompletionContext,
    _In_ FLT_POST_OPERATION_FLAGS Flags)
{
    UNREFERENCED_PARAMETER(CompletionContext);

    if (Flags == FLTFL_POST_OPERATION_DRAINING) {
        return FLT_POSTOP_FINISHED_PROCESSING;
    }

    /* IRP_MJ_CREATE 的 post 回调在 PASSIVE_LEVEL, 可安全查询文件名 */
    DlcFileInsertFromCreate(Data, FltObjects);

    return FLT_POSTOP_FINISHED_PROCESSING;
}

FLT_POSTOP_CALLBACK_STATUS DlcPostClose(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_opt_ PVOID CompletionContext,
    _In_ FLT_POST_OPERATION_FLAGS Flags)
{
    UNREFERENCED_PARAMETER(Data);
    UNREFERENCED_PARAMETER(CompletionContext);
    UNREFERENCED_PARAMETER(Flags);

    if (FltObjects->FileObject != NULL) {
        DlcFileRemove(FltObjects->FileObject);
    }
    return FLT_POSTOP_FINISHED_PROCESSING;
}
