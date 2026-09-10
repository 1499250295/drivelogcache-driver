/*++

    blockmap.c - 缓存块描述符哈希表

    键: (卷序列号, 文件ID, 文件内块号)  值: 在 cache.dat 中的字节偏移。
    应用层构建好缓存后通过 SET_TABLE 消息整体下发, 驱动原子替换整表。
    写 IO 命中时逐块删除 (失效)。

--*/

#include "driver.h"

static KSPIN_LOCK       g_BlockLock;
static PDLC_BLOCK_NODE  g_BlockBuckets[DLC_BLOCK_BUCKETS];
static ULONG            g_TableBlockSize = 0;

static ULONG DlcBlockBucket(ULONG VolSerial, unsigned long long FileId,
                            unsigned long long BlockIndex)
{
    DLC_BLOCK_KEY key;
    key.VolSerial = VolSerial;
    key.Pad = 0;
    key.FileId = FileId;
    key.BlockIndex = BlockIndex;
    return DlcHashBytes(&key, sizeof(key)) % DLC_BLOCK_BUCKETS;
}

NTSTATUS DlcBlockInit(void)
{
    RtlZeroMemory(g_BlockBuckets, sizeof(g_BlockBuckets));
    KeInitializeSpinLock(&g_BlockLock);
    return STATUS_SUCCESS;
}

static VOID DlcBlockFreeAllLocked(void)
{
    for (ULONG b = 0; b < DLC_BLOCK_BUCKETS; b++) {
        PDLC_BLOCK_NODE node = g_BlockBuckets[b];
        while (node) {
            PDLC_BLOCK_NODE next = node->Next;
            ExFreePoolWithTag(node, DLC_POOL_TAG);
            node = next;
        }
        g_BlockBuckets[b] = NULL;
    }
    g_TableBlockSize = 0;
}

VOID DlcBlockDestroy(void)
{
    KIRQL oldIrql;
    KeAcquireSpinLock(&g_BlockLock, &oldIrql);
    DlcBlockFreeAllLocked();
    KeReleaseSpinLock(&g_BlockLock, oldIrql);
}

BOOLEAN DlcBlockLookup(ULONG VolSerial, unsigned long long FileId,
                       unsigned long long BlockIndex,
                       _Out_ unsigned long long *CacheOffset)
{
    KIRQL oldIrql;
    ULONG bucket = DlcBlockBucket(VolSerial, FileId, BlockIndex);
    PDLC_BLOCK_NODE node;
    BOOLEAN found = FALSE;

    KeAcquireSpinLock(&g_BlockLock, &oldIrql);
    for (node = g_BlockBuckets[bucket]; node != NULL; node = node->Next) {
        if (node->Key.VolSerial == VolSerial &&
            node->Key.FileId == FileId &&
            node->Key.BlockIndex == BlockIndex) {
            *CacheOffset = node->CacheOffset;
            found = TRUE;
            break;
        }
    }
    KeReleaseSpinLock(&g_BlockLock, oldIrql);
    return found;
}

BOOLEAN DlcBlockRemove(ULONG VolSerial, unsigned long long FileId,
                       unsigned long long BlockIndex)
{
    KIRQL oldIrql;
    ULONG bucket = DlcBlockBucket(VolSerial, FileId, BlockIndex);
    PDLC_BLOCK_NODE *pp;
    BOOLEAN removed = FALSE;

    KeAcquireSpinLock(&g_BlockLock, &oldIrql);
    pp = &g_BlockBuckets[bucket];
    while (*pp != NULL) {
        PDLC_BLOCK_NODE node = *pp;
        if (node->Key.VolSerial == VolSerial &&
            node->Key.FileId == FileId &&
            node->Key.BlockIndex == BlockIndex) {
            *pp = node->Next;
            ExFreePoolWithTag(node, DLC_POOL_TAG);
            removed = TRUE;
            break;
        }
        pp = &node->Next;
    }
    KeReleaseSpinLock(&g_BlockLock, oldIrql);
    return removed;
}

VOID DlcBlockClear(void)
{
    KIRQL oldIrql;
    KeAcquireSpinLock(&g_BlockLock, &oldIrql);
    DlcBlockFreeAllLocked();
    KeReleaseSpinLock(&g_BlockLock, oldIrql);
}

/* 整体替换块表 (在通信消息回调中, PASSIVE_LEVEL 调用) */
NTSTATUS DlcBlockReplace(_In_ DLC_BLOCK_ENTRY *Entries, ULONG Count, ULONG BlockSize)
{
    PDLC_BLOCK_NODE *newBuckets = NULL;
    SIZE_T bytes;
    NTSTATUS status = STATUS_SUCCESS;
    ULONG i;

    if (BlockSize == 0 || BlockSize % 4096 != 0) {
        return STATUS_INVALID_PARAMETER;
    }
    if (Count == 0) {
        DlcBlockClear();
        g_Dlc.BlockSize = BlockSize;
        return STATUS_SUCCESS;
    }

    bytes = sizeof(PDLC_BLOCK_NODE) * DLC_BLOCK_BUCKETS;
    newBuckets = ExAllocatePool2(POOL_FLAG_NON_PAGED, bytes, DLC_POOL_TAG);
    if (newBuckets == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(newBuckets, bytes);

    for (i = 0; i < Count; i++) {
        PDLC_BLOCK_NODE node;
        ULONG bucket;

        /* 校验缓存范围 */
        if (Entries[i].CacheOffset + BlockSize > g_Dlc.CacheSize) {
            continue;
        }
        if (Entries[i].FileId == 0) {
            continue;
        }

        node = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(DLC_BLOCK_NODE), DLC_POOL_TAG);
        if (node == NULL) {
            status = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }
        RtlZeroMemory(node, sizeof(*node));
        node->Key.VolSerial = Entries[i].VolSerial;
        node->Key.FileId = Entries[i].FileId;
        node->Key.BlockIndex = Entries[i].BlockIndex;
        node->CacheOffset = Entries[i].CacheOffset;

        bucket = DlcBlockBucket(node->Key.VolSerial, node->Key.FileId,
                                node->Key.BlockIndex);
        node->Next = newBuckets[bucket];
        newBuckets[bucket] = node;
    }

    if (NT_SUCCESS(status)) {
        KIRQL oldIrql;
        KeAcquireSpinLock(&g_BlockLock, &oldIrql);
        DlcBlockFreeAllLocked();
        for (i = 0; i < DLC_BLOCK_BUCKETS; i++) {
            g_BlockBuckets[i] = newBuckets[i];
        }
        g_TableBlockSize = BlockSize;
        KeReleaseSpinLock(&g_BlockLock, oldIrql);
        g_Dlc.BlockSize = BlockSize;
    } else {
        for (ULONG b = 0; b < DLC_BLOCK_BUCKETS; b++) {
            PDLC_BLOCK_NODE node = newBuckets[b];
            while (node) {
                PDLC_BLOCK_NODE next = node->Next;
                ExFreePoolWithTag(node, DLC_POOL_TAG);
                node = next;
            }
        }
        ExFreePoolWithTag(newBuckets, DLC_POOL_TAG);
    }
    return status;
}
