#include <inc/x86.h>

#include <kern/cpuid.h>

// TODO: test whether CPUID is supported either
// 1. on invalid opcode exception
// 2. Trying to set EFLAG's ID bit
// See: http://wiki.osdev.org/CPUID#Checking_CPUID_availability

static bool isloaded;
static char vendorID[13] = "";
static uint32_t cpuid_ecx = -1;
static uint32_t cpuid_edx = -1;

static void load_cpuid(void)
{
	uint32_t eax;
	uint32_t ebx;
	uint32_t ecx;
	uint32_t edx;

	// 1. EAX = 0 (CPUID_GETVENDORSTRING)
	eax = 0;
	// Note: EAX would be set to mmaximum EAX value supported for CPUID calls, but
	cpuid(eax, NULL, &ebx, &ecx, &edx);

	//       MSB         LSB
	// EBX = 'u' 'n' 'e' 'G'
        // EDX = 'I' 'e' 'n' 'i'
        // ECX = 'l' 'e' 't' 'n'
	*(uint32_t *) &vendorID[0] = ebx;
	*(uint32_t *) &vendorID[4] = edx;
	*(uint32_t *) &vendorID[8] = ecx;
	vendorID[12] = '\0';

	// 2. EAX = 1 (CPUID_GETFEATURES)
	eax = 1;
	cpuid(eax, NULL, NULL, &cpuid_ecx, &cpuid_edx);

	isloaded = true;
}

char *cpu_vendorID()
{
	if (!isloaded)
		load_cpuid();

	return vendorID;
}

int cpu_hasecxfeat(int feature)
{
	if (!isloaded)
		load_cpuid();

	return cpuid_ecx & feature;
}

int cpu_hasedxfeat(int feature)
{
	if (!isloaded)
		load_cpuid();

	return cpuid_edx & feature;
}
