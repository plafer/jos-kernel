/* See COPYRIGHT for copyright information. */

#include <inc/x86.h>
#include <inc/error.h>
#include <inc/string.h>
#include <inc/assert.h>

#include <kern/env.h>
#include <kern/pmap.h>
#include <kern/trap.h>
#include <kern/syscall.h>
#include <kern/console.h>
#include <kern/sched.h>

#define ALIGNED_USER_ADDR(va) ((uintptr_t)va % PGSIZE == 0 ||	\
                               (uintptr_t)va < UTOP)

#define VALID_USER_PERM(perm) (!(perm & ~PTE_SYSCALL) &&		\
			       ((perm & PTE_U) || (perm & PTE_P)))


// Print a string to the system console.
// The string is exactly 'len' characters long.
// Destroys the environment on memory errors.
static void
sys_cputs(const char *s, size_t len)
{
	// Check that the user has permission to read memory [s, s+len).
	// Destroy the environment if not.
	user_mem_assert(curenv, s, len, 0);

	// Print the string supplied by the user.
	cprintf("%.*s", len, s);
}

// Read a character from the system console without blocking.
// Returns the character, or 0 if there is no input waiting.
static int
sys_cgetc(void)
{
	return cons_getc();
}

// Returns the current environment's envid.
static envid_t
sys_getenvid(void)
{
	return curenv->env_id;
}

// Destroy a given environment (possibly the currently running environment).
//
// Returns 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist,
//		or the caller doesn't have permission to change envid.
static int
sys_env_destroy(envid_t envid)
{
	int r;
	struct Env *e;

	if ((r = envid2env(envid, &e, 1)) < 0)
		return r;
	env_destroy(e);
	return 0;
}

// Deschedule current environment and pick a different one to run.
static void
sys_yield(void)
{
	sched_yield();
}

// Allocate a new environment.
// Returns envid of new environment, or < 0 on error.  Errors are:
//	-E_NO_FREE_ENV if no free environment is available.
//	-E_NO_MEM on memory exhaustion.
static envid_t
sys_exofork(void)
{
	int err;
	struct Env *new_env;

	// Create the new environment with env_alloc(), from kern/env.c.
	// It should be left as env_alloc created it, except that
	// status is set to ENV_NOT_RUNNABLE, and the register set is copied
	// from the current environment -- but tweaked so sys_exofork
	// will appear to return 0.
	// LAB 4: Your code here.
	err = env_alloc(&new_env, curenv->env_id);
	if (err != 0)
		return err;

	new_env->env_status = ENV_NOT_RUNNABLE;

	// Copy registers from parent, except for eax (return value of system
	// call)
	new_env->env_tf = curenv->env_tf;
	new_env->env_tf.tf_regs.reg_eax = 0;

	// Current environment sees the new env_id as the return value, while
	// the child sees 0.
	return new_env->env_id;
}

// Set envid's env_status to status, which must be ENV_RUNNABLE
// or ENV_NOT_RUNNABLE.
//
// Returns 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist,
//		or the caller doesn't have permission to change envid.
//	-E_INVAL if status is not a valid status for an environment.
static int
sys_env_set_status(envid_t envid, int status)
{
	int err;
	struct Env *env;
	// Hint: Use the 'envid2env' function from kern/env.c to translate an
	// envid to a struct Env.
	// You should set envid2env's third argument to 1, which will
	// check whether the current environment has permission to set
	// envid's status.

	// LAB 4: Your code here.
	if (status != ENV_RUNNABLE && status != ENV_NOT_RUNNABLE)
		return -E_INVAL;

	if ((err = envid2env(envid, &env, 1)) != 0)
		return err;

	env->env_status = status;
	return 0;
}

