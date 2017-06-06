
#include "fs.h"

// Return the virtual address of this disk block.
void*
diskaddr(uint32_t blockno)
{
	if (blockno == 0 || (super && blockno >= super->s_nblocks))
		panic("bad block number %08x in diskaddr", blockno);
	return (char*) (DISKMAP + blockno * BLKSIZE);
}

// Reverse operation to diskaddr()
uint32_t
blocknum(void *addr)
{
	if ((uint32_t)addr < DISKMAP || (uint32_t)addr > DISKMAP + DISKSIZE)
		panic("blocknum: bad addr: %p", addr);

	return ((uint32_t)addr - DISKMAP) / BLKSIZE;
}


// Is this virtual address mapped?
bool
va_is_mapped(void *va)
{
	return (uvpd[PDX(va)] & PTE_P) && (uvpt[PGNUM(va)] & PTE_P);
}

// Is this virtual address dirty?
bool
va_is_dirty(void *va)
{
	return (uvpt[PGNUM(va)] & PTE_D) != 0;
}

bool
va_is_accessed(void *va)
{
	return (uvpt[PGNUM(va)] & PTE_A) != 0;
}


#define MEMBLKSZ 50
#define MEMBLKTHRESH (0.9 * MEMBLKSZ)

// Adds a new block to the tracked blocks, and possibly evicts old ones
static void
manage_eviction(uint32_t newblockno)
{
	static uint32_t memblocks[MEMBLKSZ];
	static uint32_t curblock = 0;
	int dirty_swap;

	memblocks[curblock++] = newblockno;
	dirty_swap = curblock - 1;

	while (curblock >= MEMBLKTHRESH)
	{
		// Evict old blocks
		int i;
		int r;

		for (i = 0;
		     i < curblock && curblock > (MEMBLKTHRESH / 2);
		     i++)
		{
			void *va = diskaddr(memblocks[i]);
			bool is_accessed = va_is_accessed(va);
			bool is_dirty = va_is_dirty(va);

			// Clear the access bit
			if (is_dirty)
			{
				if (dirty_swap > i)
				{
					uint32_t t = memblocks[dirty_swap];
					memblocks[dirty_swap] = memblocks[i];
					memblocks[i] = t;

					dirty_swap--;
					continue;
				}
				else
				{
					break;
				}
			}
			else
				if ((r = sys_page_map(0, va, 0, va,
						      PTE_SYSCALL)) < 0)
					panic("sys_page_map: %e", r);

			if (!is_accessed)
			{
				// If it wasn't accessed, evict block.
				if ((r = sys_page_unmap(0, va)) < 0)
					panic("Couldn't free block: %e", r);

				// Bring last block in this new hole.
				memblocks[i] = memblocks[curblock - 1];
				i--;
				curblock--;
			}
		}
	}
}

// Fault any disk block that is read in to memory by
// loading it from disk.
static void
bc_pgfault(struct UTrapframe *utf)
{
	void *addr = (void *) utf->utf_fault_va;
	uint32_t blockno = blocknum(addr);
	int r;

	// Check that the fault was within the block cache region
	if (addr < (void*)DISKMAP || addr >= (void*)(DISKMAP + DISKSIZE))
		panic("page fault in FS: eip %08x, va %08x, err %04x",
		      utf->utf_eip, addr, utf->utf_err);

	// Sanity check the block number.
	if (super && blockno >= super->s_nblocks)
		panic("reading non-existent block %08x\n", blockno);

	// Allocate a page in the disk map region, read the contents
	// of the block from the disk into that page.
	// Hint: first round addr to page boundary. fs/ide.c has code to read
	// the disk.
	//
	// LAB 5: your code here:
	addr = ROUNDDOWN(addr, PGSIZE);
	if ((r = sys_page_alloc(0, addr, PTE_U | PTE_W)) < 0)
		panic("page fault failed to allocate page: %e\n", r);

	if ((r = ide_read(blockno * BLKSECTS, addr, BLKSECTS)) < 0)
		panic("page fault failed to ide_read: %\n", r);

	// Clear the dirty bit for the disk block page since we just read the
	// block from disk
	if ((r = sys_page_map(0, addr, 0, addr, uvpt[PGNUM(addr)] & PTE_SYSCALL)) < 0)
		panic("in bc_pgfault, sys_page_map: %e", r);

	// Check that the block we read was allocated. (exercise for
	// the reader: why do we do this *after* reading the block
	// in?)
	if (bitmap && block_is_free(blockno))
		panic("reading free block %08x\n", blockno);

	// If we are running low on memory, clean up
	// FIXME: Doesn't work since we added journaling, because of the added
	// complexity that we can't flush dirty blocks whenever we like
	// anymore.
	// manage_eviction(blockno);
}

// Flush the contents of the block containing VA out to disk if
// necessary, then clear the PTE_D bit using sys_page_map.
// If the block is not in the block cache or is not dirty, does
// nothing.
// Hint: Use va_is_mapped, va_is_dirty, and ide_write.
// Hint: Use the PTE_SYSCALL constant when calling sys_page_map.
// Hint: Don't forget to round addr down.
void
flush_block(void *addr)
{
	int r;
	uint32_t blockno = blocknum(addr);

	if (addr < (void*)DISKMAP || addr >= (void*)(DISKMAP + DISKSIZE))
		panic("flush_block of bad va %08x", addr);

	// LAB 5: Your code here.
	addr = ROUNDDOWN(addr, BLKSIZE);

	if (!va_is_mapped(addr) || !va_is_dirty(addr))
		return;

	if ((r = ide_write(blockno * BLKSECTS, addr, BLKSECTS)) < 0)
		panic("ide_write: %e\n", r);

	if ((r = sys_page_map(0, addr, 0, addr, PTE_SYSCALL)) < 0)
		panic("sys_page_map: %e\n", r);
}

// Test that the block cache works, by smashing the superblock and
// reading it back.
static void
check_bc(void)
{
	struct Super backup;

	// back up super block
	memmove(&backup, diskaddr(1), sizeof backup);

	// smash it
	strcpy(diskaddr(1), "OOPS!\n");
	flush_block(diskaddr(1));
	assert(va_is_mapped(diskaddr(1)));
	assert(!va_is_dirty(diskaddr(1)));

	// clear it out
	sys_page_unmap(0, diskaddr(1));
	assert(!va_is_mapped(diskaddr(1)));

	// read it back in
	assert(strcmp(diskaddr(1), "OOPS!\n") == 0);

	// fix it
	memmove(diskaddr(1), &backup, sizeof backup);
	flush_block(diskaddr(1));

	cprintf("block cache is good\n");
}

void
bc_init(void)
{
	struct Super super;
	set_pgfault_handler(bc_pgfault);
	check_bc();

	// cache the super block by reading it once
	memmove(&super, diskaddr(1), sizeof super);
}
