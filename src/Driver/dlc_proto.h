/*++

    dlc_proto.h - DriveLogCache 驱动与应用层共享协议定义

    本头文件同时被内核驱动 (C) 和 WinForms 应用 (C# 镜像结构体) 使用。
    所有结构体按 1 字节紧凑对齐, 小端序 (x64 Windows)。

--*/

#ifndef _DLC_PROTO_H_
#define _DLC_PROTO_H_

/* 通信端口名称 */
#define DLC_PORT_NAME_W          L"\\DlcIoPort"
#define DLC_PORT_NAME_USERMODE   L"\\\\.\\DlcIoPort"

#define DLC_PROTOCOL_VERSION     1

/* 应用 -> 驱动 消息类型 */
#define DLC_MSG_ADD_TARGET       1   /* 添加目标进程 PID           */
#define DLC_MSG_CLEAR_TARGETS    2   /* 清空目标进程集合           */
#define DLC_MSG_SET_LOGGING      3   /* 开启/关闭 IO 日志记录      */
#define DLC_MSG_CLEAR_CACHE      4   /* 卸载缓存映射并清空块表     */
#define DLC_MSG_SET_CACHE        5   /* 设置 cache.dat 路径/块大小 */
#define DLC_MSG_SET_TABLE        6   /* 下发缓存块描述符表         */
#define DLC_MSG_PING             7   /* 连通性测试                 */
#define DLC_MSG_SET_LOG_MODE     8   /* 日志模式: 0=游戏(按进程树) 1=全盘(按卷) */

/* 驱动 -> 应用 消息类型 */
#define DLC_MSG_IO_BATCH         100 /* 批量 IO 日志记录           */

#define DLC_PATH_CHARS           128   /* 文件路径 WCHAR 数 */
#define DLC_IMAGE_CHARS          16    /* 进程映像名 CHAR 数 */
#define DLC_MAX_TABLE_MSG        4096  /* 单条 SET_TABLE 最多块数 */

/* IO 记录标志位 */
#define DLC_IOFLAG_HIT           0x01  /* 读命中缓存, 已由缓存完成 */
#define DLC_IOFLAG_INVALIDATE    0x02  /* 写命中缓存块, 块已失效   */
#define DLC_IOFLAG_PAGING        0x04  /* 分页 IO                  */
#define DLC_IOFLAG_NOCACHE       0x08  /* 无缓存 IO (FILE_NO_INTERMEDIATE_BUFFERING) */

#pragma pack(push, 1)

typedef struct _DLC_MSG_HEADER {
    unsigned long  MsgType;
    unsigned long  MsgLen;          /* 含本头在内的总字节数 */
} DLC_MSG_HEADER;

typedef struct _DLC_TARGET_PID {
    DLC_MSG_HEADER Header;
    unsigned long  Pid;
    unsigned long  Pad;
} DLC_TARGET_PID;

typedef struct _DLC_SET_LOGGING_REQ {
    DLC_MSG_HEADER Header;
    unsigned long  Enable;          /* 1=开 0=关 */
    unsigned long  Pad;
} DLC_SET_LOGGING_REQ;

typedef struct _DLC_SET_LOG_MODE_REQ {
    DLC_MSG_HEADER Header;
    unsigned long  Mode;            /* 0=游戏模式(按目标进程树) 1=全盘模式(按卷采集全部IO) */
    unsigned long  VolSerial;       /* Mode=1 时要采集的卷序列号 */
} DLC_SET_LOG_MODE_REQ;

typedef struct _DLC_SET_CACHE_REQ {
    DLC_MSG_HEADER Header;
    unsigned long  BlockSize;       /* 缓存块大小, 4096 的整数倍 */
    unsigned long  BlockCount;      /* cache.dat 中块总数 */
    wchar_t        Path[260];       /* cache.dat 的 DOS 全路径 */
} DLC_SET_CACHE_REQ;

/* 缓存块描述符: 32 字节 */
typedef struct _DLC_BLOCK_ENTRY {
    unsigned long      VolSerial;   /* 卷序列号 */
    unsigned long      Flags;       /* 保留 */
    unsigned long long FileId;      /* NTFS 文件引用号 (FileInternalInformation) */
    unsigned long long BlockIndex;  /* 文件内块号 = 文件偏移 / BlockSize */
    unsigned long long CacheOffset; /* 在 cache.dat 中的字节偏移 */
} DLC_BLOCK_ENTRY;

typedef struct _DLC_SET_TABLE_REQ {
    DLC_MSG_HEADER Header;
    unsigned long  BlockSize;
    unsigned long  Count;
    DLC_BLOCK_ENTRY Entries[1];     /* 变长 */
} DLC_SET_TABLE_REQ;

/* 单条 IO 原始记录: 316 字节 */
typedef struct _DLC_IO_RECORD {
    unsigned long long Time;        /* 系统时间 (UTC, 100ns 单位) */
    unsigned long  Pid;
    unsigned long  Tid;
    unsigned char  Op;              /* 1=读 2=写 */
    unsigned char  Flags;          /* DLC_IOFLAG_* */
    unsigned short Pad16;
    unsigned long  VolSerial;
    unsigned long long FileId;
    long long      Offset;          /* 文件内字节偏移 */
    unsigned long  Length;          /* IO 字节数 */
    char           Image[DLC_IMAGE_CHARS];  /* 进程短名, ASCII */
    wchar_t        Path[DLC_PATH_CHARS];    /* 文件 NT 路径 */
} DLC_IO_RECORD;

typedef struct _DLC_IO_BATCH {
    DLC_MSG_HEADER Header;
    unsigned long  Count;
    unsigned long  Dropped;         /* 自上次发送以来丢弃的记录数 */
    DLC_IO_RECORD  Records[1];      /* 变长 */
} DLC_IO_BATCH;

#pragma pack(pop)

#endif /* _DLC_PROTO_H_ */
