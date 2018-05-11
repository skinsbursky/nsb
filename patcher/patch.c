#include "nsb_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <dlfcn.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <linux/limits.h>
#include <sys/mman.h>

#include "include/patch.h"
#include "include/log.h"
#include "include/xmalloc.h"
#include "include/vma.h"
#include "include/elf.h"

#include "include/context.h"
#include "include/process.h"
#include "include/x86_64.h"
#include "include/backtrace.h"
#include "include/protobuf.h"
#include "include/relocations.h"
#include "include/dl_map.h"

struct process_ctx_s process_context = {
	.service = {
		.name = "libnsb_service.so",
		.sock = -1,
	},
	.vmas = LIST_HEAD_INIT(process_context.vmas),
	.dl_maps = LIST_HEAD_INIT(process_context.dl_maps),
	.needed_list = LIST_HEAD_INIT(process_context.needed_list),
	.threads = LIST_HEAD_INIT(process_context.threads),
	.applied_patches = LIST_HEAD_INIT(process_context.applied_patches),
	.remote_vma = {
		.list = LIST_HEAD_INIT(process_context.remote_vma.list),
		.length = 4096,
		.flags = MAP_ANONYMOUS | MAP_PRIVATE,
		.prot = PROT_READ | PROT_WRITE | PROT_EXEC,
	},
};

static int write_func_code(struct process_ctx_s *ctx, struct func_jump_s *fj)
{
	pr_info("  - Restoring code in \"%s\":\n", fj->name);
	pr_info("      old address: %#lx\n", fj->func_addr);

	return ctx->arch_callback->process_write_data(ctx, fj->func_addr,
				  fj->code, sizeof(fj->code));
}

static int write_func_jump(const struct patch_s *p, struct func_jump_s *fj,
			   void *data)
{
	struct process_ctx_s *ctx = data;
	uint64_t patch_addr;

	patch_addr = dlm_load_base(p->patch_dlm) + fj->patch_value;

	pr_info("  - Function \"%s\":\n", fj->name);
	pr_info("      jump: %#lx ---> %#lx (%s)\n", fj->func_addr, patch_addr,
						     p->patch_dlm->path);

	if (ctx->dry_run)
		return 0;

	return ctx->arch_callback->process_write_data(ctx, fj->func_addr,
				  fj->func_jump, sizeof(fj->func_jump));
}

static int iterate_patch_function_jumps(const struct patch_s *p,
					int (*actor)(const struct patch_s *p,
						     struct func_jump_s *fj,
						     void *data),
					void *data)
{
	const struct patch_info_s *pi = &p->pi;
	int i, err;

	for (i = 0; i < pi->n_func_jumps; i++) {
		struct func_jump_s *fj = pi->func_jumps[i];

		err = actor(p, fj, data);
		if (err)
			return err;
	}
	return 0;
}

static int apply_func_jumps(struct process_ctx_s *ctx)
{
	int err;

	pr_info("= Apply function jumps:\n");
	err = iterate_patch_function_jumps(P(ctx), write_func_jump, ctx);
	if (err)
		pr_err("failed to apply function jump\n");
	return 0;
}

static int read_func_jump_code(const struct dl_map *target_dlm, struct func_jump_s *fj)
{
	int fd, err = 0;
	ssize_t ret;
	size_t size = sizeof(fj->code);
	off_t offset;
	const char *map_file = target_dlm->exec_vma->map_file;
	const struct elf_info_s *ei = target_dlm->ei;

	fd = open(map_file, O_RDONLY);
	if (fd == -1) {
		pr_perror("failed to open %s", map_file);
		return -errno;
	}

	offset = fj->func_value - elf_section_virt_base(ei, fj->shndx);

	if (lseek(fd, offset, SEEK_SET) != offset) {
		pr_err("failed to set offset %#lx in %s",
				fj->func_value, map_file);
		err = -errno;
		goto close_fd;
	}

	ret = read(fd, fj->code, size);
	if (ret != size) {
		if (ret == -1) {
			pr_perror("failed to read %s", map_file);
			err = -errno;
		} else {
			pr_perror("read from %s less than requested: %ld < %ld",
					map_file, ret, size);
			err = -EINVAL;
		}
	}

close_fd:
	close(fd);
	return err;
}

