#include "vfslist.h"
#include "ob/ob.h"
#include "charutil.h"

// Context for VFS list caching and settings
typedef struct tdVFSLIST_CONTEXT {
	QWORD qwCacheValidMs;
	FILETIME ftDefaultTime;
	time_t time_default;
	POB_CACHEMAP pcm;
	VFS_LIST_U_PFN pfnVfsListU;
	BOOL fSingleThread;
	CRITICAL_SECTION Lock;
} VFSLIST_CONTEXT, * PVFSLIST_CONTEXT;

VFSLIST_CONTEXT g_ctxVfsList = { 0 };

// Increase chunk size to 64 to reduce memory allocation overhead.
#define VFSLIST_CONFIG_FILELIST_ITEMS   8192
#define VFSLIST_CONFIG_FILELIST_MAGIC   0x7f646555caffee66

typedef struct tdVFSLIST_DIRECTORY {
	QWORD magic;
	struct tdVFSLIST_DIRECTORY* FLink;
	DWORD cFiles;
	VFS_ENTRY pFilesU[VFSLIST_CONFIG_FILELIST_ITEMS];
} VFSLIST_DIRECTORY, * PVFSLIST_DIRECTORY;

typedef struct tdVFSLISTOB_DIRECTORY {
	OB ObHdr;
	QWORD tc64;
	QWORD qwHash;
	VFSLIST_DIRECTORY Dir;
} VFSLISTOB_DIRECTORY, * PVFSLISTOB_DIRECTORY;

// Cleanup callback to free chained directory chunks
VOID VfsList_CallbackCleanup_ObDirectory(PVFSLISTOB_DIRECTORY pObDir)
{
	PVFSLIST_DIRECTORY pDirNext, pDir = pObDir->Dir.FLink;
	while (pDir) {
		pDirNext = pDir->FLink;
		LocalFree(pDir);
		pDir = pDirNext;
	}
}

#define VFSLIST_ASCII "________________________________ !_#$%&'()_+,-._0123456789_;_=__@ABCDEFGHIJKLMNOPQRSTUVWXYZ[_]^_`abcdefghijklmnopqrstuvwxyz{_}~ "

