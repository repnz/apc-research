#include <Windows.h>
#include <stdio.h>
#include <winternl.h>

typedef
VOID
(*PPS_APC_ROUTINE)(
	PVOID SystemArgument1,
	PVOID SystemArgument2,
	PVOID SystemArgument3
	);

typedef
NTSTATUS
(NTAPI* PNTQUEUEAPCTHREAD)(
	HANDLE ThreadHandle,
	PPS_APC_ROUTINE ApcRoutine,
	PVOID SystemArgument1,
	PVOID SystemArgument2,
	PVOID SystemArgument3
	);


typedef enum _QUEUE_USER_APC_FLAGS {
	QueueUserApcFlagsNone,
	QueueUserApcFlagsSpecialUserApc,
	QueueUserApcFlagsMaxValue
} QUEUE_USER_APC_FLAGS;

typedef union _USER_APC_OPTION {
	ULONG_PTR UserApcFlags;
	HANDLE MemoryReserveHandle;
} USER_APC_OPTION, * PUSER_APC_OPTION;


typedef NTSTATUS
(NTAPI* PNT_QUEUE_APC_THREAD_EX)(
	IN HANDLE ThreadHandle,
	IN USER_APC_OPTION UserApcOption,
	IN PPS_APC_ROUTINE ApcRoutine,
	IN PVOID SystemArgument1 OPTIONAL,
	IN PVOID SystemArgument2 OPTIONAL,
	IN PVOID SystemArgument3 OPTIONAL
	);

typedef
NTSTATUS
(NTAPI* PNTGETNEXTTHREAD)(
	_In_ HANDLE ProcessHandle,
	_In_ HANDLE ThreadHandle,
	_In_ ACCESS_MASK DesiredAccess,
	_In_ ULONG HandleAttributes,
	_In_ ULONG Flags,
	_Out_ PHANDLE NewThreadHandle
	);


PNTGETNEXTTHREAD NtGetNextThread;
PNTQUEUEAPCTHREAD NtQueueApcThread;
PNT_QUEUE_APC_THREAD_EX NtQueueApcThreadEx;

PVOID LoadLibraryAPtr;

typedef enum _APC_TYPE {
	ApcTypeWin32,
	ApcTypeNative,
	ApcTypeSpecial
} APC_TYPE;

typedef struct _APC_INJECTOR_ARGS {
	APC_TYPE ApcType;
	PCSTR DllPath;
	ULONG ProcessId;
	ULONG ThreadId;
} APC_INJECTOR_ARGS, * PAPC_INJECTOR_ARGS;

VOID
InitializeProcedures(
	VOID
)
{
	HMODULE NtdllHandle = GetModuleHandle("ntdll.dll");
	HMODULE Kernel32Handle = GetModuleHandle("kernel32.dll");

	NtGetNextThread = (PNTGETNEXTTHREAD)GetProcAddress(NtdllHandle, "NtGetNextThread");
	NtQueueApcThread = (PNTQUEUEAPCTHREAD)GetProcAddress(NtdllHandle, "NtQueueApcThread");
	NtQueueApcThreadEx = (PNT_QUEUE_APC_THREAD_EX)GetProcAddress(NtdllHandle, "NtQueueApcThreadEx");
	LoadLibraryAPtr = GetProcAddress(Kernel32Handle, "LoadLibraryA");
}

PVOID WriteLibraryNameToRemote(
	HANDLE ProcessHandle,
	PCSTR Library
)
{
	SIZE_T LibraryLength = strlen(Library);

	PVOID LibraryRemoteAddress = VirtualAllocEx(
		ProcessHandle,
		NULL,
		LibraryLength + 1,
		MEM_RESERVE | MEM_COMMIT,
		PAGE_READWRITE
	);

	if (!LibraryRemoteAddress) {
		printf("Cannot allocate memory for library path. Error: 0x%08X\n", GetLastError());
		exit(-1);
	}

	if (!WriteProcessMemory(
		ProcessHandle,
		LibraryRemoteAddress,
		Library,
		LibraryLength + 1,
		NULL
	)) {
		printf("Cannot write library path to remote process. Error: 0x%08X\n", GetLastError());
		exit(-1);
	}

	return LibraryRemoteAddress;
}