/* How to create a jump to new function?
 * We have here:
 *   - function value in target binary
 *   - function value in path
 * And we want to create jump instruction, which uses relative offset to
 * the *next* command (i.e. next after jump instruction we want).
 * Thus to create a jump instruction we need:
 *   1) Add target executable start address to function value in target binary.
 *      This is so-called "target position".
 *   2) Add patch executable start address to function value in patch binary
 *      plus size of jump command.
 *      This is so-called "current position".
 * We can then use these two addresses, to find offset from current positon to
 * target one, and write it as a part of jump command.
 */
static int tune_patch_func_jump(const struct patch_s *p, struct func_jump_s *fj,
				void *data)
{
	uint64_t patch_addr;
	ssize_t size;

	fj->func_addr = dlm_load_base(p->target_dlm) + fj->func_value;
	patch_addr = dlm_load_base(p->patch_dlm) + fj->patch_value;

        size = x86_jmpq_instruction(fj->func_jump, sizeof(fj->func_jump),
				    fj->func_addr, patch_addr);
	if (size < 0)
		return size;

	return read_func_jump_code(p->target_dlm, fj);
}

static int tune_patch_func_jumps(struct patch_s *p)
{
	int err;

	err = iterate_patch_function_jumps(p, tune_patch_func_jump, NULL);
	if (err)
		pr_err("failed to tune function jump\n");
	return 0;
}

static int print_patch_func_jump(const struct patch_s *p, struct func_jump_s *fj,
				 void *data)
{
	pr_info("  - Function \"%s\":\n", fj->name);
	pr_info("      original address: %#lx\n", fj->func_addr);
	pr_info("      patch address   : %#lx\n",
			dlm_load_base(p->patch_dlm) + fj->patch_value);
	return 0;
}

static int tune_func_jumps(struct process_ctx_s *ctx)
{
	int err;

	pr_info("= Tune function jumps:\n");

	err = tune_patch_func_jumps(P(ctx));
	if (err)
		return err;

	return iterate_patch_function_jumps(P(ctx), print_patch_func_jump, NULL);
}

static int patch_unload(struct process_ctx_s *ctx, const struct patch_s *p)
{
	const struct dl_map *dlm = p->patch_dlm;

	pr_info("= Unloading %s:\n", dlm->path);

	return unload_elf(ctx, dlm);
}

static int unload_patch(struct process_ctx_s *ctx)
{
	return patch_unload(ctx, P(ctx));
}

static int load_patch(struct process_ctx_s *ctx)
{
	int err;
	struct dl_map *dlm;

	pr_info("= Loading %s:\n", ctx->patchfile);

	dlm = alloc_dl_map(ctx->patch_ei, ctx->patchfile);
	if (!dlm)
		return -ENOMEM;

	err = load_elf(ctx, dlm, TDLM(ctx));
	if (err)
		goto destroy_dlm;

	list_add_tail(&P(ctx)->list, &ctx->applied_patches);
	P(ctx)->patch_dlm = dlm;
	return 0;

destroy_dlm:
	return err;
}

static int func_jump_applied(struct process_ctx_s *ctx,
			     const struct func_jump_s *fj)
{
	int err;
	uint8_t code[8];

	BUILD_BUG_ON(sizeof(code) != sizeof(fj->func_jump));

	if (!fj->func_addr)
		return 0;

	err = ctx->arch_callback->process_read_data(ctx, fj->func_addr, code, sizeof(code));
	if (err)
		return err;

	return !memcmp(code, fj->func_jump, sizeof(code));
}

static int find_function_jump(const struct patch_info_s *pi,
			      int (*compare)(const struct func_jump_s *fj,
					     const void *data),
			      const void *data,
			      struct func_jump_s **func_jump)
{
	int i, ret;

