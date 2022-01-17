#include "Emulator/VirtualMemory.h"

#include "Kyty/Core/DbgAssert.h"

#include "Emulator/Common.h"
#include "Emulator/Jit.h"
#include "Emulator/Profiler.h"

#include <new>

// NOLINTNEXTLINE
//#define NTDDI_VERSION 0x0A000005

#include <windows.h> // IWYU pragma: keep

// IWYU pragma: no_include <minwindef.h>
// IWYU pragma: no_include <sysinfoapi.h>
// IWYU pragma: no_include <memoryapi.h>
// IWYU pragma: no_include <errhandlingapi.h>
// IWYU pragma: no_include <processthreadsapi.h>
// IWYU pragma: no_include <basetsd.h>
// IWYU pragma: no_include <excpt.h>
// IWYU pragma: no_include <wtypes.h>
// IWYU pragma: no_include <minwinbase.h>
// IWYU pragma: no_include <apisetcconv.h>

//#include <memoryapi.h>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Loader {

SystemInfo GetSystemInfo()
{
	SystemInfo ret {};

	SYSTEM_INFO system_info;
	GetSystemInfo(&system_info);

	switch (system_info.wProcessorArchitecture)
	{
		case PROCESSOR_ARCHITECTURE_AMD64: ret.ProcessorArchitecture = ProcessorArchitecture::Amd64; break;
		case PROCESSOR_ARCHITECTURE_UNKNOWN:
		default: ret.ProcessorArchitecture = ProcessorArchitecture::Unknown;
	}

	ret.PageSize                  = system_info.dwPageSize;
	ret.MinimumApplicationAddress = reinterpret_cast<uintptr_t>(system_info.lpMinimumApplicationAddress);
	ret.MaximumApplicationAddress = reinterpret_cast<uintptr_t>(system_info.lpMaximumApplicationAddress);
	ret.ActiveProcessorMask       = system_info.dwActiveProcessorMask;
	ret.NumberOfProcessors        = system_info.dwNumberOfProcessors;
	ret.ProcessorLevel            = system_info.wProcessorLevel;
	ret.ProcessorRevision         = system_info.wProcessorRevision;

	return ret;
}

namespace VirtualMemory {

class ExceptionHandlerPrivate
{
public:
#pragma pack(1)

	struct UnwindInfo
	{
		uint8_t Version : 3;
		uint8_t Flags   : 5;
		uint8_t SizeOfProlog;
		uint8_t CountOfCodes;
		uint8_t FrameRegister : 4;
		uint8_t FrameOffset   : 4;
		ULONG   ExceptionHandler;

		ExceptionHandlerPrivate* ExceptionData;
	};

	struct HandlerInfo
	{
		Jit::JmpRax      code;
		RUNTIME_FUNCTION function_table = {};
		UnwindInfo       unwind_info    = {};
	};

#pragma pack()

