// Simple command-line kernel monitor useful for
// controlling the kernel and exploring the system interactively.

#include <inc/stdarg.h>
#include <inc/stdio.h>
#include <inc/string.h>
#include <inc/memlayout.h>
#include <inc/assert.h>
#include <inc/x86.h>

#include <kern/console.h>
#include <kern/monitor.h>
#include <kern/kdebug.h>
#include <kern/trap.h>
#include <kern/pmap.h>


#define CMDBUF_SIZE	80	// enough for one VGA text line


struct Command {
	const char *name;
	const char *desc;
	// return -1 to force monitor to exit
	int (*func)(int argc, char** argv, struct Trapframe* tf);
};

static struct Command commands[] = {
	{ "help", "Display this list of commands", mon_help },
	{ "kerninfo", "Display information about the kernel", mon_kerninfo },
	{ "backtrace", "Display backtrace up to first C function called", mon_backtrace},
	{ "physlayout", "Display ASCII diagram of physical layout", mon_physlayout},
	{ "showmappings", "Show the physical page mappings of a range of virtual"
	  "addresses", mon_showmappings },
	{ "chgmappings", "Set, clear, or change the permissions of any mapping in "
	  "the current address space", mon_chgmappings },
	{ "memdump", "Dump the memory contents of a physical or virtual address range",
	  mon_memdump }
};

static int
show_usage(char *fmt, ...)
{
	va_list varargs;

	cprintf("Usage: ");

	va_start(varargs, fmt);
	vcprintf(fmt, varargs);
	va_end(varargs);

	cprintf("\n");

	return -1;
}

/***** Implementations of basic kernel monitor commands *****/

int
mon_help(int argc, char **argv, struct Trapframe *tf)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(commands); i++)
		cprintf("%s - %s\n", commands[i].name, commands[i].desc);
	return 0;
}

int
mon_kerninfo(int argc, char **argv, struct Trapframe *tf)
{
	extern char _start[], entry[], etext[], edata[], end[];

	cprintf("Special kernel symbols:\n");
	cprintf("  _start                  %08x (phys)\n", _start);
	cprintf("  entry  %08x (virt)  %08x (phys)\n", entry, entry - KERNBASE);
	cprintf("  etext  %08x (virt)  %08x (phys)\n", etext, etext - KERNBASE);
	cprintf("  edata  %08x (virt)  %08x (phys)\n", edata, edata - KERNBASE);
	cprintf("  end    %08x (virt)  %08x (phys)\n", end, end - KERNBASE);
	cprintf("Kernel executable memory footprint: %dKB\n\n",
		ROUNDUP(end - entry, 1024) / 1024);

	int free_pages, used_pages;
	free_pages = num_free_pages();
	used_pages = npages - free_pages;
	cprintf("Physical pages:\n");
	cprintf("Total: %d (%dMB)\n", npages, (npages * PGSIZE) / MB);
	cprintf("Used:  %d (%dKB)\n", used_pages,
		(used_pages * PGSIZE) / KB);
	cprintf("Free:  %d (%dMB)\n", free_pages,
		(free_pages * PGSIZE) / MB);
	cprintf("Page_alloc'ed: %d\n", num_page_alloced);
	return 0;
}


static int
page_is_free(struct PageInfo *pp)
{
	// You're free if
	// 1. You're in the free list, or
	// 2. You're the last page (link is NULL) and no one references you
	return (pp->pp_link != NULL) ||
		(pp == &pages[npages - 1] && pp->pp_ref == 0);
}
//  +---------------+ 0x8000000
//  | Free:   32422 |
//  +---------------+ 0x15A000
//  | Used:      65 |
//  +---------------+ 0x119000
//  ...
//  +---------------+ 0x0
int
mon_physlayout(int argc, char **argv, struct Trapframe *tf)
{
	char *seperator = "+---------------+"; // length: 17

	char *state_name;
	int state_free;
	int same_state_count;
	struct PageInfo *pp;

	// 1. Print seperator and corresponding address
	// 2. while same state (free vs used), increment count
	// 3. when change state, print State: count
	for (pp = &pages[npages - 1]; pp >= pages; pp--)
	{
		// We want to print the address above the current one,
		// which is the beginning of a new state and thus BELOW the seperator
		cprintf("%s 0x%x\n", seperator, page2pa(pp+1));

		// We add the pp->link != NULL condition for the last element
		// on the free list: its pp->link will be NULL, but it is free
		state_free = page_is_free(pp);
		state_name = state_free ? "Free" : "Used";

		for (same_state_count = 0;
		     pp >= pages && state_free == page_is_free(pp);
		     pp--, same_state_count++);

		// %d width = length - 7 ("| Free:") - 3 (" |")
		cprintf("| %s: %7d |\n", state_name, same_state_count);

		// Adjust before it gets re-decremented
		pp++;
	}

	// Print last seperator and address (hopefully 0x0)
	cprintf("%s 0x%x\n", seperator, page2pa(pp+1));

	return 0;
}