VOID
ParseArguments(
	__in int ArgumentCount,
	__in const char** Arguments,
	__out PAPC_INJECTOR_ARGS Args)
{
	if (ArgumentCount < 4) {
		printf("Missing arguments!\n");
		printf("ApcDllInjector.exe <native/win32/special> <process_id> <dll_path> [thread_id]\n");
		exit(-1);
	}

	Args->ProcessId = atoi(Arguments[2]);
	Args->DllPath = Arguments[3];
	Args->ThreadId = 0;
	Args->ApcType = ApcTypeWin32;

	if (ArgumentCount > 4) {
		Args->ThreadId = atoi(Arguments[4]);
	}

	if (strcmp(Arguments[1], "native") == 0) {
		Args->ApcType = ApcTypeNative;
	}
	else if (strcmp(Arguments[1], "win32") == 0) {
		Args->ApcType = ApcTypeWin32;
	}
	else if (strcmp(Arguments[1], "special") == 0) {
		Args->ApcType = ApcTypeSpecial;
	}
	else {
		printf("Invalid injection mode '%s'\n", Arguments[1]);
		exit(-1);
	}
}

int main(int argc, const char** argv) {

	APC_INJECTOR_ARGS Args;
	NTSTATUS Status;
	HANDLE ThreadHandle;
	HANDLE ProcessHandle;

	InitializeProcedures();

	ParseArguments(argc, argv, &Args);

	ProcessHandle = OpenProcess(
		PROCESS_QUERY_INFORMATION |
		PROCESS_VM_OPERATION |
		PROCESS_VM_WRITE,
		FALSE,
		Args.ProcessId
	);

	if (!ProcessHandle) {
		printf("Cannot open process handle: 0x%08X\n", GetLastError());
		exit(-1);
	}

	PVOID RemoteLibraryAddress = WriteLibraryNameToRemote(ProcessHandle, Args.DllPath);

	if (Args.ThreadId) {
		ThreadHandle = OpenThread(THREAD_SET_CONTEXT, FALSE, Args.ThreadId);

		if (!ThreadHandle) {
			printf("Cannt open thread handle 0x%08X\n", GetLastError());
			exit(-1);
		}
	}
	else {
		Status = NtGetNextThread(
			ProcessHandle,
			NULL,
			THREAD_SET_CONTEXT,
			0,
			0,
			&ThreadHandle
		);
	}

	if (!NT_SUCCESS(Status)) {
		printf("Cannot open thread handle 0x%08X\n", Status);
		exit(-1);
		return -1;
	}

	switch (Args.ApcType) {
	case ApcTypeWin32: {
		if (!QueueUserAPC((PAPCFUNC)LoadLibraryAPtr, ThreadHandle, (ULONG_PTR)RemoteLibraryAddress)) {
			printf("QueueUserAPC Error! 0x%08X\n", GetLastError());
			exit(-1);
		}
	}
					   break;
	case ApcTypeNative: {
		Status = NtQueueApcThread(
			ThreadHandle,
			LoadLibraryAPtr,
			RemoteLibraryAddress,
			NULL,
			NULL
		);

		if (!NT_SUCCESS(Status)) {
			printf("NtQueueApcThread Failed: 0x%08X\n", Status);
			exit(-1);
		}
	}
						break;
	case ApcTypeSpecial: {
		USER_APC_OPTION UserApcOption;
		UserApcOption.UserApcFlags = QueueUserApcFlagsSpecialUserApc;

		Status = NtQueueApcThreadEx(
			ThreadHandle,
			UserApcOption,
			LoadLibraryAPtr,
			RemoteLibraryAddress,
			NULL,
			NULL
		);

		if (!NT_SUCCESS(Status)) {
			printf("NtQueueApcThreadEx Failed: 0x%08X\n", Status);
			exit(-1);
		}
	}
						 break;
	default:
		break;
	}

	return 0;
}