#include "boxedwine.h"
#include <windows.h>

U32 nativeMemoryPagesAllocated;

#ifdef BOXEDWINE_64BIT_MMU
static U32 gran = 0; // number of K_PAGE_SIZE in an allocation, on Windows this is 16

#include "../../source/emulation/hardmmu/hard_memory.h"

U32 getHostPageSize() {
    SYSTEM_INFO sSysInfo;

    GetSystemInfo(&sSysInfo);
    if (sSysInfo.dwPageSize != K_PAGE_SIZE) {
        kpanic("Was expecting a host page size of 4k, instead host page size is %d bytes", sSysInfo.dwPageSize);
    }
    return sSysInfo.dwPageSize;
}

U32 getHostAllocationSize() {
    SYSTEM_INFO sSysInfo;

    GetSystemInfo(&sSysInfo);
    if ((sSysInfo.dwAllocationGranularity & K_PAGE_SIZE) != 0) {
        kpanic("Unexpected host allocation granularity size: %d", sSysInfo.dwAllocationGranularity);
    }
    return sSysInfo.dwAllocationGranularity;
}

#ifdef BOXEDWINE_X64

// :TODO: what about some sort of garbage collection to MEM_DECOMMIT chunks that no longer contain code mappings
void commitHostAddressSpaceMapping(Memory* memory, U32 page, U32 pageCount, U64 defaultValue) {
    U64 granPage;
    U64 granCount;

    if (!gran) {
        gran = getHostAllocationSize() / K_PAGE_SIZE;
    }
    if (page < 10) {
        int ii = 0;
    }
    U64 hostPage = page * sizeof(void*);
    U64 hostPageCount = pageCount * sizeof(void*);

    granPage = hostPage & ~((U64)gran - 1);
    granCount = ((gran - 1) + hostPageCount + (hostPage - granPage)) / gran;
    for (U32 i = 0; i < granCount; i++) {
        if (!memory->isEipPageCommitted((U32)(granPage / sizeof(void*)))) {
            U8* address = (U8*)memory->eipToHostInstructionAddressSpaceMapping + (granPage << K_PAGE_SHIFT);
            if (!VirtualAlloc(address, (gran << K_PAGE_SHIFT), MEM_COMMIT, PAGE_READWRITE)) {
                LPSTR messageBuffer = NULL;
                size_t size = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, GetLastError(), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&messageBuffer, 0, NULL);
                kpanic("allocNativeMemory: failed to commit memory: granPage=%x page=%x pageCount=%d: %s", granPage, page, pageCount, messageBuffer);
            }
            U64* address64 = (U64*)address;
            U32 count = (gran << K_PAGE_SHIFT) / sizeof(void*); // 8K
            for (U32 j = 0; j < count; j++, address64++) {
                *address64 = defaultValue;
            }
            U32 startPage = (U32)(granPage / sizeof(void*));
            U32 pageCount = (U32)((granCount * gran) / sizeof(void*));
            for (U32 j = 0; j < pageCount; j++) {
                memory->setEipPageCommitted(startPage + j);
            }
        }
        granPage += gran;
    }
}
#endif

#ifdef BOXEDWINE_BINARY_TRANSLATOR
void* allocExecutable64kBlock(Memory* memory, U32 count) {
    void* result = VirtualAlloc(NULL, 64 * 1024 * count, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!result) {
        LPSTR messageBuffer = NULL;
        size_t size = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, GetLastError(), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&messageBuffer, 0, NULL);
        kpanic("allocExecutable64kBlock: failed to commit memory : %s", messageBuffer);
    }
    return result;
}
#endif

static void* reserveNext4GBMemory() {
    void* p;
    U64 i=1;

    p = (void*)(i << 32);
    while (VirtualAlloc(p, 0x100000000l, MEM_RESERVE, PAGE_READWRITE)==0) {
        i++;
        p = (void*)(i << 32);
    } 
    return p;
}

static void* reserveNext32GBMemory() {
    void* p;
    U64 i = 1;

    p = (void*)(i << 32);
    while (VirtualAlloc(p, 0x800000000l, MEM_RESERVE, PAGE_READWRITE) == 0) {
        i++;
        p = (void*)(i << 32);
    }
    return p;
}

void reserveNativeMemory(Memory* memory) {    
    memory->id = (U64)reserveNext4GBMemory();
    for (int i = 0; i < K_NUMBER_OF_PAGES; i++) {
        memory->memOffsets[i] = memory->id;
    }
#ifdef BOXEDWINE_BINARY_TRANSLATOR
    if (KSystem::useLargeAddressSpace) {
        memory->eipToHostInstructionAddressSpaceMapping = reserveNext32GBMemory();
    }
#endif
}

void releaseNativeMemory(Memory* memory) {
    U32 i;
    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(memory->executableMemoryMutex);
    for (i=0;i<K_NUMBER_OF_PAGES;i++) {
        memory->clearCodePageFromCache(i);
    }
    memory->clearAllNeedsMemoryOffset();

    if (!VirtualFree((void*)memory->id, 0, MEM_RELEASE)) {
        LPSTR messageBuffer = NULL;
        size_t size = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, GetLastError(), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&messageBuffer, 0, NULL);
        kpanic("failed to release memory: %s", messageBuffer);
    }    
    memset(memory->flags, 0, sizeof(memory->flags));
    memset(memory->nativeFlags, 0, sizeof(memory->nativeFlags));
    memset(memory->memOffsets, 0, sizeof(memory->memOffsets));
    memory->allocated = 0;
