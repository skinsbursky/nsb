#include <stdlib.h>
#include <errno.h>

#include "include/log.h"
#include "include/x86_64.h"

#define OP_CALLQ		0xe8
#define OP_JMPQ			0xe9
#define OP_JMP			0xeb
#define OP_MOV_RM_32		0x89
#define OP_MOV_MR_32		0x8b
#define OP_MAX			0x100

struct x86_op_info_s {
	unsigned char instr_size;
	unsigned char cmd_size;
} x86_ops[] = {
	[OP_CALLQ] = {
		.instr_size = 5,
		.cmd_size = 1,
	},
	[OP_JMPQ] = {
		.instr_size = 5,
		.cmd_size = 1,
	},
	[OP_JMP] = {
		.instr_size = 2,
		.cmd_size = 1,
	},
	[OP_MOV_RM_32] = {
		.instr_size = 6,
		.cmd_size = 2,
	},
	[OP_MOV_MR_32] = {
		.instr_size = 6,
		.cmd_size = 2,
	},
};

static struct x86_op_info_s *x86_get_op_info(unsigned char op)
{
	struct x86_op_info_s *info;

	if (op >= OP_MAX) {
		pr_err("%s: invalid opcode: %#x\n", __func__, op);
		return NULL;
	}
	info = &x86_ops[op];
	if (!info->instr_size) {
		pr_err("%s: unknown opcode: %#x\n", __func__, op);
		return NULL;
	}
	return info;
}

static int ip_gen_offset(unsigned long next_ip, unsigned long tgt_pos,
			 char addr_size, int *buf)
{
	int i;
	long offset;
	unsigned long mask = 0;

	for (i = 0; i < addr_size; i++) {
		mask |= ((unsigned long)0xff << (8 * i));
	}

	offset = tgt_pos - next_ip;
	if (abs(offset) & ~mask) {
		pr_err("%s: offset is beyond command size: %#lx > %#lx\n",
				__func__, offset, mask);
		return -EINVAL;
	}

	pr_debug("%s: addr_size : %d\n", __func__, addr_size);
	pr_debug("%s: next_ip : %#lx\n", __func__, next_ip);
	pr_debug("%s: tgt_pos : %#lx\n", __func__, tgt_pos);
	pr_debug("%s: offset  : %#x\n", __func__, (int)offset);

	*buf = offset;

	return 0;
}

static int ip_change_relative(unsigned char *addr,
			      unsigned long next_ip, unsigned long tgt_pos,
			      size_t addr_size)
{
	int offset;
	int i;

	if (ip_gen_offset(next_ip, tgt_pos, addr_size, &offset))
		return -1;

	memcpy(addr, (void *)&offset, addr_size);

	pr_debug("%s: offset  :", __func__);
	for (i = 0; i < addr_size; i++)
		pr_msg(" %02x", addr[i]);
	pr_debug("\n");

	return 0;
}

int x86_modify_instruction(unsigned char *buf,
			   unsigned long cur_pos, unsigned long tgt_pos)
{
	unsigned char *addr;
	size_t addr_size;
	struct x86_op_info_s *info;

	info = x86_get_op_info(buf[0]);
	if (!info)
		return -EINVAL;

	addr_size = info->instr_size - info->cmd_size;
	addr = buf + info->cmd_size;
	if (ip_change_relative(addr, cur_pos + info->instr_size, tgt_pos, addr_size))
		return -1;
	return info->instr_size;
}

int x86_create_instruction(unsigned char *buf, unsigned char op,
			   unsigned long cur_pos, unsigned long tgt_pos)
{
	*buf = op;
	return x86_modify_instruction(buf, cur_pos, tgt_pos);
}
