#include "service_hook.h"
#include "include/stdafx.h"
#include "include/vector.hpp"
#include "include/exclusivity.h"
#include "include/write_protect.h"
#include "kernel-hook/khook/hde/hde.h"
#include "include/handle.h"
#include "common.h"
#include "log.h"
#include "util.h"
#include "config.h"
#include <stdarg.h>
#include <ntstrsafe.h>
#include <intrin.h>
#include <cassert>
#include "systemcall.h"

extern "C"
{
#include"kernel-hook/khook/khook/hk.h"
#include "minirtl/minirtl.h"
extern ULONG_PTR KernelBase;
extern ULONG_PTR PspCidTable;
extern ULONG_PTR Win32kfullBase;
extern ULONG Win32kfullSize;
}

extern tagGlobalConfig GlobalConfig;

using std::vector; 
vector<ServiceHook> vServcieHook;
hde64s gIns;

LARGE_INTEGER MmOneSecond = { (ULONG)(-1 * 1000 * 1000 * 10), -1 };
LARGE_INTEGER MmTwentySeconds = { (ULONG)(-20 * 1000 * 1000 * 10), -1 };
LARGE_INTEGER MmShortTime = { (ULONG)(-10 * 1000 * 10), -1 }; // 10 milliseconds
LARGE_INTEGER MmHalfSecond = { (ULONG)(-5 * 100 * 1000 * 10), -1 };
LARGE_INTEGER Mm30Milliseconds = { (ULONG)(-30 * 1000 * 10), -1 };


void ServiceHook::Construct()
{
	if (!this->DetourFunc || !this->TrampolineFunc || !this->fp.GuestVA)
	{
		HYPERPLATFORM_LOG_WARN("ServiceHook::Construct fail");
		return;
	}

	HYPERPLATFORM_LOG_INFO("ServiceHook::Construct %s", this->funcName.c_str());

	//
	if (this->fp.GuestVA > (PVOID)Win32kfullBase && this->fp.GuestVA < (PVOID)(Win32kfullBase + Win32kfullSize))
	{
		HYPERPLATFORM_LOG_INFO("this->isWin32Hook = true");
		this->isWin32Hook = true;
	}

	// 如果是win32 hook,要切换session
	PEPROCESS Csrss = NULL;
	KAPC_STATE pRkapcState = { 0x00 };
	if (this->isWin32Hook) {
		NTSTATUS Status;
		Status = PsLookupProcessByProcessId(g_CsrssPid, &Csrss);
		if (NT_SUCCESS(Status)) {
			if (NT_SUCCESS(MmAttachSession(Csrss, &pRkapcState))) {
			}
			else {
				HYPERPLATFORM_LOG_INFO("Attach Session fail");
				ObDereferenceObject(Csrss);
				return;
			}
		}
		else {
			HYPERPLATFORM_LOG_INFO("PsLookupProcessByProcessId Csrss fail");
			return;
		}
	}

	// 获得指定函数所在页的开始处
	auto tmp = (PVOID)(((ULONG_PTR)(this->fp).GuestVA >> 12) << 12);
	
	// GuestPA为GuestVA这个页面起始的物理地址
	// GuestVA必须初始化后不能改变
	//
	// 如果pte.vaild为0，MmGetPhysicalAddress返回0 , 因此前面就要切换session
	//
	this->fp.GuestPA = MmGetPhysicalAddress(tmp);
	if (!this->fp.GuestPA.QuadPart)
	{
		HYPERPLATFORM_LOG_WARN("ServiceHook::Construct fail , Address %p is invalid", tmp);
		if (this->isWin32Hook)
		{
			MmDetachSession(Csrss, &pRkapcState);
		}
		return;
	}
	this->fp.PageContent = ExAllocatePoolWithQuotaTag(NonPagedPool, PAGE_SIZE,'zxc');

	// 拷贝原页面内容
	memcpy(this->fp.PageContent, tmp, PAGE_SIZE);

	// 
	this->fp.PageContentPA = MmGetPhysicalAddress(this->fp.PageContent);
	if (!this->fp.PageContentPA.QuadPart){
		HYPERPLATFORM_LOG_WARN("ServiceHook::Construct fail , Address %p is invalid", this->fp.PageContentPA.QuadPart);
		return;
	}

	auto exclusivity = ExclGainExclusivity();
	//
	//
	//mov rax,xx
	//jmp rax
	//
	//
	static char hook[] = { 0x48,0xB8,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0xFF,0xE0 };
	size_t CodeLength = 0;
	while (CodeLength < 12)
	{
		HdeDisassemble((void*)((ULONG_PTR)(this->fp.GuestVA) + CodeLength), &gIns);
		CodeLength += gIns.len;
	}
	this->HookCodeLen = (ULONG)CodeLength;

	/*
	* 1.分配一个动态内存(Orixxxxx)保存函数开头至少12个字节,还得加上一个jmpxxx的字节，因为Ori还得jmp回去
	* 2.然后修改函数开头为move rax,xx jump rax,xx
	*/

	*(this->TrampolineFunc) = ExAllocatePoolWithTag(NonPagedPool, CodeLength + 14, 'zxc');
	if (!*(this->TrampolineFunc))
	{
		HYPERPLATFORM_LOG_INFO("ExAllocatePoolWithTag failed ,no memory!");
		return;
	}
	
	memcpy(*(this->TrampolineFunc), this->fp.GuestVA, CodeLength);
	static char hook2[] = { 0xff,0x25,0,0,0,0,1,1,1,1,1,1,1,1 };
	ULONG_PTR jmp_return = (ULONG_PTR)this->fp.GuestVA + CodeLength;
	memcpy(hook2 + 6, &jmp_return, 8);
	memcpy((void*)((ULONG_PTR)(*(this->TrampolineFunc)) + CodeLength), hook2, 14);
	auto irql = WPOFFx64();

	PVOID* Ptr = &this->DetourFunc;
	memcpy(hook + 2, Ptr, 8);

	vServcieHook.push_back(*this);
	memcpy((PVOID)this->fp.GuestVA, hook, sizeof(hook));   // 实际hook

	WPONx64(irql);

	ExclReleaseExclusivity(exclusivity);

	if (this->isWin32Hook)
	{
		MmDetachSession(Csrss, &pRkapcState);
	}
}