// Optimized function to add a file (or directory) entry into the file list.
VOID VfsList_AddDirectoryFileInternal(_Inout_ PVFSLIST_DIRECTORY pFileList,
	_In_ DWORD dwFileAttributes, _In_ FILETIME ftCreationTime,
	_In_ FILETIME ftLastAccessTime, _In_ FILETIME ftLastWriteTime,
	_In_ QWORD cbFileSize, _In_ LPCSTR uszName)
{
	DWORD i = 0;
	PVFS_ENTRY pe;

	// Traverse to the chunk that has available space.
	while (pFileList->cFiles >= VFSLIST_CONFIG_FILELIST_ITEMS) {
		if (!pFileList->FLink) {
			pFileList->FLink = LocalAlloc(LMEM_ZEROINIT, sizeof(VFSLIST_DIRECTORY));
			if (!pFileList->FLink)
				return;
		}
		pFileList = pFileList->FLink;
	}

	// Get pointer to the next file entry and increment count.
	pe = &pFileList->pFilesU[pFileList->cFiles];
	pFileList->cFiles++;

	// Fill in entry fields.
	pe->fDirectory = (dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? TRUE : FALSE;
	pe->dwFileAttributes = dwFileAttributes;
	pe->ftCreationTime = ftCreationTime;
	pe->ftLastAccessTime = ftLastAccessTime;
	pe->ftLastWriteTime = ftLastWriteTime;
	pe->cbFileSize = cbFileSize;

	// Combined conversion and ASCII remapping.
	{
		size_t maxLen = sizeof(pe->uszName) - 1;
		size_t j = 0;
		while (j < maxLen && uszName[j] != '\0') {
			char c = uszName[j];
			pe->uszName[j] = (c < 128) ? VFSLIST_ASCII[(unsigned char)c] : c;
			j++;
		}
		pe->uszName[j] = '\0';
	}
}

VOID VfsList_AddFile(_Inout_ HANDLE hFileList, _In_ LPCSTR uszName, _In_ QWORD cb,
	_In_opt_ PVMMDLL_VFS_FILELIST_EXINFO pExInfo)
{
	PVFSLIST_DIRECTORY pFileList2 = (PVFSLIST_DIRECTORY)hFileList;
	BOOL fExInfo = pExInfo && (pExInfo->dwVersion == VMMDLL_VFS_FILELIST_EXINFO_VERSION);
	if (pFileList2 && (pFileList2->magic == VFSLIST_CONFIG_FILELIST_MAGIC)) {
		VfsList_AddDirectoryFileInternal(
			pFileList2,
			FILE_ATTRIBUTE_NOT_CONTENT_INDEXED | ((fExInfo && pExInfo->fCompressed) ? FILE_ATTRIBUTE_COMPRESSED : 0),
			(fExInfo && pExInfo->qwCreationTime) ? pExInfo->ftCreationTime : g_ctxVfsList.ftDefaultTime,
			(fExInfo && pExInfo->qwLastAccessTime) ? pExInfo->ftLastAccessTime : g_ctxVfsList.ftDefaultTime,
			(fExInfo && pExInfo->qwLastWriteTime) ? pExInfo->ftLastWriteTime : g_ctxVfsList.ftDefaultTime,
			cb,
			uszName
		);
	}
}

VOID VfsList_AddDirectory(_Inout_ HANDLE hFileList, _In_ LPCSTR uszName,
	_In_opt_ PVMMDLL_VFS_FILELIST_EXINFO pExInfo)
{
	PVFSLIST_DIRECTORY pFileList2 = (PVFSLIST_DIRECTORY)hFileList;
	BOOL fExInfo = pExInfo && (pExInfo->dwVersion == VMMDLL_VFS_FILELIST_EXINFO_VERSION);
	if (pFileList2 && (pFileList2->magic == VFSLIST_CONFIG_FILELIST_MAGIC)) {
		VfsList_AddDirectoryFileInternal(
			pFileList2,
			FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED | ((fExInfo && pExInfo->fCompressed) ? FILE_ATTRIBUTE_COMPRESSED : 0),
			(fExInfo && pExInfo->qwCreationTime) ? pExInfo->ftCreationTime : g_ctxVfsList.ftDefaultTime,
			(fExInfo && pExInfo->qwLastAccessTime) ? pExInfo->ftLastAccessTime : g_ctxVfsList.ftDefaultTime,
			(fExInfo && pExInfo->qwLastWriteTime) ? pExInfo->ftLastWriteTime : g_ctxVfsList.ftDefaultTime,
			0,
			uszName
		);
	}
}

PVFSLISTOB_DIRECTORY VfsList_GetDirectory(_In_ LPSTR uszPath)
{
	QWORD i = 0, qwHash;
	PVFSLISTOB_DIRECTORY pObDir = NULL;
	VMMDLL_VFS_FILELIST2 VfsFileList;
	CHAR c, uszPathCopy[3 * MAX_PATH];
	// 1: Try to fetch from cache.
	qwHash = CharUtil_HashPathFsU(uszPath);
	if ((pObDir = ObCacheMap_GetByKey(g_ctxVfsList.pcm, qwHash))) {
		return pObDir;
	}
	if (g_ctxVfsList.fSingleThread) {
		EnterCriticalSection(&g_ctxVfsList.Lock);
		if ((pObDir = ObCacheMap_GetByKey(g_ctxVfsList.pcm, qwHash))) {
			LeaveCriticalSection(&g_ctxVfsList.Lock);
			return pObDir;
		}
	}
	// 2: Replace forward slashes with backward slashes for compatibility.
	strncpy_s(uszPathCopy, sizeof(uszPathCopy), uszPath, _TRUNCATE);
	while ((c = uszPathCopy[i++])) {
		if (c == '/') { uszPathCopy[i - 1] = '\\'; }
	}
	// 3: Create a new directory object.
	pObDir = Ob_Alloc('VFSD', LMEM_ZEROINIT, sizeof(VFSLISTOB_DIRECTORY),
		(VOID(*)(PVOID))VfsList_CallbackCleanup_ObDirectory, NULL);
	if (!pObDir) { goto fail; }
	pObDir->Dir.magic = VFSLIST_CONFIG_FILELIST_MAGIC;
	VfsFileList.dwVersion = VMMDLL_VFS_FILELIST_VERSION;
	VfsFileList.h = (HANDLE)&pObDir->Dir;
	VfsFileList.pfnAddFile = VfsList_AddFile;
	VfsFileList.pfnAddDirectory = VfsList_AddDirectory;
	if (g_ctxVfsList.pfnVfsListU(uszPathCopy, &VfsFileList)) {
		pObDir->tc64 = GetTickCount64();
		pObDir->qwHash = qwHash;
		ObCacheMap_Push(g_ctxVfsList.pcm, qwHash, pObDir, 0);
		if (g_ctxVfsList.fSingleThread) { LeaveCriticalSection(&g_ctxVfsList.Lock); }
		return pObDir;
	}
fail:
	if (g_ctxVfsList.fSingleThread) { LeaveCriticalSection(&g_ctxVfsList.Lock); }
	Ob_DECREF(pObDir);
	return NULL;
}

BOOL VfsList_ListDirectory(_In_ LPSTR uszPath, _In_opt_ PVOID ctx,
	_In_opt_ PFN_VFSLIST_CALLBACK pfnListCallback)
{
	DWORD i;
	PVFSLIST_DIRECTORY pDir;
	PVFSLISTOB_DIRECTORY pObDir;
	if (!(pObDir = VfsList_GetDirectory(uszPath))) { return FALSE; }
	if (pfnListCallback) {
		pDir = &pObDir->Dir;
		while (pDir) {
			for (i = 0; i < pDir->cFiles; i++) {
				pfnListCallback(&pDir->pFilesU[i], ctx);
			}
			pDir = pDir->FLink;
		}
	}
	Ob_DECREF(pObDir);
	return TRUE;
}

BOOL VfsList_GetSingle(_In_ LPSTR uszPath, _In_ LPSTR uszFile,
	_Out_ PVFS_ENTRY pVfsEntry, _Out_ PBOOL pfPathValid)
{
	DWORD i;
	PVFSLIST_DIRECTORY pDir;
	PVFSLISTOB_DIRECTORY pObDir;
	*pfPathValid = FALSE;
	if ((pObDir = VfsList_GetDirectory(uszPath))) {
		*pfPathValid = TRUE;
		pDir = &pObDir->Dir;
		while (pDir) {
			for (i = 0; i < pDir->cFiles; i++) {
				if (!_stricmp(uszFile, pDir->pFilesU[i].uszName)) {
					memcpy(pVfsEntry, &pDir->pFilesU[i], sizeof(VFS_ENTRY));
					Ob_DECREF(pObDir);
					return TRUE;
				}
			}
			pDir = pDir->FLink;
		}
		Ob_DECREF(pObDir);
	}
	return FALSE;
}

VOID VfsList_Clear(_In_ LPSTR uszPath)
{
	CHAR uszPathSplit[3 * MAX_PATH] = { 0 };
	CharUtil_PathSplitLastEx(uszPath, uszPathSplit, _countof(uszPathSplit));
	Ob_DECREF(ObCacheMap_RemoveByKey(g_ctxVfsList.pcm, CharUtil_HashPathFsU(uszPath)));
	Ob_DECREF(ObCacheMap_RemoveByKey(g_ctxVfsList.pcm, CharUtil_HashPathFsU(uszPathSplit)));
}

#ifdef _WIN32
BOOL VfsList_EntryUtoW(_In_ PVFS_ENTRY peVfs, _Out_ PWIN32_FIND_DATAW pFindData)
{
	pFindData->dwFileAttributes = peVfs->dwFileAttributes;
	pFindData->ftCreationTime = peVfs->ftCreationTime;
	pFindData->ftLastAccessTime = peVfs->ftLastAccessTime;
	pFindData->ftLastWriteTime = peVfs->ftLastWriteTime;
	pFindData->nFileSizeHigh = (DWORD)(peVfs->cbFileSize >> 32);
	pFindData->nFileSizeLow = (DWORD)(peVfs->cbFileSize);
	CharUtil_UtoW(peVfs->uszName, -1, (PBYTE)pFindData->cFileName,
		sizeof(pFindData->cFileName), NULL, NULL,
		CHARUTIL_FLAG_TRUNCATE_ONFAIL_NULLSTR | CHARUTIL_FLAG_STR_BUFONLY);
	return TRUE;
}

BOOL VfsList_GetSingleW(_In_ LPWSTR wszPath, _In_ LPWSTR wszFile,
	_Out_ PWIN32_FIND_DATAW pFindData, _Out_ PBOOL pfPathValid)
{
	VFS_ENTRY eVfs;
	LPSTR uszPath, uszFile;
	BYTE pbBuffer1[2 * MAX_PATH], pbBuffer2[MAX_PATH];
	if (!CharUtil_WtoU(wszPath, -1, pbBuffer1, sizeof(pbBuffer1), &uszPath, NULL, 0)) { return FALSE; }
	if (!CharUtil_WtoU(wszFile, -1, pbBuffer2, sizeof(pbBuffer2), &uszFile, NULL, 0)) { return FALSE; }
	if (!VfsList_GetSingle(uszPath, uszFile, &eVfs, pfPathValid)) { return FALSE; }
	return VfsList_EntryUtoW(&eVfs, pFindData);
}

BOOL VfsList_ListDirectoryW(_In_ LPWSTR wszPath, _In_opt_ PVOID ctx,
	_In_opt_ PFN_VFSLISTW_CALLBACK pfnListCallback)
{
	DWORD i;
	PVFSLIST_DIRECTORY pDir;
	PVFSLISTOB_DIRECTORY pObDir;
	WIN32_FIND_DATAW eFindData = { 0 };
	LPSTR uszPath;
	BYTE pbBuffer[3 * MAX_PATH];
	if (!CharUtil_WtoU(wszPath, -1, pbBuffer, sizeof(pbBuffer), &uszPath, NULL, 0)) { return FALSE; }
	if (!(pObDir = VfsList_GetDirectory(uszPath))) { return FALSE; }
	if (pfnListCallback) {
		pDir = &pObDir->Dir;
		while (pDir) {
			for (i = 0; i < pDir->cFiles; i++) {
				VfsList_EntryUtoW(&pDir->pFilesU[i], &eFindData);
				pfnListCallback(&eFindData, ctx);
			}
			pDir = pDir->FLink;
		}
	}
	Ob_DECREF(pObDir);
	return TRUE;
}
#endif /* _WIN32 */

BOOL VfsList_ValidEntry(_In_ VMM_HANDLE hVMM, _Inout_ PQWORD qwContext,
	_In_ QWORD qwKey, _In_ PVFSLISTOB_DIRECTORY pvObject)
{
	UNREFERENCED_PARAMETER(qwContext);
	UNREFERENCED_PARAMETER(qwKey);
	return pvObject->tc64 + g_ctxVfsList.qwCacheValidMs > GetTickCount64();
}

VOID VfsList_Close()
{
	Ob_DECREF(g_ctxVfsList.pcm);
	if (g_ctxVfsList.fSingleThread) {
		DeleteCriticalSection(&g_ctxVfsList.Lock);
	}
	ZeroMemory(&g_ctxVfsList, sizeof(VFSLIST_CONTEXT));
}

BOOL VfsList_Initialize(_In_ VFS_LIST_U_PFN pfnVfsListU, _In_ DWORD dwCacheValidMs,
	_In_ DWORD cCacheMaxEntries, _In_ BOOL fSingleThread)
{
	g_ctxVfsList.pcm = ObCacheMap_New(
		NULL,
		cCacheMaxEntries,
		(OB_CACHEMAP_VALIDENTRY_PFN_CB)VfsList_ValidEntry,
		OB_CACHEMAP_FLAGS_OBJECT_OB
	);
	if (!g_ctxVfsList.pcm) { return FALSE; }
	g_ctxVfsList.qwCacheValidMs = dwCacheValidMs;
	g_ctxVfsList.pfnVfsListU = pfnVfsListU;
	if (fSingleThread) {
		InitializeCriticalSection(&g_ctxVfsList.Lock);
		g_ctxVfsList.fSingleThread = TRUE;
	}
#ifdef _WIN32
	SYSTEMTIME SystemTimeNow;
	GetSystemTime(&SystemTimeNow);
	SystemTimeToFileTime(&SystemTimeNow, &g_ctxVfsList.ftDefaultTime);
#else
	g_ctxVfsList.ftDefaultTime = (time(NULL) * 10000000) + 116444736000000000;
#endif /* _WIN32 */
	return TRUE;
}