// Format:
// VA            PA           K / U
// 0xF0000000 ->        0x0   RW/--
// 0xEFFFF000 (NOT MAPPED)
int
mon_showmappings(int argc, char **argv, struct Trapframe *tf)
{
	long lowaddr;
	long highaddr;
	pde_t *pgdir;

	if (argc <= 1 || argc > 3)
	{
		return show_usage("%s lowaddr [highaddr]");
	}

	lowaddr = ROUNDDOWN(strtol(argv[1], NULL, 0), PGSIZE);
	if (argc == 3)
	{
		highaddr = ROUNDUP(strtol(argv[2], NULL, 0), PGSIZE);
		if (highaddr > (uint32_t)~0)
			highaddr = 0xFFFFFFFF;
	}
	else
		highaddr = lowaddr;

	if (lowaddr > highaddr)
	{
		cprintf("Low address (%s) has to be lower than high addr (%s)\n",
			argv[1], argv[2]);
		return -1;
	}

	cprintf("VA%12sPA%10s K/U\n", "", "");
	for(pgdir = KADDR(rcr3()) ; lowaddr <= highaddr; highaddr -= PGSIZE)
	{
		pte_t *pte;

		cprintf("%0#10x ", highaddr);

		pte = pgdir_walk(pgdir, (void *)highaddr, 0);
		if (pte == NULL || !(*pte & PTE_P))
		{
			cprintf("   (NOT MAPPED)\n");
		}
		else
		{
			cprintf("-> %0#10x  ", PTE_ADDR(*pte));
			// Kernel can always read
			cprintf("R%c/%c%c\n",
				(*pte & PTE_W) ? 'W' : '-',
				(*pte & PTE_U) ? 'R' : '-',
				((*pte & PTE_U) && (*pte & PTE_W)) ? 'W' : '-');
		}
	}

	return 0;
}

// chgmapping 0xF0000000 [0xF000F000] rw/r-
int
mon_chgmappings(int argc, char **argv, struct Trapframe *tf)
{
	long lowva;
	long highva;
	long curva;
	pde_t *pgdir;
	pte_t *pte;
	char *endnum;
	uint32_t perm;

	if (argc < 3 || argc > 4)
	{
		return show_usage("%s lowva [highva] r[w|-]/[r|-][w|-]", argv[0]);
	}

	lowva = ROUNDDOWN(strtol(argv[1], NULL, 0), PGSIZE);
	highva = strtol(argv[2], &endnum, 0);

	if (endnum == argv[2] || highva < lowva)
	{
		// We're not dealing with a range of addresses
		highva = lowva;
	}
	else
	{
		highva = ROUNDUP(highva, PGSIZE);
	}

	// r-/-- ()
	// rw/-- (PTE_W)
	// r-/r- (PTE_U)
	// rw/rw (PTE_U PTE_W)
	if (strcmp("r-/--", argv[argc-1]) == 0)
		perm = 0;
	else if (strcmp("rw/--", argv[argc-1]) == 0)
		perm = PTE_W;
	else if (strcmp("r-/r-", argv[argc-1]) == 0)
		perm = PTE_U;
	else if (strcmp("rw/rw", argv[argc-1]) == 0)
		perm = PTE_U | PTE_W;
	else
	{
		cprintf("Possible permission schemes:\n"
			"\tr-/--\n"
			"\trw/--\n"
			"\tr-/r-\n"
			"\trw/rw\n");
		return -1;
	}


	pgdir = KADDR(rcr3());
	for (curva = lowva; curva <= highva; curva += PGSIZE)
	{
		pte = pgdir_walk(pgdir, (void *)curva, 0);
		if (pte == NULL)
		{
			cprintf("No mapping for %p\n", (void *)curva);
			continue;
		}
		*pte &= (~PTE_W & ~PTE_U);
		*pte |= perm;
	}

	return 0;
}