	for (i = 0; i < pi->n_func_jumps; i++) {
		struct func_jump_s *fj = pi->func_jumps[i];

		ret = compare(fj, data);
		if (ret < 0)
			return ret;
		if (ret) {
			*func_jump = fj;
			return 0;
		}
	}
	return -ENOENT;
}

static int compare_fj_addr(const struct func_jump_s *fj, const void *data)
{
	uint64_t addr = *(uint64_t *)data;

	return fj->func_addr == addr;
}

static int find_previous_func_jump(const struct process_ctx_s *ctx,
				   const struct patch_s *p,
				   const struct func_jump_s *fj,
				   const struct patch_s **prev_patch,
				   struct func_jump_s **prev_func_jump)
{
	const struct patch_s *pp = p;
	int err;

	list_for_each_entry_continue_reverse(pp, &ctx->applied_patches, list) {
		if (pp->target_dlm != p->target_dlm)
			continue;

		err = find_function_jump(&pp->pi, compare_fj_addr,
					 &fj->func_addr, prev_func_jump);
		if (err != -ENOENT) {
			*prev_patch = pp;
			return err;
		}
	}
	return -ENOENT;
}

static int do_revert_func_jump(struct process_ctx_s *ctx,
			       const struct patch_s *p,
			       struct func_jump_s *fj)
{
	int err;
	const struct patch_s *prev_patch;
	struct func_jump_s *prev_func_jump;

	err = find_previous_func_jump(ctx, p, fj, &prev_patch, &prev_func_jump);
	if (err < 0) {
		if (err != -ENOENT)
			return err;
		return write_func_code(ctx, fj);
	}
	return write_func_jump(prev_patch, prev_func_jump, ctx);
}

static int revert_func_jump(const struct patch_s *p, struct func_jump_s *fj,
			    void *data)
{
	struct process_ctx_s *ctx = data;
	int applied;

	applied = func_jump_applied(ctx, fj);
	if (applied <= 0)
		return applied;

	return do_revert_func_jump(ctx, p, fj);
}

static int patch_revert_func_jumps(struct process_ctx_s *ctx, struct patch_s *p)
{
	int err;

	pr_info("= Revert function jumps:\n");
	err = iterate_patch_function_jumps(p, revert_func_jump, ctx);
	if (err)
		pr_err("failed to revert function jump\n");
	return 0;
}

static int revert_func_jumps(struct process_ctx_s *ctx)
{
	return patch_revert_func_jumps(ctx, P(ctx));
}

static int write_static_ref(const struct process_ctx_s *ctx,
			    uint64_t addr,
			    int64_t offset, int64_t offset_size)
{
	char bytes[8];
	const void *off = &offset;
	int offset_small = offset;

	if (offset_size == 4) {
		int err;

		err = ctx->arch_callback->process_read_data(ctx, addr, bytes, sizeof(bytes));
		if (err)
			return err;

		off = &offset_small;
	}

	memcpy(bytes, off, offset_size);

	return ctx->arch_callback->process_write_data(ctx, addr, bytes, sizeof(bytes));
}

/*
 * How to fix a static variable reference?
 * All this commands uses relative addressation and thus we have to create
 * "offset" and place it into the right place.
 * We are provided with:
 *   - patch_size - this is size of the offset we need to write.
 *   - patch_address - this is address in patch, which has to be patched with
 *                     the offset.
 *   - target_value - some interim value.
 *
 * Offset has to be calculated by using the following formula:
 *
 *    offset = target_value + "target ELF load base" - "Patch load base"
 *
 * What is "target_value"?
 * This interim value was constructed by generator to simplify offset
 * calculation here in patcher.
 *
 * Consider relocation generated for some symbol,
 * having address Sn in new library:
 *
 * Rn = Sn + X
 *
 * where X indicates terms indepedent of address symbols (relocation offset,
 * addend, etc). If the new library should refer to symbol from old library,
 * then, by relocation definition, its value should be:
 *
 * Ro = So + X = (So - Sn) + (Sn + X) = Rn + (So - Sn)
 *
 * Note that all arithmetic here is done modulo 2**64
 *
 * Symbol address equals library load address Lx plus offset to library Dx:
 *
 * So = Lo + Do
 * Sn = Ln + Dn
 *
 * Substituting these to formula above and rearranging terms, we get
 *
 * Ro = (Rn + Do - Dn) + (Lo - Ln)
 *
 * Value in the first parens is 'target_value'
 */
