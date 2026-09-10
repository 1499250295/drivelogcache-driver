/*++

    comm.c - minifilter 用户态通信端口

    用户态通过 fltlib!FilterConnectCommunicationPort 连接 \\DlcIoPort,
    用 FilterSendMessage 下发控制消息, 用 FilterGetMessage 接收 IO 日志批。

--*/

#include "driver.h"

NTSTATUS DlcSendToApp(PVOID Buffer, ULONG Length)
{
    LARGE_INTEGER timeout;
    NTSTATUS status;

    if (g_Dlc.ClientPort == NULL || g_Dlc.FilterHandle == NULL) {
        return STATUS_PORT_DISCONNECTED;
    }

    timeout.QuadPart = -10000000LL;   /* 相对超时 1 秒 */

    status = FltSendMessage(g_Dlc.FilterHandle,
                            &g_Dlc.ClientPort,
                            Buffer, Length,
                            NULL, NULL,
                            &timeout);
    return status;
}

/*-------------------------------------------------------------------------- 
  连接 / 断开
--------------------------------------------------------------------------*/

NTSTATUS DlcConnect(
    _In_ PFLT_PORT ClientPort,
    _In_opt_ PVOID ServerPortCookie,
    _In_reads_bytes_opt_(SizeOfContext) PVOID ConnectionContext,
    _In_ ULONG SizeOfContext,
    _Outptr_result_maybenull_ PVOID *ConnectionPortCookie)
{
    UNREFERENCED_PARAMETER(ServerPortCookie);
    UNREFERENCED_PARAMETER(ConnectionContext);
    UNREFERENCED_PARAMETER(SizeOfContext);

    g_Dlc.ClientPort = ClientPort;
    *ConnectionPortCookie = NULL;
    return STATUS_SUCCESS;
}

VOID DlcDisconnect(_In_opt_ PVOID ConnectionCookie)
{
    UNREFERENCED_PARAMETER(ConnectionCookie);
    FltCloseClientPort(g_Dlc.FilterHandle, &g_Dlc.ClientPort);
    g_Dlc.ClientPort = NULL;
}

/*-------------------------------------------------------------------------- 
  应用 -> 驱动 消息处理
--------------------------------------------------------------------------*/

NTSTATUS DlcHandleMessage(
    _In_reads_bytes_(InLen) PVOID InBuffer,
    ULONG InLen,
    _Out_writes_bytes_to_opt_(OutLen, *OutLenRet) PVOID OutBuffer,
    ULONG OutLen,
    _Out_ PULONG OutLenRet)
{
    PDLC_MSG_HEADER hdr;
    NTSTATUS status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(OutBuffer);
    UNREFERENCED_PARAMETER(OutLen);

    *OutLenRet = 0;

    if (InBuffer == NULL || InLen < sizeof(DLC_MSG_HEADER)) {
        return STATUS_INVALID_PARAMETER;
    }
    hdr = (PDLC_MSG_HEADER)InBuffer;
    if (hdr->MsgLen > InLen) {
        return STATUS_INVALID_PARAMETER;
    }

    switch (hdr->MsgType) {

    case DLC_MSG_PING:
        break;

    case DLC_MSG_ADD_TARGET: {
        PDLC_TARGET_PID req = (PDLC_TARGET_PID)InBuffer;
        if (InLen < sizeof(*req)) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        DlcProcSetTarget((HANDLE)(ULONG_PTR)req->Pid, TRUE);
        break;
    }

    case DLC_MSG_CLEAR_TARGETS:
        DlcProcClearTargets();
        break;

    case DLC_MSG_SET_LOGGING: {
        PDLC_SET_LOGGING_REQ req = (PDLC_SET_LOGGING_REQ)InBuffer;
        if (InLen < sizeof(*req)) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        InterlockedExchange(&g_Dlc.LoggingEnabled, req->Enable ? 1 : 0);
        break;
    }

    case DLC_MSG_SET_LOG_MODE: {
        PDLC_SET_LOG_MODE_REQ req = (PDLC_SET_LOG_MODE_REQ)InBuffer;
        if (InLen < sizeof(*req)) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        InterlockedExchange(&g_Dlc.LogMode, (req->Mode == 1) ? 1 : 0);
        InterlockedExchange((PLONG)&g_Dlc.LogVolSerial, (LONG)req->VolSerial);
        break;
    }

    case DLC_MSG_CLEAR_CACHE:
        DlcCacheClose();
        DlcBlockClear();
        break;

    case DLC_MSG_SET_CACHE: {
        PDLC_SET_CACHE_REQ req = (PDLC_SET_CACHE_REQ)InBuffer;
        if (InLen < sizeof(*req)) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        req->Path[259] = L'\0';
        status = DlcCacheOpen(req->Path, req->BlockSize, req->BlockCount);
        break;
    }

    case DLC_MSG_SET_TABLE: {
        PDLC_SET_TABLE_REQ req = (PDLC_SET_TABLE_REQ)InBuffer;
        ULONG need;
        if (InLen < FIELD_OFFSET(DLC_SET_TABLE_REQ, Entries)) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        if (req->Count > DLC_MAX_TABLE_MSG) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        need = FIELD_OFFSET(DLC_SET_TABLE_REQ, Entries) +
               req->Count * sizeof(DLC_BLOCK_ENTRY);
        if (InLen < need) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        status = DlcBlockReplace(&req->Entries[0], req->Count, req->BlockSize);
        break;
    }

    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    return status;
}

NTSTATUS DlcMessageNotify(
    _In_ PVOID PortCookie,
    _In_reads_bytes_opt_(InputBufferLength) PVOID InputBuffer,
    _In_ ULONG InputBufferLength,
    _Out_writes_bytes_to_opt_(OutputBufferLength, *ReturnOutputBufferLength)
        PVOID OutputBuffer,
    _In_ ULONG OutputBufferLength,
    _Out_ PULONG ReturnOutputBufferLength)
{
    UNREFERENCED_PARAMETER(PortCookie);

    return DlcHandleMessage(InputBuffer, InputBufferLength,
                            OutputBuffer, OutputBufferLength,
                            ReturnOutputBufferLength);
}

/*-------------------------------------------------------------------------- 
  端口创建/关闭
--------------------------------------------------------------------------*/

NTSTATUS DlcCommCreate(PFLT_FILTER Filter)
{
    OBJECT_ATTRIBUTES oa;
    UNICODE_STRING portName;
    NTSTATUS status;
    PSECURITY_DESCRIPTOR sd = NULL;

    status = FltBuildDefaultSecurityDescriptor(&sd, FLT_PORT_ALL_ACCESS);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    RtlInitUnicodeString(&portName, DLC_PORT_NAME_W);
    InitializeObjectAttributes(&oa, &portName,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               NULL, sd);

    status = FltCreateCommunicationPort(Filter,
                                        &g_Dlc.ServerPort,
                                        &oa,
                                        NULL,
                                        DlcConnect,
                                        DlcDisconnect,
                                        DlcMessageNotify,
                                        1);

    FltFreeSecurityDescriptor(sd);

    if (!NT_SUCCESS(status)) {
        g_Dlc.ServerPort = NULL;
    }
    return status;
}

VOID DlcCommClose(void)
{
    if (g_Dlc.ClientPort != NULL) {
        FltCloseClientPort(g_Dlc.FilterHandle, &g_Dlc.ClientPort);
        g_Dlc.ClientPort = NULL;
    }
    if (g_Dlc.ServerPort != NULL) {
        FltCloseCommunicationPort(g_Dlc.ServerPort);
        g_Dlc.ServerPort = NULL;
    }
}
