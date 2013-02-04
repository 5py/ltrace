/*
 * This file is part of ltrace.
 * Copyright (C) 2010,2011,2012 Petr Machata, Red Hat Inc.
 * Copyright (C) 2004,2008,2009 Juan Cespedes
 * Copyright (C) 2006 Ian Wienand
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 */

#include "config.h"

#include <sys/wait.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "backend.h"
#include "debug.h"
#include "proc.h"
#include "ptrace.h"
#include "type.h"
#include "common.h"

#ifdef __x86_64__
static const int x86_64 = 1;
#else
static const int x86_64 = 0;
#endif

static struct threadinfo *
switch_thread(struct process *proc, lwpid_t tid)
{
	struct threadinfo *td;

	if (curthread && curthread->proc == proc) {
		/* Do not do anything if thread the same. */
		if (curthread->tid == tid)
			return curthread;
		/* Save current context. */
		debug(0100, "save context: tid=%u, callstack_depth=%zu",
		    curthread->tid, proc->callstack_depth);
		curthread->callstack_depth = proc->callstack_depth;
		memcpy(curthread->callstack, proc->callstack,
		    sizeof(struct callstack_element) * proc->callstack_depth);
		curthread->event_handler = proc->event_handler;
		curthread->saved = 1;
	}
	/* Looking for already existed thread. */
	SLIST_FOREACH(td, &proc->os.threads, next) {
		if (td->tid == tid) {
			if (td->saved == 0)
				return td;
			/* Load saved context. */
			debug(0100, "load context: tid=%u, callstack_depth=%zu",
			    td->tid, td->callstack_depth);
			proc->callstack_depth = td->callstack_depth;
			memcpy(proc->callstack, td->callstack,
			    sizeof(struct callstack_element) * td->callstack_depth);
			proc->event_handler = td->event_handler;
			return td;
		}
	}
	/* New thread. */
	debug(0100, "new thread: tid=%u", tid);
	td = malloc(sizeof(*td));
	if (td == NULL) {
		perror("malloc");
		return curthread;
	}
	td->tid = tid;
	td->onstep = 0;
	td->proc = proc;
	td->saved = 0;
	td->callstack_depth = 0;
	td->callstack = malloc(sizeof(struct callstack_element) * MAX_CALLDEPTH);
	td->event_handler = NULL;
	SLIST_INSERT_HEAD(&proc->os.threads, td, next);
	return td;
}

void
get_arch_dep(struct process *proc)
{
	if (ptrace(PT_LWPINFO, proc->pid, (caddr_t)&proc->os.lwpinfo,
	    sizeof(proc->os.lwpinfo)) == -1) {
		if (errno != ESRCH)
			perror("PT_LWPINFO");
		return;
	}
	curthread = switch_thread(proc, proc->os.lwpinfo.pl_lwpid);
	curthread->valid_regs = (ptrace(PT_GETREGS, curthread->tid,
	    (caddr_t)&curthread->regs, 0) == 0);

	/* Unfortunately there are still remnants of mask_32bit uses
	 * around.  */

	if (proc->e_machine == EM_X86_64) {
		proc->mask_32bit = 0;
		proc->personality = 0;
	} else if (x86_64) { /* x86_64/i386 */
		proc->mask_32bit = 1;
		proc->personality = 0;
	} else {
		proc->mask_32bit = 0;
		proc->personality = 0;
	}
}

/* Returns 1 if syscall, 2 if sysret, 0 otherwise.
 */
int
syscall_p(struct process *proc, int status, int *sysnum)
{
	if (WIFSTOPPED(status) && WSTOPSIG(status) == SIGTRAP) {
		struct callstack_element *elem = NULL;
		if (proc->callstack_depth > 0)
			elem = proc->callstack + proc->callstack_depth - 1;

		if ((proc->os.lwpinfo.pl_flags & PL_FLAG_SCE) != 0) {
#ifdef __x86_64__
			*sysnum = curthread->regs.r_rax;
#else
			*sysnum = curthread->regs.r_eax;
#endif
		} else if ((proc->os.lwpinfo.pl_flags & PL_FLAG_SCX) != 0) {
			if (elem != NULL && elem->is_syscall)
				*sysnum = elem->c_un.syscall;
			else
				*sysnum = -1;	/* in STATE_IGNORED */
		} else
			return 0;
		debug(DEBUG_FUNCTION, "sysnum=%d %p %d", *sysnum,
		    get_instruction_pointer(proc), errno);
		if ((proc->os.lwpinfo.pl_flags & PL_FLAG_SCE) != 0)
			return 1;
		else if ((proc->os.lwpinfo.pl_flags & PL_FLAG_SCX) != 0)
			return 2;
	}
	return 0;
}

size_t
arch_type_sizeof(struct process *proc, struct arg_type_info *info)
{
	if (proc == NULL)
		return (size_t)-2;

	switch (info->type) {
	case ARGTYPE_VOID:
		return 0;

	case ARGTYPE_CHAR:
		return 1;

	case ARGTYPE_SHORT:
	case ARGTYPE_USHORT:
		return 2;

	case ARGTYPE_INT:
	case ARGTYPE_UINT:
		return 4;

	case ARGTYPE_LONG:
	case ARGTYPE_ULONG:
	case ARGTYPE_POINTER:
		return proc->e_machine == EM_X86_64 ? 8 : 4;

	case ARGTYPE_FLOAT:
		return 4;
	case ARGTYPE_DOUBLE:
		return 8;

	case ARGTYPE_ARRAY:
	case ARGTYPE_STRUCT:
		/* Use default value.  */
		return (size_t)-2;

	default:
		assert(info->type != info->type);
		abort();
	}
}

size_t
arch_type_alignof(struct process *proc, struct arg_type_info *info)
{
	if (proc == NULL)
		return (size_t)-2;

	switch (info->type) {
	default:
		assert(info->type != info->type);
		abort();
		break;

	case ARGTYPE_CHAR:
		return 1;

	case ARGTYPE_SHORT:
	case ARGTYPE_USHORT:
		return 2;

	case ARGTYPE_INT:
	case ARGTYPE_UINT:
		return 4;

	case ARGTYPE_LONG:
	case ARGTYPE_ULONG:
	case ARGTYPE_POINTER:
		return proc->e_machine == EM_X86_64 ? 8 : 4;

	case ARGTYPE_FLOAT:
		return 4;
	case ARGTYPE_DOUBLE:
		return proc->e_machine == EM_X86_64 ? 8 : 4;

	case ARGTYPE_ARRAY:
	case ARGTYPE_STRUCT:
		/* Use default value.  */
		return (size_t)-2;
	}
}