static int apply_static_ref(struct process_ctx_s *ctx,
			    const struct static_sym_s *ss)
{
	uint64_t patch_ref_addr, var_addr, reloc;

	patch_ref_addr = dlm_load_base(PDLM(ctx)) + ss->patch_address;

	reloc = ss->target_value + dlm_load_base(TDLM(ctx)) -
		 dlm_load_base(PDLM(ctx));

	var_addr = patch_ref_addr + reloc + ss->patch_size;

	if (ss->patch_size < 8) {
		int reloc_bit_size;
		uint64_t reloc_sign, reloc_high_bits;

		reloc_bit_size = 8 * ss->patch_size;
		reloc_sign = (reloc >> (reloc_bit_size  - 1)) & 1;
		reloc_high_bits = (reloc_sign ? ~reloc : reloc) >> reloc_bit_size;

		if (reloc_high_bits) {
			pr_err("Relocation %#lx at offset %#lx overflows\n",
					reloc, var_addr);
			return -EINVAL;
		}
	}

	pr_debug("  - ref: %#lx ---> %#lx (%#lx + %#lx)\n", patch_ref_addr,
			var_addr, dlm_load_base(TDLM(ctx)),
			var_addr - dlm_load_base(TDLM(ctx)));

	return write_static_ref(ctx, patch_ref_addr, reloc, ss->patch_size);
}

static int apply_static_refs(struct process_ctx_s *ctx)
{
	int i, err;
	struct static_sym_s *ss;

	pr_info("= Fix static variables references:\n");

	for (i = 0; i < PI(ctx)->n_static_syms; i++) {
		ss = PI(ctx)->static_syms[i];

		err = apply_static_ref(ctx, ss);
		if (err)
			return err;
	}
	return 0;
}

static int apply_dyn_binpatch(struct process_ctx_s *ctx)
{
	int err;

	err = load_patch(ctx);
	if (err) {
		pr_err("failed to load patch\n");
		return err;
	}

	err = apply_relocations(ctx);
	if (err)
		goto unload_patch;

	err = apply_static_refs(ctx);
	if (err)
		goto unload_patch;

	err = tune_func_jumps(ctx);
	if (err)
		goto unload_patch;

	err = apply_func_jumps(ctx);
	if (err)
		goto revert_jumps;

	return 0;

revert_jumps:
	if (revert_func_jumps(ctx))
		pr_err("failed to revert function jumps\n");
	return err;

unload_patch:
	if (unload_patch(ctx))
		pr_err("failed to unload patch\n");
	return err;
}

int patch_set_target_dlm(struct process_ctx_s *ctx, struct patch_s *p)
{
	const char *bid = p->pi.target_bid;

	p->target_dlm = find_dl_map_by_bid(&ctx->dl_maps, bid);
	if (!p->target_dlm)
		pr_warn("failed to find vma with Build ID %s in process %d\n",
				bid, ctx->pid);
	return 0;
}

int create_patch_by_dlm(struct process_ctx_s *ctx, const struct dl_map *dlm,
			struct patch_s **patch)
{
	struct patch_s *p;
	int err;

	pr_info("  %s: %s\n", dlm->path, elf_bid(dlm->ei));

	p = xmalloc(sizeof(*p));
	if (!p)
		return -ENOMEM;
	p->patch_dlm = dlm;

	err = elf_info_binpatch(&p->pi, dlm->ei);
	if (err)
		goto free_patch;

	err = patch_set_target_dlm(ctx, p);
	if (err)
		goto free_patch;

	if (p->target_dlm && p->patch_dlm->exec_vma) {
		err = tune_patch_func_jumps(p);
		if (err)
			goto free_patch;
	}

