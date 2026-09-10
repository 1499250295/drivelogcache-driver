/*++

    driver.h - DriveLogCache minifilter 驱动内部定义

--*/

#ifndef _DLC_DRIVER_H_
#define _DLC_DRIVER_H_

#include <fltKernel.h>
#include <ntstrsafe.h>
#include "dlc_proto.h"

#pragma warning(disable:4204)  /* 非标准扩展: 结构中初始化聚合 */

#define DLC_POOL_TAG        (ULONG) 'CLwD'   /* 'DwLC' */

#define DLC_FILE_BUCKETS    4093
#define DC_PROC_BUCKETS     4093
#define DLC_BLOCK_BUCKETS   8191

#define DLC_MAX_QUEUED      8192
#define DLC_BATCH_RECORDS   24
#define DLC_LOG_STACK_LIMIT 0x4000

/*-------------------------------------------------------------------------- 
  全局数据
--------------------------------------------------------------------------*/

typedef struct _DLC_GLOBAL {

    PFLT_FILTER      FilterHandle;
    PFLT_PORT        ServerPort;
    PFLT_PORT        ClientPort;       /* 已连接的用户态端口 */

    /* IO 日志队列 (自旋锁保护, 任意 IRQL) */
    KSPIN_LOCK       QueueLock;
    LIST_ENTRY       LogQueue;
    volatile ULONG   QueuedCount;
    volatile ULONG   DroppedCount;
    KEVENT           LogEvent;
    PVOID            LogThreadObj;
    HANDLE           LogThreadHandle;
    volatile LONG    StopWorker;

    volatile LONG    LoggingEnabled;
    volatile LONG    LogMode;         /* 0=游戏模式(按进程树) 1=全盘模式(按卷) */
    volatile ULONG   LogVolSerial;    /* 全盘模式下采集的卷序列号 */

    /* cache.dat 系统空间映射 */
    PVOID            CacheBase;
    SIZE_T           CacheSize;
    ULONG            BlockSize;

} DLC_GLOBAL;

extern DLC_GLOBAL g_Dlc;

/*-------------------------------------------------------------------------- 
  日志节点
--------------------------------------------------------------------------*/

typedef struct _DLC_LOG_NODE {
    LIST_ENTRY    List;
    DLC_IO_RECORD Record;
} DLC_LOG_NODE;

/*-------------------------------------------------------------------------- 
  FileObject -> 文件信息 表
--------------------------------------------------------------------------*/

typedef struct _DLC_FILE_INFO {
    PFILE_OBJECT        FileObject;   /* 键 */
    ULONG               VolSerial;
    unsigned long long  FileId;
    WCHAR               Path[DLC_PATH_CHARS];
    struct _DLC_FILE_INFO *Next;
} DLC_FILE_INFO;

/*-------------------------------------------------------------------------- 
  缓存块哈希表
--------------------------------------------------------------------------*/

typedef struct _DLC_BLOCK_KEY {
    ULONG               VolSerial;
    ULONG               Pad;
    unsigned long long  FileId;
    unsigned long long  BlockIndex;
} DLC_BLOCK_KEY;

typedef struct _DLC_BLOCK_NODE {
    DLC_BLOCK_KEY          Key;
    unsigned long long     CacheOffset;
    struct _DLC_BLOCK_NODE *Next;
} DLC_BLOCK_NODE;

/*-------------------------------------------------------------------------- 
  进程树表
--------------------------------------------------------------------------*/

typedef struct _DLC_PROC_NODE {
    HANDLE                  Pid;
    HANDLE                  Ppid;
    BOOLEAN                 InTarget;
    CHAR                    Image[DLC_IMAGE_CHARS];
    struct _DLC_PROC_NODE  *Next;
} DLC_PROC_NODE;

/*-------------------------------------------------------------------------- 
  函数声明
--------------------------------------------------------------------------*/

/* driver.c */
DRIVER_INITIALIZE  DriverEntry;
DRIVER_UNLOAD      DlcUnload;
NTSTATUS DlcDriverUnload(FLT_FILTER_UNLOAD_FLAGS Flags);

