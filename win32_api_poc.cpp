#include "stdafx.h"

extern "C"
{
	NTSYSAPI ULONG __cdecl DbgPrint (PCSTR Format, ...);

	NTSYSAPI ULONG NTAPI RtlNtStatusToDosError (_In_ NTSTATUS Status);
}

#if 1 //1 //0
#define UM_IRP_ UM_IRP_WRONG
#else
#define UM_IRP_ UM_IRP_OK
#endif

class UM_IRP;

class __declspec(novtable) IoFile
{
	friend UM_IRP;
protected:
	HANDLE _hFile;
private:
	LONG _dwRefCount;

protected:

	virtual void IoCompletion(ULONG opCode, DWORD dwErrorCode, ULONG_PTR dwNumberOfBytesTransfered, PVOID Pointer) = 0;

	virtual ~IoFile()
	{
		if (_hFile) CloseHandle(_hFile);
	}

	IoFile() : _dwRefCount(1), _hFile(0)
	{ 
	}

public:
	void AddRef()
	{
		InterlockedIncrementNoFence(&_dwRefCount);
	}

	void Release()
	{
		if (!InterlockedDecrement(&_dwRefCount)) delete this;
	}
};

class __declspec(novtable) UM_IRP : public OVERLAPPED
{
	IoFile* pObj;
	PVOID Pointer;
	ULONG opCode;

	VOID IoCompletionWin32(ULONG dwErrorCode, ULONG_PTR dwNumberOfBytesTransfered)
	{
		DbgPrint("%x>%s<%p>(%x, %p)\n", GetCurrentThreadId(), __FUNCTION__, this, dwErrorCode, dwNumberOfBytesTransfered);
		pObj->IoCompletion(opCode, dwErrorCode, dwNumberOfBytesTransfered, Pointer);
		DeleteOrReleaseSelf();
	}

	static VOID WINAPI IoCompletionNT(
		_In_    NTSTATUS status,
		_In_    ULONG_PTR dwNumberOfBytesTransfered,
		_Inout_ LPOVERLAPPED lpOverlapped
		)
	{
		DbgPrint("%x> !! %s<%p>(%x, %p)\n", GetCurrentThreadId(), __FUNCTION__, static_cast<UM_IRP*>(lpOverlapped), status, dwNumberOfBytesTransfered);
		static_cast<UM_IRP*>(lpOverlapped)->IoCompletionWin32(RtlNtStatusToDosError(status), dwNumberOfBytesTransfered);
	}

protected:

	virtual ~UM_IRP()
	{
		pObj->Release();
		DbgPrint("%x>%s<%p>\n", GetCurrentThreadId(), __FUNCTION__, this);
	}

	virtual void DeleteOrReleaseSelf() = 0;
	virtual bool IsWillBeNotification(ULONG dwErrorCode, BOOL bSkipOnSynchronous) = 0;

	UM_IRP(IoFile* pObj, ULONG opCode, PVOID Pointer = 0) : pObj(pObj), opCode(opCode), Pointer(Pointer)
	{
		pObj->AddRef();
		// Status = STATUS_PENDING, Information = 0;
		Internal = STATUS_PENDING, InternalHigh = 0;
		hEvent = 0, Offset = 0, OffsetHigh = 0;
		DbgPrint("%x>%s<%p>\n", GetCurrentThreadId(), __FUNCTION__, this);
	}

public:

	void CheckError(BOOL fOk, BOOL bSkipOnSynchronous)
	{
		CheckError(fOk ? NOERROR : GetLastError(), bSkipOnSynchronous);
	}

	void CheckError(ULONG dwErrorCode, BOOL bSkipOnSynchronous)
	{
		if (!IsWillBeNotification(dwErrorCode, bSkipOnSynchronous))
		{
			IoCompletionWin32(dwErrorCode, InternalHigh);
		}
	}

	static BOOL BindIoCompletionCallback (HANDLE FileHandle)
	{
		return ::BindIoCompletionCallback(FileHandle, (LPOVERLAPPED_COMPLETION_ROUTINE)IoCompletionNT, 0);
	}
};

class UM_IRP_OK : public UM_IRP
{
	LONG dwRefCount;

	void Release()
	{
		if (!InterlockedDecrement(&dwRefCount)) delete this;
	}

	virtual void DeleteOrReleaseSelf()
	{
		Release();
	}

	virtual bool IsWillBeNotification(ULONG dwErrorCode, BOOL bSkipOnSynchronous)
	{
		bool b = (dwErrorCode == ERROR_IO_PENDING || (Internal != STATUS_PENDING && !bSkipOnSynchronous));

		Release();

		return b;
	}

public:
	UM_IRP_OK(IoFile* pObj, ULONG opCode, PVOID Pointer = 0) : UM_IRP(pObj, opCode, Pointer), dwRefCount(2)
	{
	}
};

class UM_IRP_WRONG : public UM_IRP
{
	virtual void DeleteOrReleaseSelf()
	{
		delete this;
	}

	virtual bool IsWillBeNotification(ULONG dwErrorCode, BOOL bSkipOnSynchronous)
	{
		switch (dwErrorCode)
		{
		case ERROR_IO_PENDING:
			return true;
		case NOERROR:
			return !bSkipOnSynchronous;
		default:
			return false;
		}
	}
public:
	UM_IRP_WRONG(IoFile* pObj, ULONG opCode, PVOID Pointer = 0) : UM_IRP(pObj, opCode, Pointer)
	{
	}
};

