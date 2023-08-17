/*
 *  Copyright (C) 2016  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */
#include "boxedwine.h"

#include "log.h"
#include "kscheduler.h"
#include "ksignal.h"
#include "ksocket.h"
#include "kepoll.h"

#include <stdarg.h>
#include <random>
#include <thread>

U64 sysCallTime;
extern struct Block emptyBlock;
//#undef LOG_SYSCALLS
#undef LOG_OPS
#define LOG_OPS
#ifdef LOG_OPS
void logsyscall(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    if (1) {
        char tmp[256];
        vsprintf(tmp, fmt, args);       
    }
    va_end(args);
}

#define LOGSYS logsyscall
#elif defined LOG_SYSCALLS
#define LOGSYS klog
#else
#define LOGSYS if (0) klog
#endif


#define ARG1 EBX
#define ARG2 ECX
#define ARG3 EDX
#define ARG4 ESI
#define ARG5 EDI
#define ARG6 EBP

#define SARG2 readd(ARG2)
#define SARG3 readd(ARG2+4)
#define SARG4 readd(ARG2+8)
#define SARG5 readd(ARG2+12)
#define SARG6 readd(ARG2+16)
#define SARG7 readd(ARG2+20)

typedef U32 (*SyscallFunc)(CPU* cpu, U32 eipCount);

#define SYSCALL_PROCESS     0x01
#define SYSCALL_THREAD      0x02
#define SYSCALL_FILE        0x04
#define SYSCALL_READ        0x08
#define SYSCALL_WRITE       0x10
#define SYSCALL_SYSTEM      0x20
#define SYSCALL_SIGNAL      0x40
#define SYSCALL_MEMORY      0x80
#define SYSCALL_SOCKET      0x100
#define SYSCALL_FUTEX       0x200

#ifdef _DEBUG
static U32 syscallMask = 0;
#else
static U32 syscallMask = 0;
#endif

void sysLog(U32 type, CPU* cpu, const char* msg, ...) {
    va_list argptr;
    va_start(argptr, msg);
    if (type & syscallMask) {
        vprintf( msg, argptr);
    }
    va_end(argptr);
}

void sysLog1(U32 type, CPU* cpu, const char* msg, ...) {
    va_list argptr;
    va_start(argptr, msg);
    if (type & syscallMask) {
        printf("%.4X/%.4X %s ", cpu->thread->process->id, cpu->thread->id, cpu->thread->process->name.c_str());
        vprintf( msg, argptr);
    }
    va_end(argptr);
}

#define SYS_LOG if (syscallMask) sysLog
#define SYS_LOG1 if (syscallMask) sysLog1