/* comm.c */
NTSTATUS DlcCommCreate(PFLT_FILTER Filter);
VOID     DlcCommClose(void);
NTSTATUS DlcSendToApp(PVOID Buffer, ULONG Length);
NTSTATUS DlcHandleMessage(_In_reads_bytes_(InLen) PVOID InBuffer, ULONG InLen,
                          _Out_writes_bytes_to_opt_(OutLen, *OutLen) PVOID OutBuffer,
                          ULONG OutLen, _Out_ PULONG OutLenRet);

/* proctree.c */
NTSTATUS DlcProcInit(void);
VOID     DlcProcDestroy(void);
VOID     DlcProcAdd(HANDLE Pid, HANDLE Ppid, BOOLEAN InTarget, PCUNICODE_STRING ImagePath);
VOID     DlcProcRemove(HANDLE Pid);
BOOLEAN  DlcProcIsTarget(HANDLE Pid);
VOID     DlcProcSetTarget(HANDLE Pid, BOOLEAN InTarget);
VOID     DlcProcClearTargets(void);
NTSTATUS DlcProcRegisterNotify(void);
VOID     DlcProcUnregisterNotify(void);

/* filectx.c */
NTSTATUS DlcFileInit(void);
VOID     DlcFileDestroy(void);
DLC_FILE_INFO *DlcFileLookup(PFILE_OBJECT FileObject);
NTSTATUS DlcFileInsertFromCreate(PFLT_CALLBACK_DATA Data, PCFLT_RELATED_OBJECTS FltObjects);
VOID     DlcFileRemove(PFILE_OBJECT FileObject);

/* blockmap.c */
NTSTATUS DlcBlockInit(void);
VOID     DlcBlockDestroy(void);
BOOLEAN  DlcBlockLookup(ULONG VolSerial, unsigned long long FileId,
                        unsigned long long BlockIndex,
                        _Out_ unsigned long long *CacheOffset);
BOOLEAN  DlcBlockRemove(ULONG VolSerial, unsigned long long FileId,
                        unsigned long long BlockIndex);
VOID     DlcBlockClear(void);
NTSTATUS DlcBlockReplace(_In_ DLC_BLOCK_ENTRY *Entries, ULONG Count, ULONG BlockSize);

/* cachemap.c */
NTSTATUS DlcCacheOpen(_In_ PCWSTR DosPath, ULONG BlockSize, ULONG BlockCount);
VOID     DlcCacheClose(void);

/* io.c */
FLT_PREOP_CALLBACK_STATUS  DlcPreRead(_Inout_ PFLT_CALLBACK_DATA Data, _In_ PCFLT_RELATED_OBJECTS FltObjects, _Flt_CompletionContext_Outptr_ PVOID *CompletionContext);
FLT_PREOP_CALLBACK_STATUS  DlcPreWrite(_Inout_ PFLT_CALLBACK_DATA Data, _In_ PCFLT_RELATED_OBJECTS FltObjects, _Flt_CompletionContext_Outptr_ PVOID *CompletionContext);
FLT_POSTOP_CALLBACK_STATUS DlcPostCreate(_Inout_ PFLT_CALLBACK_DATA Data, _In_ PCFLT_RELATED_OBJECTS FltObjects, _In_opt_ PVOID CompletionContext, _In_ FLT_POST_OPERATION_FLAGS Flags);
FLT_POSTOP_CALLBACK_STATUS DlcPostClose(_Inout_ PFLT_CALLBACK_DATA Data, _In_ PCFLT_RELATED_OBJECTS FltObjects, _In_opt_ PVOID CompletionContext, _In_ FLT_POST_OPERATION_FLAGS Flags);
VOID     DlcLogEnqueue(UCHAR Op, UCHAR Flags, PFLT_CALLBACK_DATA Data, DLC_FILE_INFO *Info);

/* util: 简单内存/字符串 */
ULONG    DlcHashBytes(_In_reads_bytes_(Len) const VOID *Data, ULONG Len);
VOID     DlcAnsiFromUnicodePath(_Out_writes_z_(DstCount) PCHAR Dst, ULONG DstCount, _In_ PCUNICODE_STRING Src);

#endif /* _DLC_DRIVER_H_ */
