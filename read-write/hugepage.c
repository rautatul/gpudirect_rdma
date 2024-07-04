// SPDX-License-Identifier: GPL-2.0

#include "hugepage.h"

struct hugepageallocator_t gAllocator = {
	.current_addr = NULL,
	.page_fd = 0,
	.init = &hugepage_init,
	.alloc = &hugepage_alloc,
	.free = &hugepage_free,
	.close = &hugepage_close
};

/* Implementation */
void hugepage_init(void)
{
	gAllocator.page_fd = open(FILE_NAME, O_CREAT |  O_RDWR, 0755);
	if (gAllocator.page_fd < 0) {
		perror("Open failed");
		exit(1);
	}
}

void *hugepage_alloc(size_t struct_size)
{
	gAllocator.current_addr = mmap(ADDR, struct_size,
				 PROTECTION, FLAGS, gAllocator.page_fd, 0);
	if (gAllocator.current_addr == MAP_FAILED) {
		perror("mmap");
		unlink(FILE_NAME);
		exit(1);
	}

	return gAllocator.current_addr;
}

void hugepage_free(void *addr, size_t struct_size)
{
	/* Free a particular page denoted by the address */
	// printf("Freeing %p\n", addr);
	munmap(addr, struct_size);
}

void hugepage_close(void)
{
	close(gAllocator.page_fd);
	unlink(FILE_NAME);
}