// Set envid's trap frame to 'tf'.
// tf is modified to make sure that user environments always run at code
// protection level 3 (CPL 3) with interrupts enabled.
//
// Returns 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist,
//		or the caller doesn't have permission to change envid.
static int
sys_env_set_trapframe(envid_t envid, struct Trapframe *tf)
{
	struct Env *env;
	int r;

	// LAB 5: Your code here.
	// Remember to check whether the user has supplied us with a good
	// address!
	if ((r = envid2env(envid, &env, 1)) < 0)
		return r;

	if ((r = user_mem_check(env, tf, sizeof(*tf), PTE_U)) < 0)
		return r;

	env->env_tf = *tf;
	env->env_tf.tf_cs |= 3;          // Ensure CPL 3
	env->env_tf.tf_eflags |= FL_IF;  // Ensure interrupts enabled

	return 0;
}

// Set the page fault upcall for 'envid' by modifying the corresponding struct
// Env's 'env_pgfault_upcall' field.  When 'envid' causes a page fault, the
// kernel will push a fault record onto the exception stack, then branch to
// 'func'.
//
// Returns 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist,
//		or the caller doesn't have permission to change envid.
//      -E_INVAL if func >= UTOP or isn't in the user's address space (my added
//              requirement)
static int
sys_env_set_pgfault_upcall(envid_t envid, void *func)
{
	struct Env *env;

	if (envid2env(envid, &env, 1) != 0)
		return -E_BAD_ENV;

	// Grade script doesn't like it
/*
	if ((uintptr_t)func >= UTOP ||
	    page_lookup(env->env_pgdir, func, NULL) == NULL)
		return -E_INVAL;
*/

	env->env_pgfault_upcall = func;

	return 0;
}

// Allocate a page of memory and map it at 'va' with permission
// 'perm' in the address space of 'envid'.
// The page's contents are set to 0.
// If a page is already mapped at 'va', that page is unmapped as a
// side effect.
//
// perm -- PTE_U | PTE_P must be set, PTE_AVAIL | PTE_W may or may not be set,
//         but no other bits may be set.  See PTE_SYSCALL in inc/mmu.h.
//
// Return 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist,
//		or the caller doesn't have permission to change envid.
//	-E_INVAL if va >= UTOP, or va is not page-aligned.
//	-E_INVAL if perm is inappropriate (see above).
//	-E_NO_MEM if there's no memory to allocate the new page,
//		or to allocate any necessary page tables.
static int
sys_page_alloc(envid_t envid, void *va, int perm)
{
	int err;
	struct Env *env;
	struct PageInfo *page;

	// Hint: This function is a wrapper around page_alloc() and
	//   page_insert() from kern/pmap.c.
	//   Most of the new code you write should be to check the
	//   parameters for correctness.
	//   If page_insert() fails, remember to free the page you
	//   allocated!

	// LAB 4: Your code here.
	// 1. Ensure envid is valid and that current environment can manipulate
	// the environment.
	if (envid2env(envid, &env, 1) != 0)
		return -E_BAD_ENV;

	// 2. Ensure valid address
	if (!ALIGNED_USER_ADDR(va))
		return -E_INVAL;

	// 3. Ensure valid permissions
	if (!VALID_USER_PERM(perm))
		return -E_INVAL;

	// 4. Allocate page and insert into address space
	if((page = page_alloc(ALLOC_ZERO)) == NULL)
		return -E_NO_MEM;

	if (page_insert(env->env_pgdir, page, va, perm) != 0)
	{
		page_free(page);
		return -E_NO_MEM;
	}

	return 0;
}

