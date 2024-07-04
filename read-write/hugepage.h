/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2019 Attala Systems Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef HUGEPAGE_H
#define HUGEPAGE_H

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>

#define FILE_NAME "/mnt/hugetlbfs/huge"
#define LENGTH (2UL*1024*1024)
#define PROTECTION (PROT_READ | PROT_WRITE)
#define ADDR (void *)(0x0UL)
#define FLAGS (MAP_SHARED)

struct hugepageallocator_t {
	void *current_addr;
	int page_fd;
	void (*init)(void);
	void * (*alloc)(size_t len);
	void (*free)(void *addr, size_t len);
	void (*close)(void);
};

void hugepage_init(void);
void *hugepage_alloc(size_t struct_size);
void hugepage_free(void *addr, size_t struct_size);
void hugepage_close(void);

#endif