	static EXCEPTION_DISPOSITION Handler(PEXCEPTION_RECORD   exception_record, ULONG64 /*EstablisherFrame*/, PCONTEXT /*ContextRecord*/,
	                                     PDISPATCHER_CONTEXT dispatcher_context)
	{
		ExceptionHandler::ExceptionInfo info {};

		if (exception_record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
		{
			info.type = ExceptionHandler::ExceptionType::AccessViolation;
			switch (exception_record->ExceptionInformation[0])
			{
				case 0: info.access_violation_type = ExceptionHandler::AccessViolationType::Read; break;
				case 1: info.access_violation_type = ExceptionHandler::AccessViolationType::Write; break;
				case 8: info.access_violation_type = ExceptionHandler::AccessViolationType::Execute; break;
				default: info.access_violation_type = ExceptionHandler::AccessViolationType::Unknown; break;
			}
			info.access_violation_vaddr = exception_record->ExceptionInformation[1];
		}

		auto* p = *static_cast<ExceptionHandlerPrivate**>(dispatcher_context->HandlerData);
		p->func(&info);

		return ExceptionContinueExecution;
	}

	void InitHandler()
	{
		auto* h           = new (reinterpret_cast<void*>(handler_addr)) HandlerInfo;
		auto* code        = &h->code;
		auto* unwind_info = &h->unwind_info;

		function_table = &h->function_table;

		function_table->BeginAddress = 0;
		function_table->EndAddress   = image_size;
		function_table->UnwindData   = reinterpret_cast<uintptr_t>(unwind_info) - base_address;

		unwind_info->Version          = 1;
		unwind_info->Flags            = UNW_FLAG_EHANDLER;
		unwind_info->SizeOfProlog     = 0;
		unwind_info->CountOfCodes     = 0;
		unwind_info->FrameRegister    = 0;
		unwind_info->FrameOffset      = 0;
		unwind_info->ExceptionHandler = reinterpret_cast<uintptr_t>(code) - base_address;
		unwind_info->ExceptionData    = this;

		code->SetFunc(Handler);

		FlushInstructionCache(reinterpret_cast<uint64_t>(code), sizeof(h->code));
	}

	uint64_t          base_address   = 0;
	uint64_t          handler_addr   = 0;
	uint64_t          image_size     = 0;
	PRUNTIME_FUNCTION function_table = nullptr;

	ExceptionHandler::handler_func_t func = nullptr;
};

ExceptionHandler::ExceptionHandler(): m_p(new ExceptionHandlerPrivate) {}

ExceptionHandler::~ExceptionHandler()
{
	Uninstall();
	delete m_p;
}

uint64_t ExceptionHandler::GetSize()
{
	return (sizeof(ExceptionHandlerPrivate::HandlerInfo) & ~(uint64_t(0x1000) - 1)) + 0x1000;
}

bool ExceptionHandler::Install(uint64_t base_address, uint64_t handler_addr, uint64_t image_size, handler_func_t func)
{
	if (m_p->function_table == nullptr)
	{
		m_p->base_address = base_address;
		m_p->handler_addr = handler_addr;
		m_p->image_size   = image_size;
		m_p->func         = func;

		m_p->InitHandler();

		if (RtlAddFunctionTable(m_p->function_table, 1, base_address) == FALSE)
		{
			printf("RtlAddFunctionTable() failed: 0x%08" PRIx32 "\n", static_cast<uint32_t>(GetLastError()));
			return false;
		}

		return true;
	}

	return false;
}

bool ExceptionHandler::Uninstall()
{
	if (m_p->function_table != nullptr)
	{
		if (RtlDeleteFunctionTable(m_p->function_table) == FALSE)
		{
			printf("RtlDeleteFunctionTable() failed: 0x%08" PRIx32 "\n", static_cast<uint32_t>(GetLastError()));
			return false;
		}
		m_p->function_table = nullptr;
		return true;
	}

	return false;
}

static DWORD get_protection_flag(VirtualMemory::Mode mode)
{
	DWORD protect = PAGE_NOACCESS;
	switch (mode)
	{
		case VirtualMemory::Mode::Read: protect = PAGE_READONLY; break;

		case VirtualMemory::Mode::Write:
		case VirtualMemory::Mode::ReadWrite: protect = PAGE_READWRITE; break;

		case VirtualMemory::Mode::Execute: protect = PAGE_EXECUTE; break;

		case VirtualMemory::Mode::ExecuteRead: protect = PAGE_EXECUTE_READ; break;

		case VirtualMemory::Mode::ExecuteWrite:
		case VirtualMemory::Mode::ExecuteReadWrite: protect = PAGE_EXECUTE_READWRITE; break;

		case VirtualMemory::Mode::NoAccess:
		default: protect = PAGE_NOACCESS; break;
	}
	return protect;
}

static VirtualMemory::Mode get_protection_flag(DWORD mode)
{
	switch (mode)
	{
		case PAGE_NOACCESS: return VirtualMemory::Mode::NoAccess;
		case PAGE_READONLY: return VirtualMemory::Mode::Read;
		case PAGE_READWRITE: return VirtualMemory::Mode::ReadWrite;
		case PAGE_EXECUTE: return VirtualMemory::Mode::Execute;
		case PAGE_EXECUTE_READ: return VirtualMemory::Mode::ExecuteRead;
		case PAGE_EXECUTE_READWRITE: return VirtualMemory::Mode::ExecuteReadWrite;
		default: return VirtualMemory::Mode::NoAccess;
	}
}

uint64_t Alloc(uint64_t address, uint64_t size, Mode mode)
{
	auto ptr = reinterpret_cast<uintptr_t>(VirtualAlloc(reinterpret_cast<LPVOID>(static_cast<uintptr_t>(address)), size,
	                                                    static_cast<DWORD>(MEM_COMMIT) | static_cast<DWORD>(MEM_RESERVE),
	                                                    get_protection_flag(mode)));
	if (ptr == 0)
	{
		printf("VirtualAlloc() failed: 0x%08" PRIx32 "\n", static_cast<uint32_t>(GetLastError()));
	}
	return ptr;
}

using VirtualAlloc2_func_t = /*WINBASEAPI*/ PVOID WINAPI (*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);

static VirtualAlloc2_func_t ResolveVirtualAlloc2()
{
	HMODULE h = GetModuleHandle("KernelBase");
	if (h != nullptr)
	{
		return reinterpret_cast<VirtualAlloc2_func_t>(GetProcAddress(h, "VirtualAlloc2"));
	}
	return nullptr;
}

uint64_t AllocAligned(uint64_t /*address*/, uint64_t size, Mode mode, uint64_t alignment)
{
	MEM_ADDRESS_REQUIREMENTS req2 {};
	MEM_EXTENDED_PARAMETER   param {};
	req2.LowestStartingAddress = nullptr;
	req2.HighestEndingAddress  = reinterpret_cast<PVOID>(0xffffffffffu); // nullptr;
	req2.Alignment             = alignment;
	param.Type                 = MemExtendedParameterAddressRequirements;
	param.Pointer              = &req2;

	static auto virtual_alloc2 = ResolveVirtualAlloc2();

	EXIT_NOT_IMPLEMENTED(virtual_alloc2 == nullptr);

	auto ptr = reinterpret_cast<uintptr_t>(virtual_alloc2(GetCurrentProcess(), nullptr, size,
	                                                      static_cast<DWORD>(MEM_COMMIT) | static_cast<DWORD>(MEM_RESERVE),
	                                                      get_protection_flag(mode), &param, 1));
	if (ptr == 0)
	{
		printf("VirtualAlloc2() failed: 0x%08" PRIx32 "\n", static_cast<uint32_t>(GetLastError()));
	}
	return ptr;
}

bool Free(uint64_t address)
{
	if (VirtualFree(reinterpret_cast<LPVOID>(static_cast<uintptr_t>(address)), 0, MEM_RELEASE) == 0)
	{
		printf("VirtualFree() failed: 0x%08" PRIx32 "\n", static_cast<uint32_t>(GetLastError()));
		return false;
	}
	return true;
}

bool Protect(uint64_t address, uint64_t size, Mode mode, Mode* old_mode)
{
	DWORD old_protect = 0;
	if (VirtualProtect(reinterpret_cast<LPVOID>(static_cast<uintptr_t>(address)), size, get_protection_flag(mode), &old_protect) == 0)
	{
		printf("VirtualProtect() failed: 0x%08" PRIx32 "\n", static_cast<uint32_t>(GetLastError()));
		return false;
	}
	if (old_mode != nullptr)
	{
		*old_mode = get_protection_flag(old_protect);
	}
	return true;
}

bool FlushInstructionCache(uint64_t address, uint64_t size)
{
	if (::FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<LPVOID>(static_cast<uintptr_t>(address)), size) == 0)
	{
		printf("FlushInstructionCache() failed: 0x%08" PRIx32 "\n", static_cast<uint32_t>(GetLastError()));
		return false;
	}
	return true;
}

bool PatchReplace(uint64_t vaddr, uint64_t value)
{
	KYTY_PROFILER_FUNCTION();

	VirtualMemory::Mode old_mode {};
	VirtualMemory::Protect(vaddr, 8, VirtualMemory::Mode::ReadWrite, &old_mode);

	auto* ptr = reinterpret_cast<uint64_t*>(vaddr);

	bool ret = (*ptr != value);

	*ptr = value;

	VirtualMemory::Protect(vaddr, 8, old_mode);

	if (VirtualMemory::IsExecute(old_mode))
	{
		VirtualMemory::FlushInstructionCache(vaddr, 8);
	}

	return ret;
}

} // namespace VirtualMemory

} // namespace Kyty::Loader

#endif // KYTY_EMU_ENABLED