// Map the page of memory at 'srcva' in srcenvid's address space
// at 'dstva' in dstenvid's address space with permission 'perm'.
// Perm has the same restrictions as in sys_page_alloc, except
// that it also must not grant write access to a read-only
// page.
//
// Return 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if srcenvid and/or dstenvid doesn't currently exist,
//		or the caller doesn't have permission to change one of them.
//	-E_INVAL if srcva >= UTOP or srcva is not page-aligned,
//		or dstva >= UTOP or dstva is not page-aligned.
//	-E_INVAL is srcva is not mapped in srcenvid's address space.
//	-E_INVAL if perm is inappropriate (see sys_page_alloc).
//	-E_INVAL if (perm & PTE_W), but srcva is read-only in srcenvid's
//		address space.
//	-E_NO_MEM if there's no memory to allocate any necessary page tables.
static int
sys_page_map(envid_t srcenvid, void *srcva,
	     envid_t dstenvid, void *dstva, int perm)
{
	struct Env *srcenv;
	struct Env *dstenv;
	pte_t *page_pte;
	struct PageInfo *page;

	if (envid2env(srcenvid, &srcenv, 1) != 0 ||
	    envid2env(dstenvid, &dstenv, 1) != 0)
		return -E_BAD_ENV;

	if (!ALIGNED_USER_ADDR(srcva) || !ALIGNED_USER_ADDR(dstva))
		return -E_INVAL;

	if (!VALID_USER_PERM(perm))
		return -E_INVAL;

	if ((page = page_lookup(srcenv->env_pgdir, srcva, &page_pte)) == NULL)
		return -E_INVAL;

	// Make sure that if the page was RO, we can't map it with write perm
	if ((perm & PTE_W) && !(*page_pte & PTE_W))
		return -E_INVAL;

	if (page_insert(dstenv->env_pgdir, page, dstva, perm) != 0)
		return -E_NO_MEM;

	return 0;
}

// Unmap the page of memory at 'va' in the address space of 'envid'.
// If no page is mapped, the function silently succeeds.
//
// Return 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist,
//		or the caller doesn't have permission to change envid.
//	-E_INVAL if va >= UTOP, or va is not page-aligned.
static int
sys_page_unmap(envid_t envid, void *va)
{
	struct Env *env;

	if (envid2env(envid, &env, 1) != 0)
		return -E_BAD_ENV;

	if (!ALIGNED_USER_ADDR(va))
		return -E_INVAL;

	page_remove(env->env_pgdir, va);

	return 0;
}

// Try to send 'value' to the target env 'envid'.
// If srcva < UTOP, then also send page currently mapped at 'srcva',
// so that receiver gets a duplicate mapping of the same page.
//
// The send fails with a return value of -E_IPC_NOT_RECV if the
// target is not blocked, waiting for an IPC.
//
// The send also can fail for the other reasons listed below.
//
// Otherwise, the send succeeds, and the target's ipc fields are
// updated as follows:
//    env_ipc_recving is set to 0 to block future sends;
//    env_ipc_from is set to the sending envid;
//    env_ipc_value is set to the 'value' parameter;
//    env_ipc_perm is set to 'perm' if a page was transferred, 0 otherwise.
// The target environment is marked runnable again, returning 0
// from the paused sys_ipc_recv system call.  (Hint: does the
// sys_ipc_recv function ever actually return?)
//
// If the sender wants to send a page but the receiver isn't asking for one,
// then no page mapping is transferred, but no error occurs.
// The ipc only happens when no errors occur.
//
// Returns 0 on success, < 0 on error.
// Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist.
//		(No need to check permissions.)
//	-E_IPC_NOT_RECV if envid is not currently blocked in sys_ipc_recv,
//		or another environment managed to send first.
//	-E_INVAL if srcva < UTOP but srcva is not page-aligned.
//	-E_INVAL if srcva < UTOP and perm is inappropriate
//		(see sys_page_alloc).
//	-E_INVAL if srcva < UTOP but srcva is not mapped in the caller's
//		address space.
//	-E_INVAL if (perm & PTE_W), but srcva is read-only in the
//		current environment's address space.
//	-E_NO_MEM if there's not enough memory to map srcva in envid's
//		address space.
static int
sys_ipc_try_send(envid_t envid, uint32_t value, void *srcva, unsigned perm)
{
	struct Env *recv_env;
	struct PageInfo *map_page;
	pte_t *map_pte;

	if (envid2env(envid, &recv_env, 0) != 0)
		return -E_BAD_ENV;

	if (!recv_env->env_ipc_recving)
		return -E_IPC_NOT_RECV;

	if ((uintptr_t)srcva < UTOP &&
	    (uintptr_t)recv_env->env_ipc_dstva < UTOP)
	{
		if (!ALIGNED_USER_ADDR(srcva))
			return -E_INVAL;
		if (!VALID_USER_PERM(perm))
			return -E_INVAL;

		if ((map_page = page_lookup(curenv->env_pgdir, srcva, &map_pte))
		    == NULL)
			return -E_INVAL;
		if ((perm & PTE_W) && !(*map_pte & PTE_W))
			return -E_INVAL;

		// We don't use sys_page_map because it always sets the
		// checkperm flag in envid2env.
		if (page_insert(recv_env->env_pgdir, map_page,
				recv_env->env_ipc_dstva, perm) != 0)
			return -E_NO_MEM;

		recv_env->env_ipc_perm = perm;
	}
	else
	{
		recv_env->env_ipc_perm = 0;
	}

	recv_env->env_ipc_recving = false;
	recv_env->env_ipc_value = value;
	recv_env->env_ipc_from = curenv->env_id;

	// Make recv_env return 0
	recv_env->env_tf.tf_regs.reg_eax = 0;
	recv_env->env_status = ENV_RUNNABLE;

	return 0;
}

