#ifndef __PATCHER_PROCESS_H__
#define __PATCHER_PROCESS_H__

#include <stdint.h>
#include <fcntl.h>

struct process_ctx_s;

long process_get_place(struct process_ctx_s *ctx, unsigned long hint, size_t size);
int process_unlink(struct process_ctx_s *ctx);
int process_cure(struct process_ctx_s *ctx);
int process_link(struct process_ctx_s *ctx);
int process_infect(struct process_ctx_s *ctx);

struct vma_area;

int process_open_file(struct process_ctx_s *ctx, const char *path,
			int flags, mode_t mode);

int process_suspend(struct process_ctx_s *ctx, const char *target_bid);

struct dl_map;
int process_mmap_dl_map(struct process_ctx_s *ctx, const struct dl_map *dlm);
int process_munmap_dl_map(struct process_ctx_s *ctx, const struct dl_map *dlm);

int64_t process_exec_code(struct process_ctx_s *ctx, uint64_t addr,
			  void *code, size_t code_size);

int process_inject_service(struct process_ctx_s *ctx);
int process_shutdown_service(struct process_ctx_s *ctx);

int process_collect_needed(struct process_ctx_s *ctx);

int process_collect_vmas(struct process_ctx_s *ctx);
int process_find_target_dlm(struct process_ctx_s *ctx);

int64_t process_find_place_for_elf(struct process_ctx_s *ctx,
				   uint64_t hint, size_t size);

void process_print_mmap(const struct vma_area *vma);
void process_print_munmap(const struct vma_area *vma);

int process_send_fd(struct process_ctx_s *ctx, int fd);

#endif
