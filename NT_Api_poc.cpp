#include "stdafx.h"

extern "C"
NTSYSCALLAPI
NTSTATUS
NTAPI
NtCancelIoFile(
			   _In_ HANDLE FileHandle,
			   _Out_ PIO_STATUS_BLOCK IoStatusBlock
			   );

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

	virtual void IoCompletion(ULONG opCode, NTSTATUS status, ULONG_PTR dwNumberOfBytesTransfered, PVOID Pointer) = 0;

	virtual ~IoFile()
	{
		if (_hFile) NtClose(_hFile);
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

class __declspec(novtable) UM_IRP : public IO_STATUS_BLOCK
{
	IoFile* pObj;
	PVOID Pointer;
	ULONG opCode;

	VOID IoCompletion(NTSTATUS status, ULONG_PTR dwNumberOfBytesTransfered)
	{
		DbgPrint("%x>%s<%p>(%x, %p)\n", GetCurrentThreadId(), __FUNCTION__, this, status, dwNumberOfBytesTransfered);
		pObj->IoCompletion(opCode, status, dwNumberOfBytesTransfered, Pointer);
		DeleteOrReleaseSelf();
	}

	static VOID WINAPI IoCompletionNT(
		_In_    NTSTATUS status,
		_In_    ULONG_PTR dwNumberOfBytesTransfered,
		_Inout_ PVOID ApcContext
		)
	{
		DbgPrint("%x> !! %s<%p>(%x, %p)\n", GetCurrentThreadId(), __FUNCTION__, ApcContext, status, dwNumberOfBytesTransfered);
		reinterpret_cast<UM_IRP*>(ApcContext)->IoCompletion(status, dwNumberOfBytesTransfered);
	}

protected:

	virtual ~UM_IRP()
	{
		pObj->Release();
		DbgPrint("%x>%s<%p>\n", GetCurrentThreadId(), __FUNCTION__, this);
	}

	virtual void DeleteOrReleaseSelf() = 0;
	virtual bool IsWillBeNotification(NTSTATUS status, BOOL bSkipOnSynchronous) = 0;

	UM_IRP(IoFile* pObj, ULONG opCode, PVOID Pointer = 0) : pObj(pObj), opCode(opCode), Pointer(Pointer)
	{
		pObj->AddRef();
		Status = STATUS_PENDING, Information = 0;
		DbgPrint("%x>%s<%p>\n", GetCurrentThreadId(), __FUNCTION__, this);
	}

public:

	void CheckError(NTSTATUS status, BOOL bSkipOnSynchronous)
	{
		if (!IsWillBeNotification(status, bSkipOnSynchronous))
		{
			IoCompletion(status, Information);
		}
	}

	static NTSTATUS BindIoCompletionCallback (HANDLE FileHandle)
	{
		return RtlSetIoCompletionCallback(FileHandle, IoCompletionNT, 0);
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

	virtual bool IsWillBeNotification(NTSTATUS status, BOOL bSkipOnSynchronous)
	{
		bool b = (status == STATUS_PENDING || (Status != STATUS_PENDING && !bSkipOnSynchronous));

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

	virtual bool IsWillBeNotification(NTSTATUS status, BOOL bSkipOnSynchronous)
	{
		return (status == STATUS_PENDING) || (!NT_ERROR(status) && !bSkipOnSynchronous);
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
		NTSTATUS status;
		ULONG dwThreadId, opCode;
	};
private:
	virtual ~LockTestFile()
	{
		DbgPrint("%x>%s<%p>\n", GetCurrentThreadId(), __FUNCTION__, this);
	}

	virtual void IoCompletion(ULONG opCode, NTSTATUS status, ULONG_PTR /*dwNumberOfBytesTransfered*/, PVOID Pointer)
	{
		IO_RESULT* pio = (IO_RESULT*)Pointer;
		pio->status = status;
		pio->opCode = opCode;
		PostThreadMessageW(pio->dwThreadId, WM_QUIT, 0, 0);
	}
public:
	enum { opLock = 'kcol', opUnlock = 'klnu' };

	NTSTATUS Open(POBJECT_ATTRIBUTES poa)
	{
		HANDLE hFile;
		IO_STATUS_BLOCK iosb;

		NTSTATUS status = NtOpenFile(&hFile, FILE_WRITE_DATA, poa, &iosb, FILE_SHARE_VALID_FLAGS, FILE_NON_DIRECTORY_FILE);

		if (0 <= status)
		{
			_hFile = hFile;

			status = UM_IRP::BindIoCompletionCallback(hFile);
		}

		return status;
	}

	void Init(IO_RESULT* pio)
	{
		pio->dwThreadId = GetCurrentThreadId();
		pio->status = STATUS_PENDING;
	}

	bool Lock(IO_RESULT* pio, bool bSkipOnSynchronous)
	{
		if (UM_IRP_* Irp = new UM_IRP_(this, opLock, pio))
		{
			Init(pio);

			LARGE_INTEGER ByteOffset {}, Length {1};
			// try also with FailImmediately = FALSE for compare, here no error even with UM_IRP_WRONG
			Irp->CheckError(
				NtLockFile(_hFile, 0, 0, Irp, Irp, &ByteOffset, &Length, 'keyX', TRUE, TRUE), bSkipOnSynchronous);

			return true;
		}

		return false;
	}

	NTSTATUS Unlock()
	{
		IO_STATUS_BLOCK iosb;
		LARGE_INTEGER ByteOffset {}, Length {1};
		return NtUnlockFile(_hFile, &iosb, &ByteOffset, &Length, 'keyX');
	}

	NTSTATUS CancelIo()
	{
		IO_STATUS_BLOCK iosb;
		return NtCancelIoFile(_hFile, &iosb);
	}

	LockTestFile()
	{
		DbgPrint("%x>%s<%p>\n", GetCurrentThreadId(), __FUNCTION__, this);
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

	if (ior.status == STATUS_PENDING)
	{
		if (0 > pObj->CancelIo()) __debugbreak();

		while (GetMessageW(&msg, 0, 0, 0)) continue;

		if (ior.status == STATUS_PENDING) __debugbreak();
	}

	if (0 > ior.status)
	{
		PWSTR psz;
		if (FormatMessage(FORMAT_MESSAGE_IGNORE_INSERTS|FORMAT_MESSAGE_ALLOCATE_BUFFER |
			FORMAT_MESSAGE_FROM_HMODULE, GetModuleHandle(L"ntdll"), ior.status, 
			MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),(PWSTR)&psz, 0, NULL))
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
			if (0 > pObj->Unlock()) __debugbreak();
		}
	}

	pObj->Release();

	return 0;
}

void LockPOC(POBJECT_ATTRIBUTES poa)
{
	if (LockTestFile* pObj = new LockTestFile)
	{
		if (!pObj->Open(poa))
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
