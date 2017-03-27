// Simple implementation of cprintf console output for the kernel,
// based on printfmt() and the kernel console's cputchar().

#include <inc/types.h>
#include <inc/stdio.h>
#include <inc/stdarg.h>

static char *log_levels[] = {
	"DEBUG",
	"INFO",
	"CRIT"
};

static void
putch(int ch, int *cnt)
{
	cputchar(ch);
	*cnt++;
}

int
vcprintf(const char *fmt, va_list ap)
{
	int cnt = 0;

	vprintfmt((void*)putch, &cnt, fmt, ap);
	return cnt;
}

int
cprintf(const char *fmt, ...)
{
	va_list ap;
	int cnt;

	va_start(ap, fmt);
	cnt = vcprintf(fmt, ap);
	va_end(ap);

	return cnt;
}

int
clogf(enum LOG_LEVEL lvl, char *subsys, const char *fmt, ...)
{
	va_list ap;
	int cnt;

	if (lvl > LOG_NUM)
		lvl = LOG_CRIT;

	cnt = cprintf("[%s] %s: ", log_levels[lvl], subsys);

	va_start(ap, fmt);
	cnt += vcprintf(fmt, ap);
	va_end(ap);

	return cnt;
}