class LockTestFile : public IoFile
{
public:
	struct IO_RESULT 
	{
		ULONG dwThreadId, opCode, dwErrorCode;
	};
private:
	virtual ~LockTestFile()
	{
		DbgPrint("%x>%s<%p>\n", GetCurrentThreadId(), __FUNCTION__, this);
	}

	virtual void IoCompletion(ULONG opCode, ULONG dwErrorCode, ULONG_PTR /*dwNumberOfBytesTransfered*/, PVOID Pointer)
	{
		IO_RESULT* pio = (IO_RESULT*)Pointer;
		pio->dwErrorCode = dwErrorCode;
		pio->opCode = opCode;
		PostThreadMessageW(pio->dwThreadId, WM_QUIT, 0, 0);
	}
public:
	enum { opLock = 'kcol' };

	LockTestFile()
	{
		DbgPrint("%x>%s<%p>\n", GetCurrentThreadId(), __FUNCTION__, this);
	}

	ULONG Open(PCWSTR pszFileName)
	{
		HANDLE hFile = CreateFileW(pszFileName, FILE_WRITE_DATA, FILE_SHARE_WRITE|FILE_SHARE_READ, 0, OPEN_EXISTING,
			FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OVERLAPPED, 0);

		if (hFile != INVALID_HANDLE_VALUE)
		{
			_hFile = hFile;

			if (UM_IRP::BindIoCompletionCallback(hFile))
			{
				return NOERROR;
			}
		}

		return GetLastError();
	}

	void Init(IO_RESULT* pio)
	{
		pio->dwThreadId = GetCurrentThreadId();
		pio->dwErrorCode = ERROR_IO_PENDING;
	}

	bool Lock(IO_RESULT* pio, bool bSkipOnSynchronous)
	{
		if (UM_IRP_* lpOverlapped = new UM_IRP_(this, opLock, pio))
		{
			Init(pio);

			// try also with LOCKFILE_EXCLUSIVE_LOCK only for compare, here no error even with UM_IRP_WRONG
			lpOverlapped->CheckError(//
				LockFileEx(_hFile, LOCKFILE_EXCLUSIVE_LOCK|LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, lpOverlapped), bSkipOnSynchronous);

			return true;
		}

		return false;
	}

	BOOL Unlock()
	{
		OVERLAPPED ov{};
		return UnlockFileEx(_hFile, 0, 1, 0, &ov);
	}

	BOOL CancelIo()
	{
		return ::CancelIo(_hFile);
	}
};

ULONG WINAPI TestLockThread(LockTestFile* pObj)
{
	WCHAR caption[64];
	swprintf_s(caption, L"[%x]", GetCurrentThreadId());

	//MessageBoxW(0, L"Before Test", caption, MB_OK);

	LockTestFile::IO_RESULT ior;

	pObj->Lock(&ior, false);

	MessageBoxW(0, L"Waiting For Lock...", caption, MB_OK);

	MSG msg;

	if (ior.dwErrorCode == ERROR_IO_PENDING)
	{
		if (!pObj->CancelIo()) __debugbreak();

		while (GetMessageW(&msg, 0, 0, 0)) continue;

		if (ior.dwErrorCode == ERROR_IO_PENDING) __debugbreak();
	}

	if (ior.dwErrorCode != NOERROR)
	{
		PWSTR psz;
		if (FormatMessage(FORMAT_MESSAGE_IGNORE_INSERTS|FORMAT_MESSAGE_ALLOCATE_BUFFER |
			FORMAT_MESSAGE_FROM_SYSTEM, 0, ior.dwErrorCode, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
			(PWSTR)&psz, 0, NULL))
		{
			DbgPrint("%x> %S\n", GetCurrentThreadId(), psz);
			while (PeekMessageW(&msg, 0, 0, 0, PM_REMOVE)) continue;//for remove WM_QUIT
			MessageBoxW(0, psz, caption, MB_ICONWARNING);
			LocalFree(psz);
		}
	}
	else
	{
		switch (ior.opCode)
		{
		case LockTestFile::opLock:

			while (PeekMessageW(&msg, 0, 0, 0, PM_REMOVE)) continue;//for remove WM_QUIT

			MessageBoxW(0, L"Lock Held!", caption, MB_OK);

			DbgPrint("\n======== unlock ======\n");
			if (!pObj->Unlock()) __debugbreak();
		}
	}

	pObj->Release();

	return 0;
}

void LockPOC(PCWSTR szfile)
{
	if (LockTestFile* pObj = new LockTestFile)
	{
		if (!pObj->Open(szfile))
		{
			int n = 2;
			do 
			{
				pObj->AddRef();
				if (HANDLE hThread = CreateThread(0, 0, (PTHREAD_START_ROUTINE)TestLockThread, pObj, 0, 0))
				{
					CloseHandle(hThread);
				}
				else
				{
					pObj->Release();
				}
			} while (--n);
		}
		pObj->Release();
	}

	MessageBoxW(0, 0, L"POC", MB_OK);
}
