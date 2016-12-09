#ifndef __PATCHER_VMA_H__
#define __PATCHER_VMA_H__

#include <stdint.h>
#include <stdlib.h>
#include "list.h"

struct vma_area {
	struct list_head	list;

	uint64_t		start;
	uint64_t		end;
	uint64_t		pgoff;
	uint32_t		prot;
	uint32_t		flags;
	char			*path;
	int			deleted;
};

int collect_vmas(pid_t pid, struct list_head *head);
void print_vmas(pid_t pid, struct list_head *head);

const struct vma_area *find_vma_by_addr(const struct list_head *vmas,
					unsigned long addr);
const struct vma_area *find_vma_by_prot(struct list_head *head, int prot);
const struct vma_area *find_vma_by_path(struct list_head *head,
					const char *path);

unsigned long find_vma_hole(const struct list_head *vmas,
			    unsigned long hint, size_t size);

#endif /* __PATCHER_VMA_H__ */