static U32 syscall_exit(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_PROCESS, cpu, "exit: status=%d", ARG1);
    U32 result = cpu->thread->process->exit(ARG1);
    SYS_LOG(SYSCALL_PROCESS, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_read(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_READ, cpu, "read: fd=%d buf=0x%X len=%d", ARG1, ARG2, ARG3);
    U32 result = cpu->thread->process->read((FD)ARG1, ARG2, ARG3);
    SYS_LOG(SYSCALL_READ, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_write(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_WRITE, cpu, "write: fd=%d buf=0x%X len=%d", ARG1, ARG2, ARG3);
    U32 result = cpu->thread->process->write((FD)ARG1, ARG2, ARG3);
    SYS_LOG(SYSCALL_WRITE, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

BoxedPtr<FsNode> findNode(BoxedPtr<FsNode> parent, const std::string& name);

static U32 syscall_open(CPU* cpu, U32 eipCount) {
    char tmp[MAX_FILEPATH_LEN];
    SYS_LOG1(SYSCALL_FILE, cpu, "open: name=%s flags=%x", getNativeString(ARG1, tmp, sizeof(tmp)), ARG2);
    U32 result = cpu->thread->process->open(getNativeString(ARG1, tmp, sizeof(tmp)), ARG2);
#ifdef _DEBUG
    if (result>1000) {
        std::string filename = Fs::getFileNameFromPath(getNativeString(ARG1, tmp, sizeof(tmp)));
        BoxedPtr<FsNode> found = findNode(Fs::getNodeFromLocalPath("", "/", false), filename);
        if (!found) {
            printf("open: name=%s flags=%x result=%X\n", getNativeString(ARG1, tmp, sizeof(tmp)), ARG2, result);
        }
    }
#endif
    SYS_LOG(SYSCALL_FILE, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_close(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_FILE, cpu, "close: fd=%d", ARG1);
    U32 result = cpu->thread->process->close(ARG1);
    SYS_LOG(SYSCALL_FILE, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_waitpid(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_PROCESS, cpu, "waitpid: pid=%d status=%d options=%x", ARG1, ARG2, ARG3);
    U32 result = KSystem::waitpid((S32)ARG1, ARG2, ARG3);
    SYS_LOG(SYSCALL_PROCESS, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_link(CPU* cpu, U32 eipCount) {
    char tmp1[MAX_FILEPATH_LEN];
    char tmp2[MAX_FILEPATH_LEN];
    SYS_LOG1(SYSCALL_FILE, cpu, "link: path1=%X(%s) path2=%X(%s)", ARG1, getNativeString(ARG1, tmp1, sizeof(tmp1)), ARG2, getNativeString(ARG2, tmp2, sizeof(tmp2)));
    U32 result = cpu->thread->process->link(getNativeString(ARG1, tmp1, sizeof(tmp1)), getNativeString(ARG2, tmp2, sizeof(tmp2)));
    SYS_LOG(SYSCALL_FILE, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_unlink(CPU* cpu, U32 eipCount) {
    char tmp[MAX_FILEPATH_LEN];
    SYS_LOG1(SYSCALL_FILE, cpu, "unlink: path=%X(%s)", ARG1, getNativeString(ARG1, tmp, sizeof(tmp)));
    U32 result = cpu->thread->process->unlinkFile(getNativeString(ARG1, tmp, sizeof(tmp)));
    SYS_LOG(SYSCALL_FILE, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static void readStringArray(U32 address, std::vector<std::string>& results) {
    char tmp2[MAX_FILEPATH_LEN];

    while (true) {
        U32 p = readd(address);		
        if (!p)
            break;
        char* str = getNativeString(p, tmp2, sizeof(tmp2));
        address+=4;	
        results.push_back(str);
    }
}

static U32 syscall_execve(CPU* cpu, U32 eipCount) {
    char tmp[MAX_FILEPATH_LEN];
    std::vector<std::string> args;
    std::vector<std::string> envs;

    readStringArray(ARG2, args);
    readStringArray(ARG3, envs);

    SYS_LOG1(SYSCALL_PROCESS, cpu, "execve: path=%X(%s) argv=%X envp=%X", ARG1, getNativeString(ARG1, tmp, sizeof(tmp)), ARG2, ARG3);
    if (cpu->thread->process->execve(getNativeString(ARG1, tmp, sizeof(tmp)), args, envs)) {
        SYS_LOG(SYSCALL_PROCESS, cpu, "\n");
        return -K_CONTINUE;
    }
    SYS_LOG(SYSCALL_PROCESS, cpu, " result=FAILED\n");
    return -ENOENT;
}

static U32 syscall_chdir(CPU* cpu, U32 eipCount) {
    char tmp[MAX_FILEPATH_LEN];
    SYS_LOG1(SYSCALL_PROCESS, cpu, "chdir: path=%X(%s)", ARG1, getNativeString(ARG1, tmp, sizeof(tmp)));
    U32 result = cpu->thread->process->chdir(getNativeString(ARG1, tmp, sizeof(tmp)));
    SYS_LOG(SYSCALL_PROCESS, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_time(CPU* cpu, U32 eipCount) {
    U32 result = (U32)(KSystem::getSystemTimeAsMicroSeconds() / 1000000l);
    if (ARG1)
        writed(ARG1, result);
    SYS_LOG1(SYSCALL_SYSTEM, cpu, "time: tloc=%X result=%d(0x%X)\n", ARG1, result, result);
    return result;
}

static U32 syscall_chmod(CPU* cpu, U32 eipCount) {
    char tmp[MAX_FILEPATH_LEN];
    SYS_LOG1(SYSCALL_FILE, cpu, "chmod: path=%X (%s) mode=%o", ARG1, getNativeString(ARG1, tmp, sizeof(tmp)), ARG2);
    U32 result = cpu->thread->process->chmod(getNativeString(ARG1, tmp, sizeof(tmp)), ARG2);
    SYS_LOG(SYSCALL_FILE, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_lseek(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_FILE, cpu, "lseek: fildes=%d offset=%d whence=%d", ARG1, ARG2, ARG3);
    U32 result = cpu->thread->process->lseek((FD)ARG1, (S32)ARG2, ARG3);
    SYS_LOG(SYSCALL_FILE, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_getpid(CPU* cpu, U32 eipCount) {    
    U32 result = cpu->thread->process->id;
    SYS_LOG1(SYSCALL_SYSTEM, cpu, "getpid: result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_getuid(CPU* cpu, U32 eipCount) {
    U32 result = cpu->thread->process->userId;
    SYS_LOG1(SYSCALL_SYSTEM, cpu, "getuid: result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_ptrace(CPU* cpu, U32 eipCount) {
    return -K_EPERM;
}

static U32 syscall_alarm(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_PROCESS, cpu, "alarm: seconds=%d", ARG1);
    U32 result = cpu->thread->process->alarm(ARG1);
    SYS_LOG(SYSCALL_PROCESS, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_utime(CPU* cpu, U32 eipCount) {
    char tmp[MAX_FILEPATH_LEN];
    SYS_LOG1(SYSCALL_FILE, cpu, "utime: filename=%s times=%X", getNativeString(ARG1, tmp, sizeof(tmp)), ARG2);
    U32 result = 0;
    SYS_LOG(SYSCALL_FILE, cpu, " result=%d(0x%X) IGNOREDED\n", result, result);
    return result;
}

static U32 syscall_access(CPU* cpu, U32 eipCount) {
    char tmp[MAX_FILEPATH_LEN];
    SYS_LOG1(SYSCALL_FILE, cpu, "access: filename=%s flags=0x%X", getNativeString(ARG1, tmp, sizeof(tmp)), ARG2);
    U32 result = cpu->thread->process->access(getNativeString(ARG1, tmp, sizeof(tmp)), ARG2);
    SYS_LOG(SYSCALL_FILE, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_sync(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_FILE, cpu, "sync:");
    U32 result = 0; 
    SYS_LOG(SYSCALL_FILE, cpu, " result=%d(0x%X) IGNOREDED\n", result, result);
    return result;
}

static U32 syscall_kill(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_SIGNAL|SYSCALL_PROCESS, cpu, "kill: pid=%d signal=%d", ARG1, ARG2);
    U32 result = KSystem::kill(ARG1, ARG2);
    SYS_LOG(SYSCALL_SIGNAL|SYSCALL_PROCESS, cpu, " result=%d(0x%X) IGNOREDED\n", result, result);
    return result;
}

static U32 syscall_rename(CPU* cpu, U32 eipCount) {
    char tmp[MAX_FILEPATH_LEN];
    char tmp2[MAX_FILEPATH_LEN];

    SYS_LOG1(SYSCALL_FILE, cpu, "rename: oldName=%X(%s) newName=%X(%s)", ARG1, getNativeString(ARG1, tmp, sizeof(tmp)), ARG2, getNativeString(ARG2, tmp2, sizeof(tmp2)));
    U32 result = cpu->thread->process->rename(getNativeString(ARG1, tmp, sizeof(tmp)), getNativeString(ARG2, tmp2, sizeof(tmp2)));
    SYS_LOG(SYSCALL_FILE, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_mkdir(CPU* cpu, U32 eipCount) {
    char tmp[MAX_FILEPATH_LEN];
    SYS_LOG1(SYSCALL_FILE, cpu, "mkdir: path=%X (%s) mode=%X", ARG1, getNativeString(ARG1, tmp, sizeof(tmp)), ARG2);
    U32 result = cpu->thread->process->mkdir(getNativeString(ARG1, tmp, sizeof(tmp)));
    SYS_LOG(SYSCALL_FILE, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_rmdir(CPU* cpu, U32 eipCount) {
    char tmp[MAX_FILEPATH_LEN];
    SYS_LOG1(SYSCALL_FILE, cpu, "rmdir: path=%X(%s)", ARG1, getNativeString(ARG1, tmp, sizeof(tmp)));
    U32 result = cpu->thread->process->rmdir(getNativeString(ARG1, tmp, sizeof(tmp)));
    SYS_LOG(SYSCALL_FILE, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_dup(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_FILE, cpu, "dup: fildes=%d", ARG1);
    U32 result = cpu->thread->process->dup(ARG1);
    SYS_LOG(SYSCALL_FILE, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_pipe(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_FILE, cpu, "pipe: fildes=%X", ARG1);
    U32 result = ksocketpair(K_AF_UNIX, K_SOCK_STREAM, 0, ARG1, 0);
    SYS_LOG(SYSCALL_FILE, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_times(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_SYSTEM, cpu, "times: buf=%X", ARG1);
    U32 result = KSystem::times(ARG1);
    SYS_LOG(SYSCALL_SYSTEM, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_brk(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_MEMORY|SYSCALL_PROCESS, cpu, "brk: address=%.8X", ARG1);
    U32 result = cpu->thread->process->brk(ARG1);
    SYS_LOG(SYSCALL_MEMORY|SYSCALL_PROCESS, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_getgid(CPU* cpu, U32 eipCount) {    
    U32 result = cpu->thread->process->groupId;
    SYS_LOG1(SYSCALL_SYSTEM, cpu, "getgid: result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_geteuid(CPU* cpu, U32 eipCount) {
    U32 result = cpu->thread->process->effectiveUserId;
    SYS_LOG1(SYSCALL_SYSTEM, cpu, "geteuid: result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_getegid(CPU* cpu, U32 eipCount) {
    U32 result = cpu->thread->process->effectiveGroupId;
    SYS_LOG1(SYSCALL_SYSTEM, cpu, "getegid: result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_ioctl(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_FILE, cpu, "ioctl: fd=%d request=%d", ARG1, ARG2);
    U32 result = cpu->thread->process->ioctl(ARG1, ARG2);
    SYS_LOG(SYSCALL_FILE, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_setpgid(CPU* cpu, U32 eipCount) {	
    SYS_LOG1(SYSCALL_SYSTEM, cpu, "setpgid: pid=%d pgid=%d", ARG1, ARG2);
    U32 result = KSystem::setpgid(ARG1, ARG2);
    SYS_LOG(SYSCALL_SYSTEM, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_umask(CPU* cpu, U32 eipCount) {	
    SYS_LOG1(SYSCALL_PROCESS, cpu, "umask: cmask=%X", ARG1);
    U32 result = cpu->thread->process->umask(ARG1);
    SYS_LOG(SYSCALL_PROCESS, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_getpgid(CPU* cpu, U32 eipCount) {	
    U32 result = KSystem::getpgid(ARG1);
    SYS_LOG1(SYSCALL_SYSTEM, cpu, "getpgid: result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_dup2(CPU* cpu, U32 eipCount) {	
    SYS_LOG1(SYSCALL_FILE, cpu, "dup2: fildes1=%d fildes2=%d", ARG1, ARG2);
    U32 result = cpu->thread->process->dup2(ARG1, ARG2);
    SYS_LOG(SYSCALL_FILE, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_getppid(CPU* cpu, U32 eipCount) {	
    U32 result = cpu->thread->process->parentId;
    SYS_LOG1(SYSCALL_SYSTEM, cpu, "getppid: result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_getpgrp(CPU* cpu, U32 eipCount) {	
    U32 result = cpu->thread->process->groupId;;
    SYS_LOG1(SYSCALL_SYSTEM, cpu, "getpgrp: result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_setsid(CPU* cpu, U32 eipCount) {	
#ifdef _DEBUG
    klog("setsid not implemented");
#endif
    U32 result = 0;
    SYS_LOG1(SYSCALL_SYSTEM, cpu, "setsid: result=%d(0x%X) IGNORED\n", result, result);
    return result;
}

static U32 syscall_setrlimit(CPU* cpu, U32 eipCount) {	
#ifdef _DEBUG
    klog("setrlimit not implemented");
#endif
    U32 result = 0;
    SYS_LOG1(SYSCALL_SYSTEM, cpu, "setrlimit: result=%d(0x%X) IGNORED\n", result, result);
    return result;
}

static U32 syscall_getrusuage(CPU* cpu, U32 eipCount) {	
    SYS_LOG1(SYSCALL_SYSTEM, cpu, "getrusage: who=%d usuage=%X", ARG1, ARG2);
    U32 result = cpu->thread->process->getrusuage(ARG1, ARG2);
    SYS_LOG(SYSCALL_FILE, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_gettimeofday(CPU* cpu, U32 eipCount) {	
    SYS_LOG1(SYSCALL_SYSTEM, cpu, "gettimeofday: tv=%X tz=%X", ARG1, ARG2);
    U32 result = KSystem::gettimeofday(ARG1, ARG2);
    SYS_LOG(SYSCALL_SYSTEM, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_symlink(CPU* cpu, U32 eipCount) {
    char tmp[MAX_FILEPATH_LEN];
    char tmp2[MAX_FILEPATH_LEN];
    SYS_LOG1(SYSCALL_FILE, cpu, "symlink: path1=%X(%s) path2=%X(%s)", ARG1, getNativeString(ARG1, tmp, sizeof(tmp)), ARG2, getNativeString(ARG2, tmp2, sizeof(tmp2)));
    U32 result = cpu->thread->process->symlink(getNativeString(ARG1, tmp, sizeof(tmp)), getNativeString(ARG2, tmp2, sizeof(tmp2)));
    SYS_LOG(SYSCALL_FILE, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_readlink(CPU* cpu, U32 eipCount) {
    char tmp[MAX_FILEPATH_LEN];    
    char tmp2[MAX_FILEPATH_LEN];    

    U32 result = cpu->thread->process->readlink(getNativeString(ARG1, tmp, sizeof(tmp)), ARG2, ARG3);
    SYS_LOG1(SYSCALL_FILE, cpu, "readlink: path=%X (%s) buffer=%X (%s) bufSize=%d result=%d(0x%X)\n", ARG1, getNativeString(ARG1, tmp, sizeof(tmp)), ARG2, (((int)result)>0?getNativeString(ARG2, tmp2, sizeof(tmp2)):""), ARG3, result, result);
    return result;
}

static U32 syscall_mmap64(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_MEMORY, cpu, "mmap: address=%.8X len=%d prot=%X flags=%X fd=%d offset=%d", readd(ARG1), readd(ARG1+4), readd(ARG1+8), readd(ARG1+12), readd(ARG1+16), readd(ARG1+20));
    U32 result = cpu->thread->process->mmap(readd(ARG1), readd(ARG1+4), readd(ARG1+8), readd(ARG1+12), readd(ARG1+16), readd(ARG1+20));
    SYS_LOG(SYSCALL_MEMORY, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_unmap(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_MEMORY, cpu, "munmap: address=%X len=%d", ARG1, ARG2);
    U32 result = cpu->thread->process->unmap(ARG1, ARG2);
    SYS_LOG(SYSCALL_MEMORY, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_ftruncate(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_FILE, cpu, "ftruncate: fd=%X len=%d", ARG1, ARG2);
    U32 result = cpu->thread->process->ftruncate64(ARG1, ARG2);
    SYS_LOG(SYSCALL_FILE, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_fchmod(CPU* cpu, U32 eipCount) {	
#ifdef _DEBUG
    klog("fchmod not implemented");
#endif
    U32 result = 0;
    SYS_LOG1(SYSCALL_FILE, cpu, "fchmod: fd=%X mod=%X result=%d(0x%X) IGNORED\n", ARG1, ARG2, result, result);
    return result;
}

static U32 syscall_setpriority(CPU* cpu, U32 eipCount) {	    
#ifdef _DEBUG
    klog("setpriority not implemented");
#endif
    U32 result = 0;
    SYS_LOG1(SYSCALL_SYSTEM, cpu, "setpriority: which=%d, who=%d, prio=%d result=%d(0x%X) IGNORED\n", ARG1, ARG2, ARG3, result, result);
    return result;
}

static U32 syscall_statfs(CPU* cpu, U32 eipCount) {
    char tmp[MAX_FILEPATH_LEN];
    SYS_LOG1(SYSCALL_FILE, cpu, "statfs: path=%X(%s) buf=%X", ARG1, getNativeString(ARG1, tmp, sizeof(tmp)), ARG2);
    U32 result = cpu->thread->process->statfs(getNativeString(ARG1, tmp, sizeof(tmp)), ARG2);
    SYS_LOG(SYSCALL_FILE, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_ioperm(CPU* cpu, U32 eipCount) {	    
#ifdef _DEBUG
    klog("ioperm not implemented: from=0x%X len=%d on=%d", ARG1, ARG2, ARG3);
#endif
    U32 result = 0;
    SYS_LOG1(SYSCALL_SYSTEM, cpu, "ioperm: from=%X num=%d turn_on=%X result=%d(0x%X) IGNORED\n", ARG1, ARG2, ARG3, result, result);
    return result;
}

static U32 syscall_socketcall(CPU* cpu, U32 eipCount) {
    U32 result = 0;
    char tmp[MAX_FILEPATH_LEN];

    switch (ARG1) {
        case 1: // SYS_SOCKET
            SYS_LOG1(SYSCALL_SOCKET, cpu, "SYS_SOCKET: domain=%d(%s) type=%d(%s) protocol=%d(%s)", SARG2, SARG2==K_AF_UNIX?"AF_UNIX":(SARG2==K_AF_INET)?"AF_INET":"", (SARG3 & 0xFF), (SARG3 & 0xFF)==K_SOCK_STREAM?"SOCK_STREAM":((SARG3 & 0xFF)==K_SOCK_DGRAM)?"AF_SOCK_DGRAM":"", SARG4, (SARG4 == 0)?"IPPROTO_IP":(SARG4==6)?"IPPROTO_TCP":(SARG4==17)?"IPPROTO_UDP":"");
            result = ksocket(SARG2, SARG3 & 0xFF, SARG4);
            break;
        case 2: // SYS_BIND
            SYS_LOG1(SYSCALL_SOCKET, cpu, "SYS_BIND: socket=%d address=%X(%s) len=%d", SARG2, SARG3, socketAddressName(SARG3, SARG4, tmp, sizeof(tmp)), SARG4);
            result = kbind(SARG2, SARG3, SARG4);
            break;
        case 3: // SYS_CONNECT
            SYS_LOG1(SYSCALL_SOCKET, cpu, "SYS_CONNECT: socket=%d address=%X(%s) len=%d", SARG2, SARG3, socketAddressName(SARG3, SARG4, tmp, sizeof(tmp)), SARG4);
            result = kconnect(SARG2, SARG3, SARG4);
            break;
        case 4: // SYS_LISTEN				
            SYS_LOG1(SYSCALL_SOCKET, cpu, "SYS_LISTEN: socket=%d backlog=%d", SARG2, SARG3);
            result = klisten(SARG2, SARG3);
            break;
        case 5: // SYS_ACCEPT
            SYS_LOG1(SYSCALL_SOCKET, cpu, "SYS_ACCEPT: socket=%d address=%X(%s) len=%d", SARG2, SARG3, socketAddressName(SARG3, SARG4, tmp, sizeof(tmp)), SARG4);
            result = kaccept(SARG2, SARG3, SARG4, 0);
            break;			
        case 6: // SYS_GETSOCKNAME
            SYS_LOG1(SYSCALL_SOCKET, cpu, "SYS_GETSOCKNAME: socket=%d address=%X len=%d", SARG2, SARG3, SARG4);
            result = kgetsockname(SARG2, SARG3, SARG4);
            break;			
        case 7: // SYS_GETPEERNAME
            SYS_LOG1(SYSCALL_SOCKET, cpu, "SYS_GETPEERNAME: socket=%d address=%X len=%d", SARG2, SARG3, SARG4);
            result = kgetpeername(SARG2, SARG3, SARG4);
            break;		
        case 8: // SYS_SOCKETPAIR
            SYS_LOG1(SYSCALL_SOCKET, cpu, "SYS_SOCKETPAIR: af=%d(%s) type=%d(%s) socks=%X", SARG2, SARG2==K_AF_UNIX?"AF_UNIX":(SARG2==K_AF_INET)?"AF_INET":"", SARG3, SARG3==K_SOCK_STREAM?"SOCK_STREAM":(SARG3==K_SOCK_DGRAM)?"AF_SOCK_DGRAM":"", SARG5);
            result = ksocketpair(SARG2, SARG3, SARG4, SARG5, 0);
            break;
        case 9: // SYS_SEND
            SYS_LOG1(SYSCALL_SOCKET, cpu, "SYS_SEND: socket=%d buffer=%X len=%d flags=%X", SARG2, SARG3, SARG4, SARG5);
            result = ksend(SARG2, SARG3, SARG4, SARG5);
            break;
        case 10: // SYS_RECV
            SYS_LOG1(SYSCALL_SOCKET, cpu, "SYS_RECV: socket=%d buffer=%X len=%d flags=%X", SARG2, SARG3, SARG4, SARG5);
            result = krecv(SARG2, SARG3, SARG4, SARG5);
            break;
        case 11: // SYS_SENDTO
            SYS_LOG1(SYSCALL_SOCKET, cpu, "SYS_SENDTO: socket=%d buffer=%X len=%d flags=%X dest=%s", SARG2, SARG3, SARG4, SARG5, socketAddressName(SARG6, SARG7, tmp, sizeof(tmp)));
            result = ksendto(SARG2, SARG3, SARG4, SARG5, SARG6, SARG7);
            break;
        case 12: // SYS_RECVFROM
            SYS_LOG1(SYSCALL_SOCKET, cpu, "SYS_RECVFROM: socket=%d buffer=%X len=%d flags=%X address=%s", SARG2, SARG3, SARG4, SARG5, socketAddressName(SARG6, SARG7, tmp, sizeof(tmp)));
            result = krecvfrom(SARG2, SARG3, SARG4, SARG5, SARG6, SARG7);
            break;
        case 13: // SYS_SHUTDOWN
            SYS_LOG1(SYSCALL_SOCKET, cpu, "SYS_SHUTDOWN: socket=%d how=%d", SARG2, SARG3);
            result = kshutdown(SARG2, SARG3);
            break;
        case 14: // SYS_SETSOCKOPT
            SYS_LOG1(SYSCALL_SOCKET, cpu, "SYS_SETSOCKOPT: socket=%d level=%d name=%d value=%d, len=%d", SARG2, SARG3, SARG4, SARG5, SARG6);
            result = ksetsockopt(SARG2, SARG3, SARG4, SARG5, SARG6);
            break;
        case 15: // SYS_GETSOCKOPT
            SYS_LOG1(SYSCALL_SOCKET, cpu, "SYS_GETSOCKOPT: socket=%d level=%d name=%d value=%d, len=%d", SARG2, SARG3, SARG4, SARG5, SARG6);
            result = kgetsockopt(SARG2, SARG3, SARG4, SARG5, SARG6);
            break;		
        case 16: // SYS_SENDMSG
            SYS_LOG1(SYSCALL_SOCKET, cpu, "SYS_SENDMSG: socket=%d message=%X flags=%X", SARG2, SARG3, SARG4);
            result = ksendmsg(SARG2, SARG3, SARG4);
            break;
        case 17: // SYS_RECVMSG
            SYS_LOG1(SYSCALL_SOCKET, cpu, "SYS_RECVMSG: socket=%d message=%X flags=%X", SARG2, SARG3, SARG4);
            result = krecvmsg(SARG2, SARG3, SARG4);
            break;
        //case 18: // SYS_ACCEPT4
        default:
            kpanic("Unknown socket syscall: %d",ARG1);
    }
    SYS_LOG(SYSCALL_SOCKET, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_setitimer(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_SIGNAL, cpu, "setitimer :which=%d newValue=%d(%d.%.06d) oldValue=%d", ARG1, ARG2, (ARG2?readd(ARG2+8):0), (ARG2?readd(ARG2+12):0), ARG3);
    U32 result = cpu->thread->process->setitimer(ARG1, ARG2, ARG3);
    SYS_LOG(SYSCALL_SIGNAL, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_iopl(CPU* cpu, U32 eipCount) {    
    U32 result = 0;
    SYS_LOG1(SYSCALL_SIGNAL, cpu, "iopl: level=%1 result=%d(0x%X) IGNORED\n", ARG1, result, result);
    return result;
}

static U32 syscall_wait4(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_PROCESS, cpu, "wait4: pid=%d status=%d options=%x rusage=%X", ARG1, ARG2, ARG3, ARG4);
#ifdef _DEBUG
        if (ARG4) {
            kwarn("__NR_wait4 rusuage not implemented");
        }
#endif
    U32 result = KSystem::waitpid((S32)ARG1, ARG2, ARG3);
    SYS_LOG(SYSCALL_PROCESS, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_sysinfo(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_SYSTEM, cpu, "sysinfo: address=%X", ARG1);
    U32 result = KSystem::sysinfo(ARG1);
    SYS_LOG(SYSCALL_SYSTEM, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_ipc(CPU* cpu, U32 eipCount) {
    U32 result;

    // ARG5 holds the pointer to be copied
    if (ARG1 == 21) { // IPCOP_shmat
        SYS_LOG1(SYSCALL_SYSTEM|SYSCALL_MEMORY, cpu, "ipc: IPCOP_shmat shmid=%d shmaddr=%d shmflg=%X", ARG2, ARG5, ARG3);
        result = KSystem::shmat(ARG2, ARG5, ARG3, ARG4);
    }  else if (ARG1 == 22) { // IPCOP_shmdt
        SYS_LOG1(SYSCALL_SYSTEM|SYSCALL_MEMORY, cpu, "ipc IPCOP_shmdt shmaddr=%d", ARG5);
        result = KSystem::shmdt(ARG5);
    } else if (ARG1 == 23) { // IPCOP_shmget
        //result = -1; // :TODO: this crashes hsetroot
        SYS_LOG1(SYSCALL_SYSTEM|SYSCALL_MEMORY, cpu, "ipc: IPCOP_shmget key=%d size=%d flags=%X", ARG2, ARG3, ARG4);
        result = KSystem::shmget(ARG2, ARG3, ARG4);
    } else if (ARG1 == 24) { // IPCOP_shmctl 
        SYS_LOG1(SYSCALL_SYSTEM|SYSCALL_MEMORY, cpu, "ipc: IPCOP_shmctl shmid=%d cmd=%d buf=%X", ARG2, ARG3, ARG5);
        result = KSystem::shmctl(ARG2, ARG3, ARG5);
    } else {
        kpanic("__NR_ipc op %d not implemented", ARG1);
        result = 0;
    }
    SYS_LOG(SYSCALL_SYSTEM, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_fsync(CPU* cpu, U32 eipCount) {    
    U32 result = 0;
    SYS_LOG1(SYSCALL_SYSTEM, cpu, "fsync: fd=%d result=%d(0x%X) IGNORED\n", ARG1, result, result);
    return result;
}

static U32 syscall_sigreturn(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_SIGNAL, cpu, "sigreturn:");
    U32 result = cpu->thread->sigreturn();
    SYS_LOG(SYSCALL_SIGNAL, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_clone(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_SYSTEM, cpu, "clone: flags=%X child_stack=%X ptid=%X tls=%X ctid=%X", ARG1, ARG2, ARG3, ARG4, ARG5);
    U32 result = cpu->thread->process->clone(ARG1, ARG2, ARG3, ARG4, ARG5);
    SYS_LOG(SYSCALL_SYSTEM, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_uname(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_SYSTEM, cpu, "uname: name=%.8X", ARG1);
    U32 result = KSystem::uname(ARG1);
    SYS_LOG(SYSCALL_SYSTEM, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_modify_ldt(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_PROCESS, cpu, "modify_ldt: func=%d ptr=%X(index=%d address=%X limit=%X flags=%X) count=%d", ARG1, ARG2, readd(ARG2),  readd(ARG2+4), readd(ARG2+8), readd(ARG2+12), ARG3);
    U32 result = cpu->thread->modify_ldt(ARG1, ARG2, ARG3);
    SYS_LOG(SYSCALL_PROCESS, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_mprotect(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_MEMORY, cpu, "mprotect: address=%X len=%d prot=%X", ARG1, ARG2, ARG3);
    U32 result = cpu->thread->process->mprotect(ARG1, ARG2, ARG3);
    SYS_LOG(SYSCALL_MEMORY, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_fchdir(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_PROCESS, cpu, "fchdir: fd=%d", ARG1);
    U32 result = cpu->thread->process->fchdir(ARG1);
    SYS_LOG(SYSCALL_PROCESS, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_llseek(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_FILE, cpu, "llseek: fildes=%d offset=%.8X%.8X pResult=%X whence=%d", ARG1, ARG2, ARG3, ARG4, ARG5);
    S64 r64 = cpu->thread->process->llseek(ARG1, ((U64)ARG2)<<32|ARG3, ARG5);
    if (ARG4 && r64>=0) {
        writeq(ARG4, r64);
    }
    U32 result;
    if (r64<0) {
        result = (U32)r64;
    } else {
        result = 0;
    }
    SYS_LOG(SYSCALL_FILE, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_getdents(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_FILE, cpu, "getdents: fd=%d dir=%X count=%d", ARG1, ARG2, ARG3);
    U32 result = cpu->thread->process->getdents(ARG1, ARG2, ARG3, false);
    SYS_LOG(SYSCALL_FILE, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_newselect(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_PROCESS, cpu, "newselect: nfd=%d readfds=%X writefds=%X errorfds=%X timeout=%d", ARG1, ARG2, ARG3, ARG4, ARG5);
    U32 result = kselect(ARG1, ARG2, ARG3, ARG4, ARG5);
    SYS_LOG(SYSCALL_PROCESS, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_flock(CPU* cpu, U32 eipCount) {    
#ifdef _DEBUG
    klog("flock not implemented");
#endif
    U32 result = 0;
    SYS_LOG1(SYSCALL_FILE, cpu, "flock: fd=%d operation=%d result=%d(0x%X) IGNORED\n", ARG1, ARG2, result, result);
    return result;
}

static U32 syscall_msync(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_MEMORY, cpu, "msync addr=%X length=%d flags=%X", ARG1, ARG2, ARG3);
    U32 result = cpu->thread->process->msync(ARG1, ARG2, ARG3);
    SYS_LOG(SYSCALL_MEMORY, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_writev(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_WRITE, cpu, "writev: filds=%d iov=0x%X iovcn=%d", ARG1, ARG2, ARG3);    
    U32 result = cpu->thread->process->writev(ARG1, ARG2, ARG3);
    SYS_LOG(SYSCALL_WRITE, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_fdatasync(CPU* cpu, U32 eipCount) {    
    U32 result = 0;
    SYS_LOG1(SYSCALL_SYSTEM, cpu, "fdatasync: fd=%d result=%d(0x%X) IGNORED\n", ARG1, result, result);
    return result;
}

static U32 syscall_mlock(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_MEMORY, cpu, "mlock: address=0x%X len=%d", ARG1, ARG2);
    U32 result = cpu->thread->process->mlock(ARG1, ARG2);
    SYS_LOG(SYSCALL_MEMORY, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_sched_getparam(CPU* cpu, U32 eipCount) {    
    U32 result = -K_EPERM;
    SYS_LOG1(SYSCALL_SYSTEM, cpu, "sched_getparam: pid=%d params=%X result=%d(0x%X) IGNORED\n", ARG1, ARG2, result, result);
    return result;
}

static U32 syscall_sched_getscheduler(CPU* cpu, U32 eipCount) {    
    U32 result = 0; // SCHED_OTHER
    SYS_LOG1(SYSCALL_SYSTEM, cpu, "sched_getscheduler: pid=%d params=%X result=%d(0x%X) IGNORED\n", ARG1, ARG2, result, result);
    return result;
}

static U32 syscall_sched_yield(CPU* cpu, U32 eipCount) {    
    cpu->yield = true;
    U32 result = 0;
    std::this_thread::yield();
    SYS_LOG1(SYSCALL_SYSTEM, cpu, "yield: result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_sched_get_priority_max(CPU* cpu, U32 eipCount) {    
    U32 result = 32;
    SYS_LOG1(SYSCALL_SYSTEM, cpu, "sched_get_priority_max: policy=%d result=%d(0x%X)\n", ARG1, result, result);
    return result;
}

static U32 syscall_sched_get_priority_min(CPU* cpu, U32 eipCount) {
    U32 result = 0;
    SYS_LOG1(SYSCALL_SYSTEM, cpu, "sched_get_priority_min: policy=%d result=%d(0x%X)\n", ARG1, result, result);
    return result;
}

static U32 syscall_clock_nanosleep(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_THREAD, cpu, "clock_nanosleep: clock=%d flags=%x req=%X(%d.%.09d sec) remaining=%X", ARG1, ARG2, ARG3, readd(ARG3), readd(ARG3 + 4), ARG4);
    U32 result = cpu->thread->clockNanoSleep(ARG1, ARG2, ((U64)readd(ARG3)) * 1000000000l + readd(ARG3 + 4), ARG4);
    SYS_LOG(SYSCALL_THREAD, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_clock_nanosleep_time64(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_THREAD, cpu, "clock_nanosleep_time64: clock=%d flags=%x req=%X(%d.%.09d sec) remaining=%X", ARG1, ARG2, ARG3, readd(ARG3), readd(ARG3 + 4), ARG4);
    U32 result = cpu->thread->clockNanoSleep(ARG1, ARG2, ((U64)readq(ARG3)) * 1000000000l + readd(ARG3 + 8), ARG4);
    SYS_LOG(SYSCALL_THREAD, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_nanosleep(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_THREAD, cpu, "nanosleep: req=%X(%d.%.09d sec)", ARG1, readd(ARG1), readd(ARG1+4));
    U32 result = cpu->thread->nanoSleep(((U64)readd(ARG1))*1000000000l+readd(ARG1+4));
    SYS_LOG(SYSCALL_THREAD, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_mremap(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_MEMORY, cpu, "mremap: oldaddress=%x oldsize=%d newsize=%d flags=%X", ARG1, ARG2, ARG3, ARG4);
    U32 result = cpu->thread->process->mremap(ARG1, ARG2, ARG3, ARG4);
    SYS_LOG(SYSCALL_MEMORY, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_vm86(CPU* cpu, U32 eipCount) {
    kpanic("Application tried to enter DOS mode (vm86).  BoxedWine does not support this.");
    return 0;
}

static U32 syscall_poll(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_PROCESS, cpu, "poll: pfds=%X nfds=%d timeout=%X", ARG1, ARG2, ARG3);
    U32 result = kpoll(ARG1, ARG2, ARG3);		
    SYS_LOG(SYSCALL_PROCESS, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_prctl(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_PROCESS, cpu, "prctl: options=%d", ARG1);
    U32 result = cpu->thread->process->prctl(ARG1, ARG2);
    SYS_LOG(SYSCALL_PROCESS, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_rt_sigaction(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_SIGNAL, cpu, "rt_sigaction: sig=%d act=%X oact=%X", ARG1, ARG2, ARG3);
    U32 result = cpu->thread->process->sigaction(ARG1, ARG2, ARG3, ARG4);
    SYS_LOG(SYSCALL_SIGNAL, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_rt_sigprocmask(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_SIGNAL, cpu, "rt_sigprocmask: how=%d set=%X oset=%X", ARG1, ARG2, ARG3);
    U32 result = cpu->thread->sigprocmask(ARG1, ARG2, ARG3, ARG4);
    EAX = result;
    cpu->eip.u32+=eipCount;
    cpu->thread->runSignals();
    SYS_LOG(SYSCALL_SIGNAL, cpu, " result=%d(0x%X)\n", result, result);
    return -K_CONTINUE;
}

static U32 syscall_rt_sigsuspend(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_SIGNAL, cpu, "rt_sigsuspend: mask=%X", ARG1);
    U32 result = cpu->thread->sigsuspend(ARG1, ARG2);
    SYS_LOG(SYSCALL_SIGNAL, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_pread64(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_READ, cpu, "pread64: fd=%d buf=%X len=%d offset=%d", ARG1, ARG2, ARG3, ARG4);
    U32 result = cpu->thread->process->pread64(ARG1, ARG2, ARG3, ARG4 | ((U64)ARG5) << 32);
    SYS_LOG(SYSCALL_READ, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_pwrite64(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_WRITE, cpu, "pwrite64: fd=%d buf=%X len=%d offset=%d", ARG1, ARG2, ARG3, ARG4);
    U32 result = cpu->thread->process->pwrite64(ARG1, ARG2, ARG3, ARG4 | ((U64)ARG5) << 32);
    SYS_LOG(SYSCALL_WRITE, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_getcwd(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_PROCESS, cpu, "getcwd: buf=%X size=%d (%s)", ARG1, ARG2, cpu->thread->process->currentDirectory.c_str());
    U32 result = cpu->thread->process->getcwd(ARG1, ARG2);
    SYS_LOG(SYSCALL_PROCESS, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_sigaltstack(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_SIGNAL, cpu, "sigaltstack ss=%X oss=%X", ARG1, ARG2);
    U32 result = cpu->thread->signalstack(ARG1, ARG2);
    SYS_LOG(SYSCALL_SIGNAL, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_vfork(CPU* cpu, U32 eipCount) {    
    U32 result = cpu->thread->process->clone(0x01000000 |0x00200000 | 0x00004000, 0, 0, 0, 0);
    SYS_LOG1(SYSCALL_SYSTEM, cpu, "vfork: result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_ugetrlimit(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_SYSTEM, cpu, "ugetrlimit: resource=%d rlim=%X", ARG1, ARG2);
    U32 result = KSystem::ugetrlimit(ARG1, ARG2);
    SYS_LOG(SYSCALL_SYSTEM, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_mmap2(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_MEMORY, cpu, "mmap2: address=%.8X len=0x%X(%d) prot=%X flags=%X fd=%d offset=%d", ARG1, ARG2, ARG2, ARG3, ARG4, ARG5, ARG6);
    U32 result = cpu->thread->process->mmap(ARG1, ARG2, ARG3, ARG4, ARG5, ARG6*4096l);
    SYS_LOG(SYSCALL_MEMORY, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_ftruncate64(CPU* cpu, U32 eipCount) {
    U64 len = ARG2 | ((U64)ARG3 << 32);
    SYS_LOG1(SYSCALL_FILE, cpu, "ftruncate64: fildes=%d length=%llu", ARG1, len);
    U32 result = cpu->thread->process->ftruncate64(ARG1, len);
    SYS_LOG(SYSCALL_FILE, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_stat64(CPU* cpu, U32 eipCount) {    
    char tmp[MAX_FILEPATH_LEN];
    SYS_LOG1(SYSCALL_FILE, cpu, "stat64: path=%s buf=%X", getNativeString(ARG1, tmp, sizeof(tmp)), ARG2);
    U32 result = cpu->thread->process->stat64(getNativeString(ARG1, tmp, sizeof(tmp)), ARG2);
    SYS_LOG(SYSCALL_FILE, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_lstat64(CPU* cpu, U32 eipCount) {
    char tmp[MAX_FILEPATH_LEN];
    SYS_LOG1(SYSCALL_FILE, cpu, "lstat64: path=%s buf=%X", getNativeString(ARG1, tmp, sizeof(tmp)), ARG2);
    U32 result = cpu->thread->process->lstat64(getNativeString(ARG1, tmp, sizeof(tmp)), ARG2);
    SYS_LOG(SYSCALL_FILE, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_fstat64(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_FILE, cpu, "fstat64: fildes=%d buf=%X", ARG1, ARG2);
    U32 result = cpu->thread->process->fstat64(ARG1, ARG2);
    SYS_LOG(SYSCALL_FILE, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_lchown32(CPU* cpu, U32 eipCount) {
    char tmp[MAX_FILEPATH_LEN];
    U32 result = 0;
    SYS_LOG1(SYSCALL_FILE, cpu, "lchown32: path=%s owner=%d group=%d result=%d(0x%X) IGNORED\n", getNativeString(ARG1, tmp, sizeof(tmp)), ARG2, ARG3, result, result);
    return result;
}

static U32 syscall_getuid32(CPU* cpu, U32 eipCount) {
    U32 result = cpu->thread->process->userId;
    SYS_LOG1(SYSCALL_SYSTEM, cpu, "getuid32: result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_getgid32(CPU* cpu, U32 eipCount) {
    U32 result = cpu->thread->process->groupId;
    SYS_LOG1(SYSCALL_SYSTEM, cpu, "getgid32: result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_geteuid32(CPU* cpu, U32 eipCount) {
    U32 result = cpu->thread->process->effectiveUserId;
    SYS_LOG1(SYSCALL_SYSTEM, cpu, "geteuid32: result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_getegid32(CPU* cpu, U32 eipCount) {
    U32 result = cpu->thread->process->effectiveGroupId;
    SYS_LOG1(SYSCALL_SYSTEM, cpu, "getegid32: result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_getgroups32(CPU* cpu, U32 eipCount) {
    if (ARG2!=0) {
        writed(ARG2, cpu->thread->process->groupId);
    }
    U32 result = 1;
    SYS_LOG1(SYSCALL_SYSTEM, cpu, "getgroups32: size=%d list=%X result=%d(0x%X)\n", ARG1, ARG2, result, result);
    return result;
}

static U32 syscall_setgroups32(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_SYSTEM, cpu, "setgroups32: size=%d list=%X", ARG1, ARG2);
    if (ARG1) {
        cpu->thread->process->groupId = readd(ARG2);
    }
    U32 result = 0;
    SYS_LOG(SYSCALL_SYSTEM, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_fchown32(CPU* cpu, U32 eipCount) {
    U32 result = 0;
    SYS_LOG1(SYSCALL_SYSTEM, cpu, "fchown32: fd=%d owner=%d group=%d result=%d(0x%X) IGNORED\n", ARG1, ARG2, ARG3, result, result);
    return result;
}

static U32 syscall_setresuid32(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_SYSTEM, cpu, "setresuid3: ruid=%d euid=%d suid=%d", ARG1, ARG2, ARG3);
    if (ARG1!=0xFFFFFFFF)
        cpu->thread->process->userId = ARG1;
    if (ARG2!=0xFFFFFFFF)
        cpu->thread->process->effectiveUserId = ARG2;
    U32 result = 0;
    SYS_LOG(SYSCALL_SYSTEM, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_getresuid32(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_SYSTEM, cpu, "getresuid32: ruid=%X(%d) euid=%X(%d) suid=%X(%d)", ARG1, cpu->thread->process->userId, ARG2, cpu->thread->process->effectiveUserId, ARG3, cpu->thread->process->userId);
    if (ARG1)
        writed(ARG1, cpu->thread->process->userId);
    if (ARG2)
        writed(ARG2, cpu->thread->process->effectiveUserId);
    if (ARG3)
        writed(ARG3, cpu->thread->process->userId);
    U32 result = 0;
    SYS_LOG(SYSCALL_SYSTEM, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_setresgid32(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_SYSTEM, cpu, "setresgid32: rgid=%d egid=%d sgid=%d", ARG1, ARG2, ARG3);
    if (ARG1!=0xFFFFFFFF)
        cpu->thread->process->groupId = ARG1;
    if (ARG2!=0xFFFFFFFF)
        cpu->thread->process->effectiveGroupId = ARG2;
    U32 result = 0;
    SYS_LOG(SYSCALL_SYSTEM, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_getresgid32(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_SYSTEM, cpu, "getresgid32: rgid=%X(%d) egid=%X(%d) sgid=%X(%d)", ARG1, cpu->thread->process->groupId, ARG2, cpu->thread->process->groupId, ARG3, cpu->thread->process->groupId);
    if (ARG1)
        writed(ARG1, cpu->thread->process->groupId);
    if (ARG2)
        writed(ARG2, cpu->thread->process->effectiveGroupId);
    if (ARG3)
        writed(ARG3, cpu->thread->process->groupId);
    U32 result = 0;
    SYS_LOG(SYSCALL_SYSTEM, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_chown32(CPU* cpu, U32 eipCount) {
    char tmp[MAX_FILEPATH_LEN];
    SYS_LOG1(SYSCALL_FILE, cpu, "chown32: path=%s owner=%d group=%d", getNativeString(ARG1, tmp, sizeof(tmp)), ARG2, ARG3);
    U32 result = 0;
    SYS_LOG(SYSCALL_FILE, cpu, " result=%d(0x%X) IGNORED\n", result, result);
    return result;
}

static U32 syscall_setuid32(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_SYSTEM, cpu, "setuid32: uid=%d", ARG1);
    cpu->thread->process->effectiveUserId = ARG1;
    U32 result = 0;
    SYS_LOG(SYSCALL_SYSTEM, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_setgid32(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_SYSTEM, cpu, "setgid32: gid=%d", ARG1);
    cpu->thread->process->groupId = ARG1;
    U32 result = 0;
    SYS_LOG(SYSCALL_SYSTEM, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_mincore(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_PROCESS, cpu, "mincore: address=%X length=%d vec=%X", ARG1, ARG2, ARG3);
    U32 result = cpu->thread->process->mincore(ARG1, ARG2, ARG3);
    SYS_LOG(SYSCALL_PROCESS, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_madvise(CPU* cpu, U32 eipCount) {    
    U32 result = 0;
    SYS_LOG1(SYSCALL_MEMORY, cpu, "madvise: address=%X len=%d advise=%d result=%d(0x%X) IGNORED\n", ARG1, ARG2, ARG3, result, result);
    return result;
}

static U32 syscall_getdents64(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_FILE, cpu, "getdents64: fd=%d dir=%X count=%d", ARG1, ARG2, ARG3);
    U32 result = cpu->thread->process->getdents(ARG1, ARG2, ARG3, true);
    SYS_LOG(SYSCALL_FILE, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_fcntl64(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_FILE, cpu, "fcntl64: fildes=%d cmd=%d arg=%d", ARG1, ARG2, ARG3);
    U32 result = cpu->thread->process->fcntrl(ARG1, ARG2, ARG3);
    SYS_LOG(SYSCALL_FILE, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_gettid(CPU* cpu, U32 eipCount) {    
    U32 result = cpu->thread->id;
    SYS_LOG1(SYSCALL_SYSTEM, cpu, "gettid: result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_fsetxattr(CPU* cpu, U32 eipCount) {    
    U32 result = -K_ENOTSUP;
    SYS_LOG1(SYSCALL_SYSTEM, cpu, "fsetxattr: result = ENOTSUP IGNORED");
    return result;
}

static U32 syscall_getxattr(CPU* cpu, U32 eipCount) {
    char tmp[MAX_FILEPATH_LEN];
    char tmp2[MAX_FILEPATH_LEN];
    U32 result = -K_ENOTSUP;
    SYS_LOG1(SYSCALL_SYSTEM, cpu, "getxattr: path=%s name=%s result = ENOTSUP IGNORED", getNativeString(ARG1, tmp, sizeof(tmp)), getNativeString(ARG2, tmp2, sizeof(tmp2)));
    return result;
}

static U32 syscall_lgetxattr(CPU* cpu, U32 eipCount) {
    char tmp[MAX_FILEPATH_LEN];
    char tmp2[MAX_FILEPATH_LEN];
    U32 result = -K_ENOTSUP;
    SYS_LOG1(SYSCALL_SYSTEM, cpu, "lgetxattr: path=%s name=%s result = ENOTSUP IGNORED", getNativeString(ARG1, tmp, sizeof(tmp)), getNativeString(ARG2, tmp2, sizeof(tmp2)));
    return result;
}

static U32 syscall_fgetxattr(CPU* cpu, U32 eipCount) {
    U32 result = -K_ENOTSUP;
    SYS_LOG1(SYSCALL_SYSTEM, cpu, "fgetxattr: result = ENOTSUP IGNORED");
    return result;
}

static U32 syscall_flistxattr(CPU* cpu, U32 eipCount) {
    U32 result = -K_ENOTSUP;
    SYS_LOG1(SYSCALL_SYSTEM, cpu, "flistxattr: result = ENOTSUP IGNORED");
    return result;
}

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_WAIT_PRIVATE 128
#define FUTEX_WAKE_PRIVATE 129
#define FUTEX_WAIT_BITSET_PRIVATE 137
#define FUTEX_WAKE_BITSET_PRIVATE 138

static const char* getFutexOp(U32 op) {
    if (op==0) return "WAIT";
    if (op==1) return "WAKE";
    if (op==128) return "WAIT PRIVATE";
    if (op==129) return "WAKE PRIVATE";
    if (op == 137) return "WAIT BITSET PRIVATE";
    if (op == 138) return "WAKE BITSET PRIVATE";
    static std::string tmp;
    tmp = std::to_string(op);
    return tmp.c_str();
}

static U32 syscall_futex(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_FUTEX, cpu, "futex start: address=%X op=%s value=%d\n", ARG1, getFutexOp(ARG2), ARG3);
    U32 result = cpu->thread->futex(ARG1, ARG2, ARG3, ARG4, ARG5, ARG6);
    SYS_LOG1(SYSCALL_FUTEX, cpu, "futex   end: address=%X op=%s value=%d result=%d(0x%X)\n", ARG1, getFutexOp(ARG2), ARG3, result, result);
    return result;
}

static U32 syscall_sched_setaffinity(CPU* cpu, U32 eipCount) {    
    U32 result = 0;
    SYS_LOG1(SYSCALL_SYSTEM, cpu, "sched_setaffinity: pid=%d cpusetsize=d cpu_set_t=%X result=%d(0x%X) IGNORED\n", ARG1, ARG2, ARG3, result, result);
    return result;
}

static U32 syscall_sched_getaffinity(CPU* cpu, U32 eipCount) {    
#ifdef _DEBUG
     kwarn("__NR_sched_getaffinity not implemented");
#endif
    U32 result = -K_EPERM;
    SYS_LOG1(SYSCALL_SYSTEM, cpu, "sched_getaffinity: pid=%d cpusetsize=%d mask=%X result=%d(0x%X) IGNORED\n", ARG1, ARG2, ARG3, result, result);
    return result;
}

static U32 syscall_set_thread_area(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_THREAD, cpu, "set_thread_area: u_info=%X", ARG1);
    U32 result = cpu->thread->process->set_thread_area(ARG1);
    SYS_LOG(SYSCALL_THREAD, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_exit_group(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_SYSTEM, cpu, "exit_group: code=%d", ARG1);
    U32 result = cpu->thread->process->exitgroup(ARG1);		
    SYS_LOG(SYSCALL_SYSTEM, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_epoll_create(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_PROCESS, cpu, "epoll_create: size=%d", ARG1);
    U32 result = cpu->thread->process->epollcreate(ARG1, 0);
    SYS_LOG(SYSCALL_PROCESS, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_epoll_ctl(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_PROCESS, cpu, "epoll_ctl: epfd=%d op=%d fd=%d events=%X", ARG1, ARG2, ARG3, ARG4);
    U32 result = cpu->thread->process->epollctl(ARG1, ARG2, ARG3, ARG4);
    SYS_LOG(SYSCALL_PROCESS, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_epoll_wait(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_PROCESS|SYSCALL_THREAD, cpu, "epoll_wait: epfd=%d events=%X maxevents=%d timeout=%d", ARG1, ARG2, ARG3, ARG4);
    U32 result = cpu->thread->process->epollwait(ARG1, ARG2, ARG3, ARG4);
    SYS_LOG(SYSCALL_PROCESS|SYSCALL_THREAD, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_set_tid_address(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_THREAD, cpu, "set_tid_address: address=%X", ARG1);
    cpu->thread->clear_child_tid = ARG1;
    U32 result = cpu->thread->id;
    SYS_LOG(SYSCALL_THREAD, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_clock_gettime(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_SYSTEM, cpu, "clock_gettime: clock_id=%d tp=%X", ARG1, ARG2);
    U32 result = KSystem::clock_gettime(ARG1, ARG2);
    SYS_LOG(SYSCALL_SYSTEM, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_clock_gettime64(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_SYSTEM, cpu, "clock_gettime64: clock_id=%d tp=%X", ARG1, ARG2);
    U32 result = KSystem::clock_gettime64(ARG1, ARG2);
    SYS_LOG(SYSCALL_SYSTEM, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_clock_getres(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_SYSTEM, cpu, "clock_getres: clock_id=%d res=%X", ARG1, ARG2);
    U32 result = KSystem::clock_getres(ARG1, ARG2);
    SYS_LOG(SYSCALL_SYSTEM, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_clock_getres_time64(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_SYSTEM, cpu, "clock_getres_time64: clock_id=%d res=%X", ARG1, ARG2);
    U32 result = KSystem::clock_getres64(ARG1, ARG2);
    SYS_LOG(SYSCALL_SYSTEM, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_statfs64(CPU* cpu, U32 eipCount) {
    char tmp[MAX_FILEPATH_LEN];
    SYS_LOG1(SYSCALL_FILE, cpu, "fstatfs64: path=%X(%s) len=%d buf=%X", ARG1, getNativeString(ARG1, tmp, sizeof(tmp)), ARG2, ARG3);
    U32 result = cpu->thread->process->statfs64(getNativeString(ARG1, tmp, sizeof(tmp)), ARG3);
    SYS_LOG(SYSCALL_FILE, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_fstatfs64(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_FILE, cpu, "fstatfs64: fd=%d len=%d buf=%X", ARG1, ARG2, ARG3);
    U32 result = cpu->thread->process->fstatfs64(ARG1, ARG3);
    SYS_LOG(SYSCALL_FILE, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_tgkill(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_SIGNAL, cpu, "tgkill: threadGroupId=%d threadId=%d signal=%d", ARG1, ARG2, ARG3);
    U32 result = KSystem::tgkill(ARG1, ARG2, ARG3);
    SYS_LOG(SYSCALL_SIGNAL, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_utimes(CPU* cpu, U32 eipCount) {
    char tmp[MAX_FILEPATH_LEN];
    SYS_LOG1(SYSCALL_FILE, cpu, "utimes: fileName=%s times=%X", getNativeString(ARG1, tmp, sizeof(tmp)), ARG2);
    U32 result = cpu->thread->process->utimes(getNativeString(ARG1, tmp, sizeof(tmp)), ARG2);
    SYS_LOG(SYSCALL_FILE, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_fadvise64(CPU* cpu, U32 eipCount) {    
    U32 result = 0;
    SYS_LOG1(SYSCALL_FILE, cpu, "fadvise64_64: fd=%d result=%d(0x%X) IGNORED\n", ARG1, result, result);
    return result;
}

static U32 syscall_inotify_init(CPU* cpu, U32 eipCount) {    
    U32 result = -K_ENOSYS;
    SYS_LOG1(SYSCALL_FILE, cpu, "inotify_init: result=%d(0x%X) IGNORED\n", result, result);
    return result;
}

BoxedPtr<FsNode> findNode(BoxedPtr<FsNode> parent, const std::string& name) {
    if (parent->name==name) {
        return parent;
    }
    std::vector<BoxedPtr<FsNode> > children;
    parent->getAllChildren(children);
    for (auto& n : children) {
        BoxedPtr<FsNode> result = findNode(n, name);
        if (result) {
            return result;
        }
    }
    return NULL;
}

static U32 syscall_openat(CPU* cpu, U32 eipCount) {
    char tmp[MAX_FILEPATH_LEN];
    SYS_LOG1(SYSCALL_FILE, cpu, "openat: dirfd=%d name=%s flags=%x", ARG1, getNativeString(ARG2, tmp, sizeof(tmp)), ARG3);
    U32 result = cpu->thread->process->openat(ARG1, getNativeString(ARG2, tmp, sizeof(tmp)), ARG3);
#ifdef _DEBUG
    if (result>1000) {
        std::string filename = Fs::getFileNameFromPath(getNativeString(ARG2, tmp, sizeof(tmp)));
        BoxedPtr<FsNode> found = findNode(Fs::getNodeFromLocalPath("", "/", false), filename);
        if (!found) {
            printf("openat: dirfd=%d name=%s flags=%x result=%x\n", ARG1, getNativeString(ARG2, tmp, sizeof(tmp)), ARG3, result);
        }
    }
#endif
    SYS_LOG(SYSCALL_FILE, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_mkdirat(CPU* cpu, U32 eipCount) {
    char tmp[MAX_FILEPATH_LEN];
    SYS_LOG1(SYSCALL_FILE, cpu, "mkdirat: dirfd=%d path=%s mode=%x", ARG1, getNativeString(ARG2, tmp, sizeof(tmp)), ARG3);
    U32 result = cpu->thread->process->mkdirat(ARG1, getNativeString(ARG2, tmp, sizeof(tmp)), ARG3);
    SYS_LOG(SYSCALL_FILE, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_fchownat(CPU* cpu, U32 eipCount) {
    char tmp[MAX_FILEPATH_LEN];    
    U32 result = 0;
    SYS_LOG1(SYSCALL_FILE, cpu, "fchown32: pathname=%X(%s) owner=%d group=%d flags=%d result=%d(0x%X) IGNORED\n", ARG2, getNativeString(ARG2, tmp, sizeof(tmp)), ARG3, ARG4, ARG5, result, result);
    return result;
}

static U32 syscall_fstatat64(CPU* cpu, U32 eipCount) {
    char tmp[MAX_FILEPATH_LEN];
    SYS_LOG1(SYSCALL_FILE, cpu, "statat64: dirfd=%d path=%s buf=%X flags=%x", ARG1, getNativeString(ARG2, tmp, sizeof(tmp)), ARG3, ARG4);
    U32 result = cpu->thread->process->fstatat64(ARG1, getNativeString(ARG2, tmp, sizeof(tmp)), ARG3, ARG4);
    SYS_LOG(SYSCALL_FILE, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_unlinkat(CPU* cpu, U32 eipCount) {
    char tmp[MAX_FILEPATH_LEN];
    SYS_LOG1(SYSCALL_FILE, cpu, "unlinkat: dirfd=%d path=%s flags=%x", ARG1, getNativeString(ARG2, tmp, sizeof(tmp)), ARG3);
    U32 result = cpu->thread->process->unlinkat(ARG1, getNativeString(ARG2, tmp, sizeof(tmp)), ARG3);
    SYS_LOG(SYSCALL_FILE, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_symlinkat(CPU* cpu, U32 eipCount) {
    char tmp[MAX_FILEPATH_LEN];
    char tmp2[MAX_FILEPATH_LEN];
    SYS_LOG1(SYSCALL_FILE, cpu, "symlinkat: oldpath=%x(%s) dirfd=%d newpath=%X(%s)", ARG1, getNativeString(ARG1, tmp, sizeof(tmp)), ARG2, ARG3, getNativeString(ARG3, tmp2, sizeof(tmp2)));
    U32 result = cpu->thread->process->symlinkat(getNativeString(ARG1, tmp, sizeof(tmp)), ARG2, getNativeString(ARG3, tmp2, sizeof(tmp2)));
    SYS_LOG(SYSCALL_FILE, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_readlinkat(CPU* cpu, U32 eipCount) {
    char tmp[MAX_FILEPATH_LEN];    
    char tmp2[MAX_FILEPATH_LEN];    
    SYS_LOG1(SYSCALL_FILE, cpu, "readlinkat: dirfd=%d pathname=%X(%s) buf=%X(%s) bufsiz=%d", ARG1, ARG2, getNativeString(ARG2, tmp, sizeof(tmp)), ARG3, getNativeString(ARG3, tmp2, sizeof(tmp2)), ARG4);
    U32 result = cpu->thread->process->readlinkat(ARG1, getNativeString(ARG2, tmp, sizeof(tmp)), ARG3, ARG4);    
    SYS_LOG(SYSCALL_FILE, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_fchmodat(CPU* cpu, U32 eipCount) {
    char tmp[MAX_FILEPATH_LEN];    
    U32 result = 0;
    SYS_LOG1(SYSCALL_FILE, cpu, "fchmodat pathname=%X(%s) mode=%X flags=%X result=%d(0x%X) IGNORED\n", ARG2, getNativeString(ARG2, tmp, sizeof(tmp)), ARG3, ARG4, result, result);
    return result;
}

static U32 syscall_faccessat(CPU* cpu, U32 eipCount) {
    char tmp[MAX_FILEPATH_LEN];
    SYS_LOG1(SYSCALL_FILE, cpu, "faccessat dirfd=%X pathname=%X(%s) mode=%X flags=%X", ARG1, ARG2, getNativeString(ARG2, tmp, sizeof(tmp)), ARG3, ARG4);
    U32 result = cpu->thread->process->faccessat(ARG1, getNativeString(ARG2, tmp, sizeof(tmp)), ARG3, ARG4);
    SYS_LOG(SYSCALL_FILE, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_set_robust_list(CPU* cpu, U32 eipCount) {    
#ifdef _DEBUG
        //kwarn("syscall __NR_set_robust_list not implemented");
#endif
    U32 result = -K_ENOSYS;
    SYS_LOG1(SYSCALL_THREAD, cpu, "set_robust_list: result=%d(0x%X) IGNORED\n", result, result);
    return result;
}

static U32 syscall_sync_file_range(CPU* cpu, U32 eipCount) {    
    U32 result = 0;
    SYS_LOG1(SYSCALL_THREAD, cpu, "sync_file_range: result=%d(0x%X) IGNORED\n", result, result);
    return result;
}

static U32 syscall_utimensat(CPU* cpu, U32 eipCount) {
    char tmp[MAX_FILEPATH_LEN];
    SYS_LOG1(SYSCALL_FILE, cpu, "utimensat dirfd=%d path=%X(%s) times=%X flags=%X", ARG1, ARG2, getNativeString(ARG2, tmp, sizeof(tmp)), ARG3, ARG4);
    U32 result = cpu->thread->process->utimesat(ARG1, getNativeString(ARG2, tmp, sizeof(tmp)), ARG3, ARG4);
    SYS_LOG(SYSCALL_FILE, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_utimensat_time64(CPU* cpu, U32 eipCount) {
    char tmp[MAX_FILEPATH_LEN];
    SYS_LOG1(SYSCALL_FILE, cpu, "utimensat_time64 dirfd=%d path=%X(%s) times=%X flags=%X", ARG1, ARG2, getNativeString(ARG2, tmp, sizeof(tmp)), ARG3, ARG4);
    U32 result = cpu->thread->process->utimesat(ARG1, getNativeString(ARG2, tmp, sizeof(tmp)), ARG3, ARG4);
    SYS_LOG(SYSCALL_FILE, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_signalfd4(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_SIGNAL, cpu, "signalfd4 fd=%d mask=%X(0x%0.8X%0.8X) size=%d flags=%X", ARG1, ARG2, (ARG3>=8?readd(ARG2+4):0), readd(ARG2), ARG3, ARG4);
    U32 result = syscall_signalfd4(ARG1, ARG2, ARG3, ARG4);
    SYS_LOG(SYSCALL_SIGNAL, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_epoll_create1(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_PROCESS, cpu, "epoll_create1: falgs=%X", ARG1);
    U32 result = cpu->thread->process->epollcreate(0, ARG1);
    SYS_LOG(SYSCALL_PROCESS, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_pipe2(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_SOCKET, cpu, "pipe2 fildes=%X", ARG1);
    U32 result = ksocketpair(K_AF_UNIX, K_SOCK_STREAM, 0, ARG1, ARG2);
    SYS_LOG(SYSCALL_SOCKET, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_prlimit64(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_SYSTEM, cpu, "prlimit64 pid=%d resource=%d newlimit=%X oldlimit=%X", ARG1, ARG2, ARG3, ARG4);
    U32 result = KSystem::prlimit64(ARG1, ARG2, ARG3, ARG4);
    SYS_LOG(SYSCALL_SYSTEM, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_sendmmsg(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_SOCKET, cpu, "sendmmsg fd=%d address=%X vlen=%d flags=%X", ARG1, ARG2, ARG3, ARG4);
    U32 result = ksendmmsg(ARG1, ARG2, ARG3, ARG4);
    SYS_LOG(SYSCALL_SOCKET, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static U32 syscall_renameat(CPU* cpu, U32 eipCount) {
    char tmp[MAX_FILEPATH_LEN];
    char tmp2[MAX_FILEPATH_LEN];
    SYS_LOG1(SYSCALL_FILE, cpu, "renameat olddirfd=%d oldpath=%X(%s) newdirfd=%d newpath=%X(%s)", ARG1, ARG2, getNativeString(ARG2, tmp, sizeof(tmp)), ARG3, ARG4, getNativeString(ARG4, tmp2, sizeof(tmp2)));
    U32 result = cpu->thread->process->renameat(ARG1, getNativeString(ARG2, tmp, sizeof(tmp)), ARG3, getNativeString(ARG4, tmp2, sizeof(tmp2)));
    SYS_LOG(SYSCALL_FILE, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

std::mt19937 gen{std::random_device{}()};
std::uniform_int_distribution<size_t> dist{0, 255};

static U32 syscall_getrandom(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_SYSTEM, cpu, "getrandom buf=%X buflen=%d flags=%X", ARG1, ARG2, ARG3);
    U32 buf = ARG1;
    U32 count = ARG2;
    for (U32 i=0;i<count;i++) {
        writeb(buf+i, (U8)dist(gen));
    }
    SYS_LOG(SYSCALL_SYSTEM, cpu, " result=%d\n", count);
    return count;
}

static U32 syscall_memfd_create(CPU* cpu, U32 eipCount) {
    char tmp[MAX_FILEPATH_LEN];
    SYS_LOG1(SYSCALL_FILE, cpu, "name=%X(%s) flags=%X", ARG1, getNativeString(ARG1, tmp, sizeof(tmp)), ARG2);
    U32 result = cpu->thread->process->memfd_create(getNativeString(ARG1, tmp, sizeof(tmp)), ARG2);
    SYS_LOG(SYSCALL_FILE, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

U32 syscall_socket(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_SOCKET, cpu, "socket domain=%d(%s) type=%d(%s) protocol=%d(%s)", ARG1, ARG1==K_AF_UNIX?"AF_UNIX":(ARG1==K_AF_INET)?"AF_INET":"", (ARG2 & 0xFF), (ARG2 & 0xFF)==K_SOCK_STREAM?"SOCK_STREAM":((ARG2 & 0xFF)==K_SOCK_DGRAM)?"AF_SOCK_DGRAM":"", ARG3, (ARG3 == 0)?"IPPROTO_IP":(ARG3==6)?"IPPROTO_TCP":(ARG3==17)?"IPPROTO_UDP":"");
    U32 result = ksocket(ARG1, ARG2 & 0xFF, ARG3);
    SYS_LOG(SYSCALL_SOCKET, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

U32 syscall_socketpair(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_SOCKET, cpu, "socketpair af=%d(%s) type=%d(%s) socks=%X", ARG1, ARG1==K_AF_UNIX?"AF_UNIX":(ARG1==K_AF_INET)?"AF_INET":"", ARG2, ARG2==K_SOCK_STREAM?"SOCK_STREAM":(ARG2==K_SOCK_DGRAM)?"AF_SOCK_DGRAM":"", ARG4);
    U32 result = ksocketpair(ARG1, ARG2, ARG3, ARG4, 0);
    SYS_LOG(SYSCALL_SOCKET, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

U32 syscall_bind(CPU* cpu, U32 eipCount) {
    char tmp[MAX_FILEPATH_LEN];
    SYS_LOG1(SYSCALL_SOCKET, cpu, "bind socket=%d address=%X(%s) len=%d", ARG1, ARG2, socketAddressName(ARG2, ARG3, tmp, sizeof(tmp)), ARG3);
    U32 result = kbind(ARG1, ARG2, ARG3);
    SYS_LOG(SYSCALL_SOCKET, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

U32 syscall_connect(CPU* cpu, U32 eipCount) {
    char tmp[MAX_FILEPATH_LEN];
    SYS_LOG1(SYSCALL_SOCKET, cpu, "connect socket=%d address=%X(%s) len=%d", ARG1, ARG2, socketAddressName(ARG2, ARG3, tmp, sizeof(tmp)), ARG3);
    U32 result = kconnect(ARG1, ARG2, ARG3);
    SYS_LOG(SYSCALL_SOCKET, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

U32 syscall_listen(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_SOCKET, cpu, "listen socket=%d backlog=%d", ARG1, ARG2);
    U32 result = klisten(ARG1, ARG2);
    SYS_LOG(SYSCALL_SOCKET, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

U32 syscall_accept4(CPU* cpu, U32 eipCount) {
    char tmp[MAX_FILEPATH_LEN];
    SYS_LOG1(SYSCALL_SOCKET, cpu, "accept4 socket=%d address=%X(%s) len=%d flags=%X", ARG1, ARG2, socketAddressName(ARG2, ARG3, tmp, sizeof(tmp)), ARG3, ARG4);
    U32 result = kaccept(ARG1, ARG2, ARG3, ARG4);
    SYS_LOG(SYSCALL_SOCKET, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

U32 syscall_getsockopt(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_SOCKET, cpu, "getsockopt socket=%d level=%d name=%d value=%d, len=%d", ARG1, ARG2, ARG3, ARG4, ARG5);
    U32 result = kgetsockopt(ARG1, ARG2, ARG3, ARG4, ARG5);
    SYS_LOG(SYSCALL_SOCKET, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

U32 syscall_setsockopt(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_SOCKET, cpu, "setsockopt socket=%d level=%d name=%d value=%d, len=%d", ARG1, ARG2, ARG3, ARG4, ARG5);
    U32 result = ksetsockopt(ARG1, ARG2, ARG3, ARG4, ARG5);
    SYS_LOG(SYSCALL_SOCKET, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

U32 syscall_getsockname(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_SOCKET, cpu, "getsockname socket=%d address=%X len=%d", ARG1, ARG2, ARG3);
    U32 result = kgetsockname(ARG1, ARG2, ARG3);
    SYS_LOG(SYSCALL_SOCKET, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

U32 syscall_getpeername(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_SOCKET, cpu, "getpeername socket=%d address=%X len=%d", ARG1, ARG2, ARG3);
    U32 result = kgetpeername(ARG1, ARG2, ARG3);
    SYS_LOG(SYSCALL_SOCKET, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

U32 syscall_sendto(CPU* cpu, U32 eipCount) {
    char tmp[MAX_FILEPATH_LEN];
    SYS_LOG1(SYSCALL_SOCKET, cpu, "sendto socket=%d buffer=%X len=%d flags=%X dest=%s", ARG1, ARG2, ARG3, ARG4, socketAddressName(ARG5, ARG6, tmp, sizeof(tmp)));
    U32 result = ksendto(ARG1, ARG2, ARG3, ARG4, ARG5, ARG6);
    SYS_LOG(SYSCALL_SOCKET, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

U32 syscall_sendmsg(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_SOCKET, cpu, "sendmsg socket=%d message=%X flags=%X", ARG1, ARG2, ARG3);
    U32 result = ksendmsg(ARG1, ARG2, ARG3);
    SYS_LOG(SYSCALL_SOCKET, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

U32 syscall_recvfrom(CPU* cpu, U32 eipCount) {
    char tmp[MAX_FILEPATH_LEN];
    SYS_LOG1(SYSCALL_SOCKET, cpu, "recvfrom socket=%d buffer=%X len=%d flags=%X address=%s", ARG1, ARG2, ARG3, ARG4, socketAddressName(ARG5, ARG6, tmp, sizeof(tmp)));
    U32 result = krecvfrom(ARG1, ARG2, ARG3, ARG4, ARG5, ARG6);
    SYS_LOG(SYSCALL_SOCKET, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

U32 syscall_recvmsg(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_SOCKET, cpu, "recvmsg socket=%d message=%X flags=%X", ARG1, ARG2, ARG3);
    U32 result = krecvmsg(ARG1, ARG2, ARG3);
    SYS_LOG(SYSCALL_SOCKET, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

U32 syscall_shutdown(CPU* cpu, U32 eipCount) {
    SYS_LOG1(SYSCALL_SOCKET, cpu, "shutdown socket=%d how=%d", ARG1, ARG2);
    U32 result = kshutdown(ARG1, ARG2);
    SYS_LOG(SYSCALL_SOCKET, cpu, " result=%d(0x%X)\n", result, result);
    return result;
}

static const SyscallFunc syscallFunc[] = {
    0,                  // 0
    syscall_exit,       // 1 __NR_exit
    0,                  // 2
    syscall_read,       // 3 __NR_read
    syscall_write,      // 4 __NR_write
    syscall_open,       // 5 __NR_open
    syscall_close,      // 6 __NR_close
    syscall_waitpid,    // 7 __NR_waitpid
    0,                  // 8
    syscall_link,       // 9 __NR_link
    syscall_unlink,     // 10 __NR_unlink
    syscall_execve,     // 11 __NR_execve
    syscall_chdir,      // 12 __NR_chdir
    syscall_time,       // 13 __NR_time
    0,                  // 14
    syscall_chmod,      // 15 __NR_chmod
    0,                  // 16
    0,                  // 17
    0,                  // 18
    syscall_lseek,      // 19 __NR_lseek
    syscall_getpid,     // 20 __NR_getpid
    0,                  // 21
    0,                  // 22
    0,                  // 23
    syscall_getuid,     // 24 __NR_getuid
    0,                  // 25
    syscall_ptrace,     // 26 __NR_ptrace
    syscall_alarm,      // 27 __NR_alarm
    0,                  // 28
    0,                  // 29
    syscall_utime,      // 30 __NR_utime
    0,                  // 31
    0,                  // 32
    syscall_access,     // 33 __NR_access
    0,                  // 34
    0,                  // 35
    syscall_sync,       // 36 __NR_sync
    syscall_kill,       // 37 __NR_kill
    syscall_rename,     // 38 __NR_rename
    syscall_mkdir,      // 39 __NR_mkdir
    syscall_rmdir,      // 40 __NR_rmdir
    syscall_dup,        // 41 __NR_dup
    syscall_pipe,       // 42 __NR_pipe
    syscall_times,      // 43 __NR_times
    0,                  // 44
    syscall_brk,        // 45 __NR_brk
    0,                  // 46
    syscall_getgid,     // 47 __NR_getgid
    0,                  // 48
    syscall_geteuid,    // 49 __NR_geteuid
    syscall_getegid,    // 50 __NR_getegid
    0,                  // 51
    0,                  // 52
    0,                  // 53
    syscall_ioctl,      // 54 __NR_ioctl
    0,                  // 55
    0,                  // 56
    syscall_setpgid,    // 57 __NR_setpgid
    0,                  // 58
    0,                  // 59
    syscall_umask,      // 60 __NR_umask
    0,                  // 61
    0,                  // 62
    syscall_dup2,       // 63 __NR_dup2
    syscall_getppid,    // 64 __NR_getppid
    syscall_getpgrp,    // 65 __NR_getpgrp
    syscall_setsid,     // 66 __NR_setsid
    0,                  // 67
    0,                  // 68
    0,                  // 69
    0,                  // 70
    0,                  // 71
    0,                  // 72
    0,                  // 73
    0,                  // 74
    syscall_setrlimit,  // 75 __NR_setrlimit 
    0,                  // 76
    syscall_getrusuage, // 77 __NR_getrusage
    syscall_gettimeofday,// 78__NR_gettimeofday
    0,                  // 79
    0,                  // 80
    0,                  // 81
    0,                  // 82
    syscall_symlink,    // 83 __NR_symlink
    0,                  // 84
    syscall_readlink,   // 85 __NR_readlink
    0,                  // 86
    0,                  // 87
    0,                  // 88
    0,                  // 89
    syscall_mmap64,     // 90 __NR_mmap
    syscall_unmap,      // 91 __NR_munmap
    0,                  // 92
    syscall_ftruncate,  // 93 __NR_ftruncate
    syscall_fchmod,     // 94 __NR_fchmod
    0,                  // 95
    0,                  // 96
    syscall_setpriority,// 97 __NR_setpriority
    0,                  // 98
    syscall_statfs,     // 99 __NR_statfs
    0,                  // 100
    syscall_ioperm,     // 101 __NR_ioperm
    syscall_socketcall, // 102 __NR_socketcall
    0,                  // 103
    syscall_setitimer,  // 104 __NR_setitimer 
    0,                  // 105
    0,                  // 106
    0,                  // 107
    0,                  // 108
    0,                  // 109
    syscall_iopl,       // 110 __NR_iopl
    0,                  // 111
    0,                  // 112
    0,                  // 113
    syscall_wait4,      // 114 __NR_wait4
    0,                  // 115
    syscall_sysinfo,    // 116 __NR_sysinfo
    syscall_ipc,        // 117 __NR_ipc
    syscall_fsync,      // 118 __NR_fsync
    syscall_sigreturn,  // 119 __NR_sigreturn
    syscall_clone,      // 120 __NR_clone
    0,                  // 121
    syscall_uname,      // 122 __NR_uname
    syscall_modify_ldt, // 123 __NR_modify_ldt
    0,                  // 124
    syscall_mprotect,   // 125 __NR_mprotect
    0,                  // 126
    0,                  // 127
    0,                  // 128
    0,                  // 129
    0,                  // 130
    0,                  // 131
    syscall_getpgid,    // 132 __NR_getpgid
    syscall_fchdir,     // 133 __NR_fchdir
    0,                  // 134
    0,                  // 135
    0,                  // 136
    0,                  // 137
    0,                  // 138
    0,                  // 139
    syscall_llseek,     // 140 __NR__llseek
    syscall_getdents,   // 141 __NR_getdents
    syscall_newselect,  // 142 __NR_newselect
    syscall_flock,      // 143 __NR_flock
    syscall_msync,      // 144 __NR_msync
    0,                  // 145
    syscall_writev,     // 146  __NR_writev
    0,                  // 147
    syscall_fdatasync,  // 148 __NR_fdatasync
    0,                  // 149
    syscall_mlock,      // 150 __NR_mlock
    0,                  // 151
    0,                  // 152
    0,                  // 153
    0,                  // 154
    syscall_sched_getparam, // 155 __NR_sched_getparam
    0,                  // 156
    syscall_sched_getscheduler, // 157 __NR_sched_getscheduler
    syscall_sched_yield,// 158 __NR_sched_yield
    syscall_sched_get_priority_max, // 159 __NR_sched_get_priority_max
    syscall_sched_get_priority_min, // 160 __NR_sched_get_priority_min
    0,                  // 161
    syscall_nanosleep,  // 162 __NR_nanosleep
    syscall_mremap,     // 163 __NR_mremap
    0,                  // 164
    0,                  // 165
    syscall_vm86,       // 166 __NR_vm86
    0,                  // 167
    syscall_poll,       // 168 __NR_poll
    0,                  // 169
    0,                  // 170
    0,                  // 171
    syscall_prctl,      // 172 __NR_prctl
    0,                  // 173
    syscall_rt_sigaction,// 174 __NR_rt_sigaction
    syscall_rt_sigprocmask, // 175 __NR_rt_sigprocmask
    0,                  // 176
    0,                  // 177
    0,                  // 178
    syscall_rt_sigsuspend, // 179 __NR_rt_sigsuspend
    syscall_pread64,    // 180 __NR_pread64
    syscall_pwrite64,   // 181 __NR_pwrite64
    0,                  // 182
    syscall_getcwd,     // 183 __NR_getcwd
    0,                  // 184
    0,                  // 185
    syscall_sigaltstack,// 186 __NR_sigaltstack
    0,                  // 187
    0,                  // 188
    0,                  // 189
    syscall_vfork,      // 190 __NR_vfork
    syscall_ugetrlimit, // 191 __NR_ugetrlimit
    syscall_mmap2,      // 192 __NR_mmap2
    0,                  // 193
    syscall_ftruncate64,// 194 __NR_ftruncate64
    syscall_stat64,     // 195 __NR_stat64
    syscall_lstat64,    // 196 __NR_lstat64
    syscall_fstat64,    // 197 __NR_fstat64
    syscall_lchown32,   // 198 __NR_lchown32
    syscall_getuid32,   // 199 __NR_getuid32
    syscall_getgid32,   // 200 __NR_getgid32
    syscall_geteuid32,  // 201 __NR_geteuid32
    syscall_getegid32,  // 202 __NR_getegid32
    0,                  // 203
    0,                  // 204
    syscall_getgroups32,// 205 __NR_getgroups32
    syscall_setgroups32,// 206 __NR_setgroups32
    syscall_fchown32,   // 207 __NR_fchown32
    syscall_setresuid32,// 208 __NR_setresuid32
    syscall_getresuid32,// 209 __NR_getresuid32
    syscall_setresgid32,// 210 __NR_setresgid32
    syscall_getresgid32,// 211 __NR_getresgid32
    syscall_chown32,    // 212 __NR_chown32
    syscall_setuid32,   // 213 __NR_setuid32
    syscall_setgid32,   // 214 __NR_setgid32
    0,                  // 215
    0,                  // 216
    0,                  // 217
    syscall_mincore,    // 218 __NR_mincore
    syscall_madvise,    // 219 __NR_madvise
    syscall_getdents64, // 220 __NR_getdents64
    syscall_fcntl64,    // 221 __NR_fcntl64
    0,                  // 222
    0,                  // 223
    syscall_gettid,     // 224 __NR_gettid
    0,                  // 225
    0,                  // 226
    0,                  // 227
    syscall_fsetxattr,  // 228 __NR_fsetxattr
    syscall_getxattr,   // 229
    syscall_lgetxattr,  // 230
    syscall_fgetxattr,  // 231 __NR_fgetxattr
    0,                  // 232
    0,                  // 233
    syscall_flistxattr, // 234 __NR_flistxattr
    0,                  // 235
    0,                  // 236
    0,                  // 237
    0,                  // 238 __NR_tkill
    0,                  // 239
    syscall_futex,      // 240 __NR_futex
    syscall_sched_setaffinity, // 241 __NR_sched_setaffinity
    syscall_sched_getaffinity, // 242 __NR_sched_getaffinity
    syscall_set_thread_area, // 243 __NR_set_thread_area
    0,                  // 244
    0,                  // 245
    0,                  // 246
    0,                  // 247
    0,                  // 248
    0,                  // 249
    0,                  // 250
    0,                  // 251
    syscall_exit_group, // 252 __NR_exit_group
    0,                  // 253
    syscall_epoll_create, // 254 __NR_epoll_create
    syscall_epoll_ctl,  // 255 __NR_epoll_ctl
    syscall_epoll_wait, // 256 __NR_epoll_wait
    0,                  // 257
    syscall_set_tid_address, // 258 __NR_set_tid_address
    0,                  // 259
    0,                  // 260
    0,                  // 261
    0,                  // 262
    0,                  // 263
    0,                  // 264
    syscall_clock_gettime, // 265 __NR_clock_gettime
    syscall_clock_getres, // 266 __NR_clock_getres
    syscall_clock_nanosleep, // 267 __NR_clock_nanosleep
    syscall_statfs64,   // 268 __NR_statfs64
    syscall_fstatfs64,  // 269 __NR_fstatfs64
    syscall_tgkill,     // 270 __NR_tgkill
    syscall_utimes,     // 271 __NR_utimes
    syscall_fadvise64,  // 272 __NR_fadvise64
    0,                  // 273
    0,                  // 274
    0,                  // 275
    0,                  // 276
    0,                  // 277
    0,                  // 278
    0,                  // 279
    0,                  // 280
    0,                  // 281
    0,                  // 282
    0,                  // 283
    0,                  // 284
    0,                  // 285
    0,                  // 286
    0,                  // 287
    0,                  // 288
    0,                  // 289
    0,                  // 290
    syscall_inotify_init,// 291 __NR_inotify_init
    0,                  // 292 __NR_inotify_add_watch
    0,                  // 293 __NR_inotify_rm_watch
    0,                  // 294
    syscall_openat,     // 295 __NR_openat
    syscall_mkdirat,    // 296 __NR_mkdirat
    0,                  // 297
    syscall_fchownat,   // 298 __NR_fchownat
    0,                  // 299
    syscall_fstatat64,  // 300 __NR_fstatat64
    syscall_unlinkat,   // 301 __NR_unlinkat
    syscall_renameat,   // 302
    0,                  // 303
    syscall_symlinkat,  // 304 __NR_symlinkat
    syscall_readlinkat, // 305 __NR_readlinkat
    syscall_fchmodat,   // 306 __NR_fchmodat
    syscall_faccessat,  // 307 __NR_faccessat
    0,                  // 308
    0,                  // 309
    0,                  // 310
    syscall_set_robust_list, // 311 __NR_set_robust_list
    0,                  // 312
    0,                  // 313
    syscall_sync_file_range, // 314 __NR_sync_file_range
    0,                  // 315
    0,                  // 316
    0,                  // 317
    0,                  // 318 __NR_getcpu
    0,                  // 319
    syscall_utimensat,  // 320 __NR_utimensat
    0,                  // 321
    0,                  // 322
    0,                  // 323 
    0,                  // 324 
    0,                  // 325
    0,                  // 326
    syscall_signalfd4,  // 327 __NR_signalfd4
    0,                  // 328
    syscall_epoll_create1, // 329 __NR_epoll_create1
    0,                  // 330
    syscall_pipe2,      // 331 __NR_pipe2
    0,                  // 332
    0,                  // 333
    0,                  // 334
    0,                  // 335
    0,                  // 336
    0,                  // 337
    0,                  // 338
    0,                  // 339
    syscall_prlimit64,  // 340 __NR_prlimit64
    0,                  // 341 __NR_name_to_handle_at
    0,                  // 342 __NR_open_by_handle_at
    0,                  // 343
    0,                  // 344
    syscall_sendmmsg,   // 345 __NR_sendmmsg 
    0,                  // 346
    0,                  // 347
    0,                  // 348
    0,                  // 349
    0,                  // 350
    0,                  // 351
    0,                  // 352
    syscall_renameat,   // 353 __NR_renameat2
    0,                  // 354
    syscall_getrandom,  // 355 __NR_getrandom
    syscall_memfd_create,// 356 __NR_memfd_create
    0,                  // 357 __NR_bpf
    0,                  // 358 __NR_execveat
    syscall_socket,     // 359 __NR_socket
    syscall_socketpair, // 360 __NR_socketpair
    syscall_bind,       // 361 __NR_bind
    syscall_connect,    // 362 __NR_connect
    syscall_listen,     // 363 __NR_listen
    syscall_accept4,    // 364 __NR_accept4
    syscall_getsockopt, // 365 __NR_getsockopt
    syscall_setsockopt, // 366 __NR_setsockopt
    syscall_getsockname,// 367 __NR_getsockname
    syscall_getpeername,// 368 __NR_getpeername
    syscall_sendto,     // 369 __NR_sendto
    syscall_sendmsg,    // 370 __NR_sendmsg
    syscall_recvfrom,   // 371 __NR_recvfrom
    syscall_recvmsg,    // 372 __NR_recvmsg
    syscall_shutdown,   // 373 __NR_shutdown
    0,                  // 374
    0,                  // 375
    0,                  // 376
    0,                  // 377
    0,                  // 378
    0,                  // 379
    0,                  // 380
    0,                  // 381
    0,                  // 382
    0,                  // 383 statx
    0,                  // 384
    0,                  // 385
    0,                  // 386
    0,                  // 387
    0,                  // 388
    0,                  // 389
    0,                  // 390
    0,                  // 391
    0,                  // 392
    0,                  // 393
    0,                  // 394
    0,                  // 395
    0,                  // 396
    0,                  // 397
    0,                  // 398
    0,                  // 399
    0,                  // 400
    0,                  // 401
    0,                  // 402
    syscall_clock_gettime64,     // 403
    0,                  // 404
    0,                  // 405
    syscall_clock_getres_time64,   // 406
    syscall_clock_nanosleep_time64, // 407
    0,                  // 408
    0,                  // 409
    0,                  // 410
    0,                  // 411
    syscall_utimensat_time64 // 412
};

#ifndef BOXEDWINE_MULTI_THREADED
extern S32 contextTime; // about the # instruction per 10 ms
#endif
void ksyscall(CPU* cpu, U32 eipCount) {
    U32 result;
#ifdef BOXEDWINE_MULTI_THREADED 
    U32 syscallNo = EAX;
#endif
    if (cpu->thread->terminating) {
        terminateCurrentThread(cpu->thread); // there is a race condition, just signal it again
		return;
    }
    if (cpu->thread->pendingSignals) {
        // I know this is a nested if statement, but it makes setting a break point easier
        if (cpu->thread->runSignals()) {
            cpu->nextBlock = NULL;
            return;
        }
    }
    if (EAX>412) {
        result = -K_ENOSYS;
        kdebug("no syscall for %d", EAX);
    } else if (!syscallFunc[EAX]) {
        result = -K_ENOSYS;
        kdebug("no syscall for %d", EAX);
    } else {
#ifndef BOXEDWINE_MULTI_THREADED
        U64 startTime = KSystem::getMicroCounter();
#endif
        result = syscallFunc[EAX](cpu, eipCount);
#ifndef BOXEDWINE_MULTI_THREADED
        U64 diff = KSystem::getMicroCounter()-startTime;
        sysCallTime+=diff;  
        cpu->blockInstructionCount+=(U32)(contextTime*diff/10000);
#endif
    }    
#ifdef BOXEDWINE_MULTI_THREADED
    if (cpu->thread->startSignal) {
        kpanic("syscall %d was not interrupted correctly by signal", syscallNo);
    }
#endif
    if (result==(U32)(-K_CONTINUE)) {

    } else if (result==(U32)(-K_WAIT)) {
        		
    } else {
        EAX = result;
        cpu->eip.u32+=eipCount;
    }
    cpu->nextBlock = NULL;
}