#ifdef BOXEDWINE_BINARY_TRANSLATOR
    memory->executableMemoryReleased();    
    for (auto& p : memory->allocatedExecutableMemory) {
        if (!VirtualFree(p.memory, 0, MEM_RELEASE)) {
            LPSTR messageBuffer = NULL;
            size_t size = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, GetLastError(), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&messageBuffer, 0, NULL);
            kpanic("failed to release executable memory: %s", messageBuffer);
        }
    }
    memory->allocatedExecutableMemory.clear();
    if (KSystem::useLargeAddressSpace) {
        if (!VirtualFree((void*)memory->eipToHostInstructionAddressSpaceMapping, 0, MEM_RELEASE)) {
            LPSTR messageBuffer = NULL;
            size_t size = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, GetLastError(), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&messageBuffer, 0, NULL);
            kpanic("failed to release large executable memory: %s", messageBuffer);
        }
        memory->eipToHostInstructionAddressSpaceMapping = NULL;
    }
#endif
}

void makeCodePageReadOnly(Memory* memory, U32 page) {
    DWORD oldProtect;

    // :TODO: would the granularity ever be more than 4k?  should I check: SYSTEM_INFO System_Info; GetSystemInfo(&System_Info);
    if (!(memory->nativeFlags[page] & NATIVE_FLAG_CODEPAGE_READONLY)) {
        if (memory->dynamicCodePageUpdateCount[page]==MAX_DYNAMIC_CODE_PAGE_COUNT) {
            kpanic("makeCodePageReadOnly: tried to make a dynamic code page read-only");
        }
        if (!VirtualProtect(getNativeAddressNoCheck(memory, page << K_NATIVE_PAGE_SHIFT), (1 << K_NATIVE_PAGE_SHIFT), PAGE_READONLY, &oldProtect)) {
            LPSTR messageBuffer = NULL;
            size_t size = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, GetLastError(), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&messageBuffer, 0, NULL);
            kpanic("makeCodePageReadOnly: Failed to protect memory 0x%0.8X: %s", (page<< K_NATIVE_PAGE_SHIFT), messageBuffer);
        }
        memory->nativeFlags[page] |= NATIVE_FLAG_CODEPAGE_READONLY;
    }
}

bool clearCodePageReadOnly(Memory* memory, U32 page) {
    DWORD oldProtect;
    bool result = false;

    // :TODO: would the granularity ever be more than 4k?  should I check: SYSTEM_INFO System_Info; GetSystemInfo(&System_Info);
    if (memory->nativeFlags[page] & NATIVE_FLAG_CODEPAGE_READONLY) {        
        if (!VirtualProtect(getNativeAddressNoCheck(memory, page << K_NATIVE_PAGE_SHIFT), (1 << K_NATIVE_PAGE_SHIFT), PAGE_READWRITE, &oldProtect)) {
            LPSTR messageBuffer = NULL;
            size_t size = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, GetLastError(), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&messageBuffer, 0, NULL);
            kpanic("failed to unprotect memory: %s", messageBuffer);
        }
        memory->nativeFlags[page] &= ~NATIVE_FLAG_CODEPAGE_READONLY;
        result = true;
    }
    return result;
}
#ifndef BOXEDWINE_MULTI_THREADED
static int seh_filter(unsigned int code, struct _EXCEPTION_POINTERS* ep, KThread* thread)
{
    if (code == EXCEPTION_ACCESS_VIOLATION) {
        U32 address = getHostAddress(thread, (void*)ep->ExceptionRecord->ExceptionInformation[1]);
        if (thread->process->memory->nativeFlags[address>>K_PAGE_SHIFT] & NATIVE_FLAG_CODEPAGE_READONLY) {
            DWORD oldProtect;
            U32 page = address>>K_PAGE_SHIFT;

            if (!VirtualProtect(getNativeAddress(thread->process->memory, address & 0xFFFFF000), (1 << K_PAGE_SHIFT), PAGE_READWRITE, &oldProtect)) {
                LPSTR messageBuffer = NULL;
                size_t size = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, GetLastError(), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&messageBuffer, 0, NULL);
                kpanic("failed to unprotect memory: %s", messageBuffer);
            }
            thread->process->memory->nativeFlags[page] &= ~NATIVE_FLAG_CODEPAGE_READONLY;
            thread->process->memory->clearCodePageFromCache(page);
            return EXCEPTION_CONTINUE_EXECUTION;
        } else {
            thread->seg_mapper(address, ep->ExceptionRecord->ExceptionInformation[0]==0, ep->ExceptionRecord->ExceptionInformation[0]!=0, false);
            return EXCEPTION_EXECUTE_HANDLER;
        }
    }   
    return EXCEPTION_CONTINUE_SEARCH;
}

void platformRunThreadSlice(KThread* thread) {
    __try {
        runThreadSlice(thread);
    } __except(seh_filter(GetExceptionCode(), GetExceptionInformation(), thread)) {
        thread->cpu->nextBlock = NULL;
    }
}
#endif
#endif