	INIT_LIST_HEAD(&p->rela_plt);
	INIT_LIST_HEAD(&p->rela_dyn);

	print_dl_vmas(p->patch_dlm);

	*patch = p;
	return 0;

free_patch:
	free(p);
	return err;
}

static int create_patch(struct elf_info_s *ei, struct patch_s **patch)
{
	int err;
	struct patch_s *p;

	p = xmalloc(sizeof(*p));
	if (!p)
		return -ENOMEM;

	err = elf_info_binpatch(&p->pi, ei);
	if (err)
		goto free_patch;

	INIT_LIST_HEAD(&p->rela_plt);
	INIT_LIST_HEAD(&p->rela_dyn);

	*patch = p;

	return 0;

free_patch:
	free(p);
	return err;
}

struct patch_s *find_patch_by_bid(struct process_ctx_s *ctx, const char *bid)
{
	struct patch_s *p;

	list_for_each_entry(p, &ctx->applied_patches, list) {
		if (!strcmp(p->pi.patch_bid, bid))
			return p;
	}
	return NULL;
}

static int process_find_patch(struct process_ctx_s *ctx)
{
	const char *bid = PI(ctx)->patch_bid;

	pr_info("= Cheking for %s patch is applied...\n", bid);

	if (find_patch_by_bid(ctx, bid)) {
		pr_err("Patch with Build ID %s is already applied\n", bid);
		return -EEXIST;
	}
	return 0;
}

static int init_patch(struct process_ctx_s *ctx)
{
	int err;
	struct elf_info_s *ei;

	err = elf_create_info(ctx->patchfile, &ei);
	if (err)
		return err;

	err = create_patch(ei, &ctx->patch);
	if (err)
		goto destroy_elf;

	ctx->patch_ei = ei;
	return 0;

destroy_elf:
	elf_destroy_info(ei);
	return err;
}

int process_resume(struct process_ctx_s *ctx)
{
	int err;

	err = process_shutdown_service(ctx);
	if (err)
		return err;

	err = process_unlink(ctx);
	if (err)
		return err;

	pr_info("= Resuming %d\n", ctx->pid);
	return process_cure(ctx);
}

struct bt_fj_data {
	const struct backtrace_s *bt;
	uint64_t start;
};

static int jump_check_backtrace(const struct patch_s *p, struct func_jump_s *fj,
				void *data)
{
	const struct bt_fj_data *d = data;

	return backtrace_check_func(fj, d->bt, d->start);
}

static int jumps_check_backtrace(const struct process_ctx_s *ctx,
				 const struct backtrace_s *bt,
				 uint64_t start, uint64_t end)
{
	struct bt_fj_data data = {
		.bt = bt,
		.start = start,
	};

	return iterate_patch_function_jumps(P(ctx), jump_check_backtrace, &data);
}

static int init_context(struct process_ctx_s *ctx, pid_t pid,
			const char *patchfile, int dry_run)
{
	if (elf_library_status())
		return -1;

	pr_info("Patch context:\n");
	pr_info("  Pid        : %d\n", pid);

	ctx->pid = pid;
	ctx->patchfile = patchfile;
	ctx->dry_run = dry_run;
    
	if (init_patch(ctx))
		return 1;

	pr_info("  Patch path    : %s\n", ctx->patchfile);
	pr_info("  Target BuildId: %s\n", PI(ctx)->target_bid);
	pr_info("  Patch BuildId : %s\n", PI(ctx)->patch_bid);
	pr_info("  Patch architecture type : %s\n", PI(ctx)->patch_arch_type);
	if(!strcmp(PI(ctx)->patch_arch_type, "EM_X86_64"))
	{
		ctx->arch_callback = &x86_64_cb;
	}
	if(!strcmp(PI(ctx)->patch_arch_type, "EM_386"))
	{
		ctx->arch_callback = &x86_cb;
	}
	return 0;
}

