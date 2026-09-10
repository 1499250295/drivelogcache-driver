/*++

    filectx.c - FileObject -> (卷序列号 / 文件ID / NT路径) 映射表

    PreRead/PreWrite 可能运行在 APC_LEVEL (分页 IO), 不能调用
    FltGetFileNameInformation / FltQueryInformationFile。因此在
    PostCreate (PASSIVE_LEVEL) 时预先取得文件标识与路径, 按
    FileObject 指针入表, PostClose 时移除。

--*/

#include "driver.h"

static KSPIN_LOCK      g_FileLock;
static PDLC_FILE_INFO  g_FileBuckets[DLC_FILE_BUCKETS];

static ULONG DlcFileBucket(PFILE_OBJECT FileObject)
{
    ULONG_PTR v = (ULONG_PTR)FileObject;
    return (ULONG)((v * 2654435761u) >> 12) % DLC_FILE_BUCKETS;
}

NTSTATUS DlcFileInit(void)
{
    RtlZeroMemory(g_FileBuckets, sizeof(g_FileBuckets));
    KeInitializeSpinLock(&g_FileLock);
    return STATUS_SUCCESS;
}

VOID DlcFileDestroy(void)
{
    KIRQL oldIrql;
    KeAcquireSpinLock(&g_FileLock, &oldIrql);
    for (ULONG b = 0; b < DLC_FILE_BUCKETS; b++) {
        PDLC_FILE_INFO node = g_FileBuckets[b];
        while (node) {
            PDLC_FILE_INFO next = node->Next;
            ExFreePoolWithTag(node, DLC_POOL_TAG);
            node = next;
        }
        g_FileBuckets[b] = NULL;
    }
    KeReleaseSpinLock(&g_FileLock, oldIrql);
}

DLC_FILE_INFO *DlcFileLookup(PFILE_OBJECT FileObject)
{
    KIRQL oldIrql;
    ULONG bucket = DlcFileBucket(FileObject);
    PDLC_FILE_INFO node;
    PDLC_FILE_INFO result = NULL;

    KeAcquireSpinLock(&g_FileLock, &oldIrql);
    for (node = g_FileBuckets[bucket]; node != NULL; node = node->Next) {
        if (node->FileObject == FileObject) {
            result = node;
            break;
        }
    }
    KeReleaseSpinLock(&g_FileLock, oldIrql);
    return result;
}

VOID DlcFileRemove(PFILE_OBJECT FileObject)
{
    KIRQL oldIrql;
    ULONG bucket = DlcFileBucket(FileObject);
    PDLC_FILE_INFO *pp;

    KeAcquireSpinLock(&g_FileLock, &oldIrql);
    pp = &g_FileBuckets[bucket];
    while (*pp != NULL) {
        if ((*pp)->FileObject == FileObject) {
            PDLC_FILE_INFO dead = *pp;
            *pp = dead->Next;
            ExFreePoolWithTag(dead, DLC_POOL_TAG);
            break;
        }
        pp = &(*pp)->Next;
    }
    KeReleaseSpinLock(&g_FileLock, oldIrql);
}

NTSTATUS DlcFileInsertFromCreate(PFLT_CALLBACK_DATA Data, PCFLT_RELATED_OBJECTS FltObjects)
{
    NTSTATUS status;
    PFLT_FILE_NAME_INFORMATION nameInfo = NULL;
    FILE_INTERNAL_INFORMATION internalInfo;
    FILE_FS_VOLUME_INFORMATION volInfo;
    ULONG returned;
    PDLC_FILE_INFO info = NULL;

    /* 只关心成功的数据文件创建 */
    if (!NT_SUCCESS(Data->IoStatus.Status)) {
        return STATUS_SUCCESS;
    }
    if (FltObjects->FileObject == NULL) {
        return STATUS_SUCCESS;
    }

    status = FltGetFileNameInformation(Data,
                                       FLT_FILE_NAME_NORMALIZED |
                                       FLT_FILE_NAME_QUERY_DEFAULT,
                                       &nameInfo);
    if (!NT_SUCCESS(status)) {
        return STATUS_SUCCESS;   /* 拿不到名字不影响过滤, 记一条无名记录即可 */
    }

    status = FltParseFileNameInformation(nameInfo);
    if (!NT_SUCCESS(status)) {
        goto out;
    }

    RtlZeroMemory(&internalInfo, sizeof(internalInfo));
    status = FltQueryInformationFile(FltObjects->Instance,
                                     FltObjects->FileObject,
                                     FileInternalInformation,
                                     &internalInfo, sizeof(internalInfo),
                                     &returned);
    if (!NT_SUCCESS(status)) {
        goto out;
    }

    RtlZeroMemory(&volInfo, sizeof(volInfo));
    status = FltQueryVolumeInformation(FltObjects->Volume,
                                       FileFsVolumeInformation,
                                       &volInfo, sizeof(volInfo),
                                       &returned);
    if (!NT_SUCCESS(status)) {
        goto out;
    }

    info = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(DLC_FILE_INFO), DLC_POOL_TAG);
    if (info == NULL) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto out;
    }
    RtlZeroMemory(info, sizeof(*info));
    info->FileObject = FltObjects->FileObject;
    info->VolSerial = volInfo.VolumeSerialNumber;
    info->FileId = internalInfo.IndexNumber.QuadPart;

    RtlStringCchCopyW(info->Path, DLC_PATH_CHARS, nameInfo->Name.Buffer);

    {
        KIRQL oldIrql;
        ULONG bucket = DlcFileBucket(FltObjects->FileObject);
        KeAcquireSpinLock(&g_FileLock, &oldIrql);
        info->Next = g_FileBuckets[bucket];
        g_FileBuckets[bucket] = info;
        KeReleaseSpinLock(&g_FileLock, oldIrql);
    }
    info = NULL;   /* 已归表 */
    status = STATUS_SUCCESS;

out:
    if (nameInfo != NULL) {
        FltReleaseFileNameInformation(nameInfo);
    }
    if (info != NULL) {
        ExFreePoolWithTag(info, DLC_POOL_TAG);
    }
    return status;
}