void ServiceHook::Destruct()
{
	// 虚拟化的hook不能用传统的hook框架,开了ept之后会有问题
	//NTSTATUS Status = HkRestoreFunction((this->fp).GuestVA, this->TrampolineFunc);

	if (KeGetCurrentIrql() >= DISPATCH_LEVEL)
	{
		HYPERPLATFORM_LOG_ERROR("IRQL is too high");
		return;
	}

	PEPROCESS Csrss = NULL;
	KAPC_STATE pRkapcState = { 0x00 };
	if (this->isWin32Hook) {
		NTSTATUS Status;
		Status = PsLookupProcessByProcessId(g_CsrssPid, &Csrss);
		if (NT_SUCCESS(Status)) {
			if (NT_SUCCESS(MmAttachSession(Csrss, &pRkapcState))) {
			}
			else {
				HYPERPLATFORM_LOG_INFO("Attach Session fail");
				ObDereferenceObject(Csrss);
				return;
			}
		}
		else {
			HYPERPLATFORM_LOG_INFO("PsLookupProcessByProcessId Csrss fail");
			return;
		}
	}

	// 这个页面地址不合法
	// 19:30:25.421	ERR	#0	  404	 5604	csrss.exe      	GuestVA ffff8ebb550e3260 is invalid
	if (!MmIsAddressValid(this->fp.GuestVA))
	{
		HYPERPLATFORM_LOG_WARN("GuestVA %llx is invalid", this->fp.GuestVA);
	}

	while (this->refCount > 0)
	{
		HYPERPLATFORM_LOG_INFO("%s reference count is %d , delay 30ms", this->funcName.c_str(), this->refCount);
		KeDelayExecutionThread(KernelMode, false, &Mm30Milliseconds);
	}

	//
	auto Exclu = ExclGainExclusivity();
	auto irql = WPOFFx64();
	memcpy(this->fp.GuestVA, *(this->TrampolineFunc), this->HookCodeLen);
	WPONx64(irql);
	ExclReleaseExclusivity(Exclu);
	ExFreePool(*(this->TrampolineFunc));
	if (this->isWin32Hook)
	{
		MmDetachSession(Csrss, &pRkapcState);
	}
}

//
// 开始hook
//
void AddServiceHook(PVOID HookFuncStart, PVOID Detour, PVOID *TramPoline,const char* funcName)
{
	HYPERPLATFORM_LOG_INFO("AddServiceHook %s ", funcName);
	if (!HookFuncStart)
	{
		HYPERPLATFORM_LOG_WARN("HookFuncStart is NULL");
		return;
	}

	ServiceHook tmp;
	memset(&tmp, 0, sizeof(tmp));
	tmp.DetourFunc = Detour;
	tmp.fp.GuestVA = HookFuncStart;
	tmp.TrampolineFunc = TramPoline;
	tmp.funcName = funcName;
	tmp.Construct();
}

//
// 卸载hook
//
void RemoveServiceHook()
{
	HYPERPLATFORM_LOG_INFO("RemoveServiceHook enter");
	for (auto& hook : vServcieHook)
	{
		HYPERPLATFORM_LOG_INFO("unload %s", hook.funcName.c_str());
		hook.Destruct();
		HYPERPLATFORM_LOG_INFO("unload hook func %s success", hook.funcName.c_str());
	}
}

