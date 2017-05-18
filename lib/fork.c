// implement fork from user space

#include <inc/string.h>
#include <inc/lib.h>

// PTE_COW marks copy-on-write page table entries.
// It is one of the bits explicitly allocated to user processes (PTE_AVAIL).
#define PTE_COW		0x800

extern void _pgfault_upcall(void);

//
// Custom page fault handler - if faulting page is copy-on-write,
// map in our own private writable copy.
// TODO: Check the corresponding page (using UPAGES); if the page is COW but the
// reference count is 1, simply remap the same page RW (this would happen when,
// for example, the child has exited and freed all its pages).
//
static void
pgfault(struct UTrapframe *utf)
{
	void *addr = (void *) utf->utf_fault_va;
	void *pgaddr = ROUNDDOWN(addr, PGSIZE);
	uint32_t err = utf->utf_err;
	pte_t *pte;
	envid_t cur_envid;
	int r;

	// Check that the faulting access was (1) a write, and (2) to a
	// copy-on-write page.  If not, panic.
	// Hint:
	//   Use the read-only page table mappings at uvpt
	//   (see <inc/memlayout.h>).

	// LAB 4: Your code here.
	// We shift 12 left and 2 right to 0-out the last two bits (as opposed
	// to shifting 10)
	pte = (pte_t *) (UVPT | (((uintptr_t)addr >> 12)*4));
	if(!(err & FEC_WR) || !(*pte & PTE_COW))
	{
		panic("pgfault was not a write or page was not COW.\n"
		      "fault va: %p\n"
		      "err: %#x\n"
		      "*pte: %#x\n"
		      "eip: %p",
		      addr, err, *pte, utf->utf_eip);
	}


	// Allocate a new page, map it at a temporary location (PFTEMP),
	// copy the data from the old page to the new page, then move the new
	// page to the old page's address.
	// Hint:
	//   You should make three system calls.

	// LAB 4: Your code here.
	// We can't use curenv->env_id because children fault before they can
	// even write to the curenv variable; therefore we can't trust the value
	// of curenv in the page fault handler.
	cur_envid = sys_getenvid();

	r = sys_page_alloc(cur_envid, PFTEMP, PTE_W | PTE_U);
	if (r < 0)
		panic("Failed to alloc page in page fault handler: %e", r);

	memcpy(PFTEMP, pgaddr, PGSIZE);

	r = sys_page_map(cur_envid, PFTEMP, cur_envid, pgaddr,
			 PTE_W | PTE_U);
	if (r < 0)
		panic("Failed to map temporary page to addr in page fault "
		      "handler: %e", r);

}

//
// Map our virtual page pn (address pn*PGSIZE) into the target envid
// at the same virtual address.  If the page is writable or copy-on-write,
// the new mapping must be created copy-on-write, and then our mapping must be
// marked copy-on-write as well.  (Exercise: Why do we need to mark ours
// copy-on-write again if it was already copy-on-write at the beginning of
// this function?)
//
// Returns: 0 on success, < 0 on error.
// It is also OK to panic on error.
//
static int
duppage(envid_t envid, unsigned pn)
{
	int r;
	pte_t *pte;
	void *page_va;
	int perm;

	// LAB 4: Your code here.
	pte = (pte_t *) (UVPT | (pn * 4));
	page_va = (void *) (pn * PGSIZE);


	if (page_va == (void *)(UXSTACKTOP - PGSIZE))
	{
		// Allocate a new page for exception stack
		return sys_page_alloc(envid, (void *)(UXSTACKTOP - PGSIZE),
				      PTE_W | PTE_U);
	}

	// TODO: Why do we have to map child before parent? I switched the
	// order and a child (3001) generated a pgfault in forktree.c when
	// returning from fork() because the return address on the stack was 0;
	// executing ret instruction generated the page fault.
	if ((*pte & PTE_W) || (*pte & PTE_COW))
	{
		// Copy all syscall-allowed flags over except for PTE_W.
		// Ensure that PTE_COW is in there
		perm = *pte & PTE_SYSCALL;
		perm &= ~PTE_W;
		perm |= PTE_COW;

		r = sys_page_map(0, page_va, envid, page_va, perm);
		if (r < 0)
			return r;

		// Remap parent copy-on-write
		r = sys_page_map(0, page_va, 0, page_va, perm);
		if (r < 0)
			return r;
	}
	else
	{
		perm = *pte & PTE_SYSCALL;
		r = sys_page_map(0, page_va, envid, page_va, perm);
		if (r < 0)
			return r;
	}

	return 0;
}

//
// User-level fork with copy-on-write.
// Set up our page fault handler appropriately.
// Create a child.
// Copy our address space and page fault handler setup to the child.
// Then mark the child as runnable and return.
//
// Returns: child's envid to the parent, 0 to the child, < 0 on error.
// It is also OK to panic on error.
//
// Hint:
//   Use uvpd, uvpt, and duppage.
//   Remember to fix "thisenv" in the child process.
//   Neither user exception stack should ever be marked copy-on-write,
//   so you must allocate a new page for the child's user exception stack.
//
envid_t
fork(void)
{
	envid_t child;
	uintptr_t pde;
	uintptr_t pte;
	int i;
	int r;

	set_pgfault_handler(pgfault);

	child = sys_exofork();
	if (child < 0)
		panic("fork - sys_exofork: %e", child);
	if (child == 0)
	{
		thisenv = (struct Env *)envs + ENVX(sys_getenvid());
		return 0;
	}



	// Scan address space *until UXSTACK* using UVPT, calling duppage only on
	// present pages.
	for (i = 0; i <= PDX(UXSTACKTOP - PGSIZE); i++)
	{
		if (uvpd[i] & PTE_P)
		{
			int j;
			// global uvpt array only enabled us to access the first
			// page table; this is a more general solution.
			pte_t *uvpt = (pte_t *) (UVPT | (i << 12));

			// Scan the 2^10 page table entries
			for (j = 0; j < (1 << 10); j++)
			{
				if (uvpt[j] & PTE_P)
				{
					r = duppage(child, (i << 10) + j);
					if (r < 0)
					{
						sys_env_destroy(child);
						panic("duppage: %e", r);
					}
				}

			}
		}
	}


	r = sys_env_set_pgfault_upcall(child, _pgfault_upcall);
	if (r < 0)
		panic("Failed to set pgfault upcall to child");

	sys_env_set_status(child, ENV_RUNNABLE);
	return child;
}

// Challenge!
int
sfork(void)
{
	panic("sfork not implemented");
	return -E_INVAL;
}
