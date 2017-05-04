#include <inc/lib.h>

void
umain(int argc, char **argv)
{
	int i = 0;
	asm volatile("int $3");	// Start debug
	for (i = 0; i< 100; i++);
	asm volatile("int $3");	// Start debug
}
