/*++

    cachemap.c - 将应用层创建的 cache.dat 映射到系统空间

    驱动通过 section 对象把整个缓存文件映射到内核系统地址空间,
    PreRead 命中时直接从映射内存 memcpy, 任意 IRQL 均可访问。

--*/

#include "driver.h"

static PVOID g_CacheSectionObj = NULL;

VOID DlcCacheClose(void)
{
    if (g_Dlc.CacheBase != NULL) {
        MmUnmapViewInSystemSpace(g_Dlc.CacheBase);
        g_Dlc.CacheBase = NULL;
    }
    if (g_CacheSectionObj != NULL) {
        ObDereferenceObject(g_CacheSectionObj);
        g_CacheSectionObj = NULL;
    }
    g_Dlc.CacheSize = 0;
    g_Dlc.BlockSize = 0;
}

NTSTATUS DlcCacheOpen(_In_ PCWSTR DosPath, ULONG BlockSize, ULONG BlockCount)
{
    NTSTATUS status;
    HANDLE hFile = NULL;
    HANDLE hSection = NULL;
    OBJECT_ATTRIBUTES oa;
    UNICODE_STRING ntPath;
    WCHAR pathBuf[280];
    IO_STATUS_BLOCK iosb;
    PVOID sectionObj = NULL;
    PVOID base = NULL;
    SIZE_T viewSize;
    LARGE_INTEGER fileSize;

    UNREFERENCED_PARAMETER(BlockCount);

    if (DosPath == NULL || DosPath[0] == L'\0' || BlockSize == 0 ||
        BlockSize % 4096 != 0) {
        return STATUS_INVALID_PARAMETER;
    }

    /* 先关闭旧映射 */
    DlcCacheClose();
    DlcBlockClear();

    /* DOS 路径 -> NT 路径 (\??\C:\...) */
    RtlZeroMemory(pathBuf, sizeof(pathBuf));
    if (DosPath[0] == L'\\') {
        RtlStringCchCopyW(pathBuf, 280, DosPath);
    } else {
        RtlStringCchCopyW(pathBuf, 280, L"\\??\\");
        RtlStringCchCatW(pathBuf, 280, DosPath);
    }
    RtlInitUnicodeString(&ntPath, pathBuf);

    InitializeObjectAttributes(&oa, &ntPath,
                               OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
                               NULL, NULL);

    status = ZwCreateFile(&hFile,
                          FILE_READ_DATA | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                          &oa, &iosb, NULL,
                          FILE_ATTRIBUTE_NORMAL,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                          FILE_OPEN,
                          FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT,
                          NULL, 0);
    if (!NT_SUCCESS(status)) {
        goto out;
    }

    /* 查询文件大小用于映射长度 */
    {
        FILE_STANDARD_INFORMATION stdInfo;
        ULONG ret;
        RtlZeroMemory(&stdInfo, sizeof(stdInfo));
        status = ZwQueryInformationFile(hFile, &iosb, &stdInfo, sizeof(stdInfo),
                                        FileStandardInformation);
        if (!NT_SUCCESS(status)) {
            goto out;
        }
        fileSize = stdInfo.EndOfFile;
    }

    if (fileSize.QuadPart < (LONGLONG)BlockSize) {
        status = STATUS_FILE_INVALID;
        goto out;
    }

    status = ZwCreateSection(&hSection, SECTION_ALL_ACCESS, NULL, &fileSize,
                             PAGE_READONLY, SEC_COMMIT, hFile);
    if (!NT_SUCCESS(status)) {
        goto out;
    }

    status = ObReferenceObjectByHandle(hSection, SECTION_ALL_ACCESS,
                                       (POBJECT_TYPE)MmSectionObjectType,
                                       KernelMode, &sectionObj, NULL);
    if (!NT_SUCCESS(status)) {
        sectionObj = NULL;
        goto out;
    }

    viewSize = (SIZE_T)fileSize.QuadPart;
    base = NULL;
    status = MmMapViewInSystemSpace(sectionObj, &base, &viewSize);
    if (!NT_SUCCESS(status)) {
        base = NULL;
        goto out;
    }

    g_Dlc.CacheBase = base;
    g_Dlc.CacheSize = viewSize;
    g_Dlc.BlockSize = BlockSize;
    g_CacheSectionObj = sectionObj;
    sectionObj = NULL;   /* 已持有引用 */
    status = STATUS_SUCCESS;

out:
    if (sectionObj != NULL) {
        ObDereferenceObject(sectionObj);
    }
    if (hSection != NULL) {
        ZwClose(hSection);
    }
    if (hFile != NULL) {
        ZwClose(hFile);
    }
    if (!NT_SUCCESS(status)) {
        DlcCacheClose();
    }
    return status;
}