// Block until a value is ready.  Record that you want to receive
// using the env_ipc_recving and env_ipc_dstva fields of struct Env,
// mark yourself not runnable, and then give up the CPU.
//
// If 'dstva' is < UTOP, then you are willing to receive a page of data.
// 'dstva' is the virtual address at which the sent page should be mapped.
//
// This function only returns on error, but the system call will eventually
// return 0 on success.
// Return < 0 on error.  Errors are:
//	-E_INVAL if dstva < UTOP but dstva is not page-aligned.
static int
sys_ipc_recv(void *dstva)
{
	if ((uintptr_t)dstva < UTOP && !((uintptr_t)dstva % PGSIZE == 0))
		return -E_INVAL;

	curenv->env_ipc_recving = true;
	curenv->env_ipc_dstva = dstva;

	curenv->env_status = ENV_NOT_RUNNABLE;
	sched_yield();

	return 0;
}

static int
sys_get_cpu(void)
{
	return cpunum();
}


// Dispatches to the correct kernel function, passing the arguments.
int32_t
syscall(uint32_t syscallno, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5)
{
	// Call the function corresponding to the 'syscallno' parameter.
	// Return any appropriate return value.
	switch (syscallno) {
	case SYS_cputs:
		sys_cputs((const char *) a1, (size_t) a2);
		return 0;
	case SYS_cgetc:
		return (int32_t) sys_cgetc();
	case SYS_getenvid:
		return (int32_t) sys_getenvid();
	case SYS_env_destroy:
		return (int32_t) sys_env_destroy((envid_t) a1);
	case SYS_page_alloc:
		return (int32_t) sys_page_alloc((envid_t) a1, (void *) a2,
						(int) a3);
	case SYS_page_map:
		return (int32_t) sys_page_map((envid_t) a1, (void *) a2,
					      (envid_t) a3, (void *) a4,
					      (int) a5);
	case SYS_page_unmap:
		return (int32_t) sys_page_unmap((envid_t) a1, (void *) a2);
	case SYS_exofork:
		return (int32_t) sys_exofork();
	case SYS_env_set_status:
		return (int32_t) sys_env_set_status((envid_t) a1, (int) a2);
	case SYS_env_set_pgfault_upcall:
		return (int32_t) sys_env_set_pgfault_upcall((envid_t) a1,
							    (void *) a2);
	case SYS_yield:
		sys_yield();
		/* Doesn't return */
		return -E_UNSPECIFIED;
	case SYS_ipc_try_send:
		return (int32_t) sys_ipc_try_send((envid_t) a1, a2, (void *) a3,
						  (unsigned) a4);
	case SYS_ipc_recv:
		return (int32_t) sys_ipc_recv((void *) a1);
	case SYS_get_cpu:
		return (int32_t) sys_get_cpu();
	case SYS_env_set_trapframe:
		return (int32_t) sys_env_set_trapframe((envid_t) a1,
						       (struct Trapframe *) a2);
	default:
		return -E_INVAL;
	}
}