static int process_cease(struct process_ctx_s *ctx, const char *bid)
{
	int err, ret;

	err = process_suspend(ctx, bid);
	if (err)
		return err;

	ret = process_link(ctx);
	if (ret)
		goto resume;

	ret = process_collect_vmas(ctx);
	if (ret)
		goto resume;

	return 0;

resume:
	err = process_resume(ctx);
	return ret ? ret : err;
}

int patch_process(pid_t pid, const char *patchfile, int dry_run, int no_plugin)
{
	int ret, err;
	struct process_ctx_s *ctx = &process_context;

	err = init_context(ctx, pid, patchfile, dry_run);
	if (err)
		return err;

	ctx->check_backtrace = jumps_check_backtrace;

	err = process_cease(ctx, PI(ctx)->target_bid);
	if (err)
		return err;

	ret = process_find_patch(ctx);
	if (ret)
		goto resume;

	ret = process_find_target_dlm(ctx);
	if (ret)
		goto resume;

	if (!no_plugin) {
		ret = process_inject_service(ctx);
		if (ret)
			goto resume;
	}

	ret = process_collect_needed(ctx);
	if (ret)
		goto resume;

	ret = collect_relocations(ctx);
	if (ret)
		goto resume;

	ret = resolve_relocations(ctx);
	if (ret)
		goto resume;

	ret = apply_dyn_binpatch(ctx);
	if (ret)
		pr_err("failed to apply binary patch\n");

resume:
	err = process_resume(ctx);

	pr_info("Done\n");
	return ret ? ret : err;
}

int check_process(pid_t pid, const char *patchfile)
{
	int err;
	struct process_ctx_s *ctx = &process_context;

	err = init_context(ctx, pid, patchfile, 0);
	if (err)
		return err;

	err = process_collect_vmas(ctx);
	if (err)
		return err;

	return find_patch_by_bid(ctx, PI(ctx)->patch_bid) ? 0 : ENOENT;
}

static void list_patch(const struct patch_s *p)
{
	pr_msg("  %s (%s) - ", p->patch_dlm->path, p->pi.patch_bid);
	if (p->target_dlm)
		pr_msg("%s\n", p->target_dlm->path);
}

int list_process_patches(pid_t pid)
{
	int err;
	struct process_ctx_s *ctx = &process_context;
	struct patch_s *p;

	if (elf_library_status())
		return -1;

	ctx->pid = pid;

	err = process_collect_vmas(ctx);
	if (err)
		return err;

	if (list_empty(&ctx->applied_patches))
		return 0;

	list_for_each_entry(p, &ctx->applied_patches, list)
		list_patch(p);

	return 0;
}

static int patch_check_backtrace(const struct process_ctx_s *ctx,
				 const struct backtrace_s *bt,
				 uint64_t start, uint64_t end)
{
	return backtrace_check_range(bt, start, end);
}

static int revert_dyn_binpatch(struct process_ctx_s *ctx, struct patch_s *p)
{
	int err;

	if (p->target_dlm) {
		err = patch_revert_func_jumps(ctx, p);
		if (err)
			return err;
	}

	return patch_unload(ctx, p);
}

int unpatch_process(pid_t pid, const char *patchfile, int dry_run)
{
	int ret, err;
	struct process_ctx_s *ctx = &process_context;
	struct patch_s *p;

	err = init_context(ctx, pid, patchfile, dry_run);
	if (err)
		return err;

	ctx->check_backtrace = patch_check_backtrace;

	err = process_cease(ctx, PI(ctx)->patch_bid);
	if (err)
		return err;

	p = find_patch_by_bid(ctx, PI(ctx)->patch_bid);
	if (!p) {
		pr_err("failed to find target ELF with Build ID %s in process %d\n",
				p->pi.patch_bid, pid);
		pr_err("It was there. This is totally wrong. Aborting\n");
		ret = -EFAULT;
		goto resume;
	}

	ret = revert_dyn_binpatch(ctx, p);
	if (ret)
		pr_err("failed to revert patch\n");

resume:
	err = process_resume(ctx);

	pr_info("Done\n");
	return ret ? ret : err;
}