// memdump [-p] 0xf0000000 [0xf000f000]
int mon_memdump(int argc, char **argv, struct Trapframe *tf)
{
	char usage[] = "%s [-p] lowaddr [highaddr]";
	bool is_virtual_range;
	int argi;               // next argument to parse
	unsigned long lowaddr;
	unsigned long highaddr;
	pde_t *pgdir;

	if (argc < 2 || argc > 4)
		return show_usage(usage, argv[0]);

	argi = 1;
	if (strcmp("-p", argv[1]) == 0)
	{
		is_virtual_range = false;
		argi++;
	}
	else
	{
		is_virtual_range = true;
		pgdir = KADDR(rcr3());
	}

	if (argi < argc)
	{
		lowaddr = (unsigned long) strtol(argv[argi], NULL, 0);
		argi++;
	}
	else
		return show_usage(usage, argv[0]);

	if (argi < argc)
	{
		highaddr = (unsigned long) strtol(argv[argi], NULL, 0);
		argi++;
	}
	else
		highaddr = lowaddr;

	if (highaddr < lowaddr)
	{
		cprintf("High address has to be higher than low address.\n");
		return -1;
	}
	if ((is_virtual_range && highaddr > ~(uint32_t)0) ||
	    (!is_virtual_range && highaddr > npages * PGSIZE))
	{
		if (is_virtual_range)
			cprintf("Can't go higher than 0xffffffff\n");
		else
			cprintf("Can't go higher than %#x\n", npages * PGSIZE);
		return -1;
	}

	// 0xf0000000: 0x00000000 0x00000000 0x00000000 0x00000000
	// 0xf0000010: 0x00000000 0x00000000 0x00000000 0x00000000
	// 0xf0000020: (unmapped) (unmapped) (unmapped) (unmapped)
	// ...
	while (lowaddr <= highaddr)
	{
		char membufs[4][11];
		int membufi;
		uint32_t *va;

		memset(membufs, 0, 4 * 11);
		if (is_virtual_range)
		{
			for (va = (uint32_t *) lowaddr, membufi = 0;
			     va < ((uint32_t *) lowaddr) + 4 && va <= (uint32_t*)highaddr;
			     va++, membufi++)
			{
				if (page_lookup(pgdir, va, NULL) == NULL)
				{
					strcpy(membufs[membufi], "(unmapped)");
				}
				else
				{
					snprintf(membufs[membufi], 11, "%0#10x", *va);
				}
			}
		}
		else
		{
			for (va = KADDR(lowaddr), membufi = 0;
			     va < ((uint32_t *)KADDR(lowaddr)) + 4 &&
				     va <= (uint32_t *)KADDR(highaddr);
			     va++, membufi++)
			{
				snprintf(membufs[membufi], 11, "%0#10x", *va);
			}
		}

		cprintf("%0#10x: %s %s %s %s\n", lowaddr,
			membufs[0], membufs[1], membufs[2], membufs[3]);
		lowaddr += 0x10;
	}

	return 0;
}

int
mon_backtrace(int argc, char **argv, struct Trapframe *tf)
{
	uint32_t ebp;
	int frame_count = 0;
	for (ebp = read_ebp(); ebp != 0; ebp = *(uint32_t *)ebp) {
		cprintf("ebp: %08x  eip %08x  args %08x %08x %08x %08x %08x\n",
			ebp,
			((uint32_t *)ebp)[1],
			((uint32_t *)ebp)[2],
			((uint32_t *)ebp)[3],
			((uint32_t *)ebp)[4],
			((uint32_t *)ebp)[5],
			((uint32_t *)ebp)[6]);
		frame_count++;
	}
	return frame_count;
}



/***** Kernel monitor command interpreter *****/

#define WHITESPACE "\t\r\n "
#define MAXARGS 16

static int
runcmd(char *buf, struct Trapframe *tf)
{
	int argc;
	char *argv[MAXARGS];
	int i;

	// Parse the command buffer into whitespace-separated arguments
	argc = 0;
	argv[argc] = 0;
	while (1) {
		// gobble whitespace
		while (*buf && strchr(WHITESPACE, *buf))
			*buf++ = 0;
		if (*buf == 0)
			break;

		// save and scan past next arg
		if (argc == MAXARGS-1) {
			cprintf("Too many arguments (max %d)\n", MAXARGS);
			return 0;
		}
		argv[argc++] = buf;
		while (*buf && !strchr(WHITESPACE, *buf))
			buf++;
	}
	argv[argc] = 0;

	// Lookup and invoke the command
	if (argc == 0)
		return 0;
	for (i = 0; i < ARRAY_SIZE(commands); i++) {
		if (strcmp(argv[0], commands[i].name) == 0)
			return commands[i].func(argc, argv, tf);
	}
	cprintf("Unknown command '%s'\n", argv[0]);
	return 0;
}

void
monitor(struct Trapframe *tf)
{
	char *buf;

	cprintf("Welcome to the JOS kernel monitor!\n");
	cprintf("Type 'help' for a list of commands.\n");

	if (tf != NULL)
		print_trapframe(tf);

	while (1) {
		buf = readline("Hacker> ");
		if (buf != NULL)
			runcmd(buf, tf);
	}
}
