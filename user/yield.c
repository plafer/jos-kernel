// yield the processor to other environments

#include <inc/lib.h>

void
umain(int argc, char **argv)
{
	int i;

	cprintf("Hello, I am environment %08x on CPU %d.\n",
		thisenv->env_id, sys_get_cpu());
	for (i = 0; i < 5; i++) {
		sys_yield();
		cprintf("Back in environment %08x on CPU %d, iteration %d.\n",
			thisenv->env_id, sys_get_cpu(), i);
	}
	cprintf("All done in environment %08x.\n", thisenv->env_id);
}
