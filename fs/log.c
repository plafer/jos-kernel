#include "fs.h"

#define debug 0

struct LogHeader {
	uint32_t *pnblocks;
	uint32_t *blocknos;
	uint32_t (*log_entries)[BLKSIZE / 4];
};

static struct LogHeader log_header;

void log_init(void)
{
	uint32_t *logstart = diskaddr(super->s_logstart);

	log_header.pnblocks = logstart;
	log_header.blocknos = &logstart[1];

	if (debug)
	{
		cprintf("Log: initial number of blocks: %d\n",
			*log_header.pnblocks);
	}

	// Log entries start at the second block in the log
	log_header.log_entries = (void *)
		(logstart + (BLKSIZE / sizeof(*logstart)));

	if (*log_header.pnblocks > 0)
	{
		if (debug)
			cprintf("Fs recovering from crash...");
		log_commit();
	}
}

void log_write(void *addr)
{
	int i;
	uint32_t blockno;

	if ((uint32_t)addr < DISKMAP || (uint32_t)addr >= DISKMAP + DISKSIZE)
		panic("Log writing to address out of range.");

	addr = ROUNDDOWN(addr, BLKSIZE);
	blockno = blocknum(addr);

	if (debug)
		cprintf("Writing block no %d in log... ", blockno);

	// See if block is already in the cache
	for (i = 0; i < *log_header.pnblocks; i++)
	{
		if (blockno == log_header.blocknos[i])
		{
			if (debug)
				cprintf("Was already in the cache at "
					"index %d\n", i);

			memcpy(log_header.log_entries[i], addr, BLKSIZE);
			return;
		}
	}

	if (*log_header.pnblocks >= super->s_lognblocks)
		panic("Out of log space  (%d >= %d).",
		      *log_header.pnblocks, super->s_lognblocks);

	if (debug)
		cprintf("At the end of log, index %d\n", *log_header.pnblocks);

	log_header.blocknos[*log_header.pnblocks] = blockno;
	memcpy(log_header.log_entries[*log_header.pnblocks], addr, BLKSIZE);

	*log_header.pnblocks += 1;
}

static void flush_log(void)
{
	int i;

	if (debug)
		cprintf("Flushing log... Blocks: ");

	for (i = 0; i < *log_header.pnblocks; i++)
	{
		if (debug)
			cprintf("%p ", log_header.log_entries[i]);
		flush_block(log_header.log_entries[i]);
	}

	// Flushes log_header.pnblocks and log_header.blocknos
	if (debug)
		cprintf(". As well as log header: %p\n", log_header.pnblocks);
	flush_block(log_header.pnblocks);
}

void log_commit(void)
{
	int i;

	if (debug)
		cprintf("Committing log...\n");

	flush_log();

	// Copy blocks from log to their actual block location
	for (i = 0; i < *log_header.pnblocks; i++)
	{
		void *actual_loc = diskaddr(log_header.blocknos[i]);
		void *log_loc = log_header.log_entries[i];

		memcpy(actual_loc, log_loc, BLKSIZE);
		flush_block(actual_loc);
	}

	*log_header.pnblocks = 0;
	flush_block(log_header.pnblocks);
}

#undef debug
