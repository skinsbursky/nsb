#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <linux/limits.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <linux/un.h>

#include "include/util.h"
#include "include/process.h"
#include "include/service.h"
#include "include/log.h"
#include "include/vma.h"
#include "include/elf.h"
#include "include/context.h"
#include "include/x86_64.h"

#include <plugins/service.h>

static int nsb_service_send_request(const struct service *service,
				    const struct nsb_service_request *rq,
				    size_t rqlen)
{
	ssize_t size;

	size = send(service->sock, rq, rqlen, 0);
	if (size < 0) {
		pr_perror("nsb_service_request: send to process %d failed",
				service->pid);
		return -errno;
	}
	return 0;
}

static ssize_t nsb_service_receive_response(const struct service *service,
					    struct nsb_service_response *rp)
{
	ssize_t size;

	size = recv(service->sock, rp, sizeof(*rp), 0);
	if (size < 0) {
		pr_perror("receive from process %d failed", service->pid);
		return -errno;
	}
	if (size < sizeof(rp->ret)) {
		pr_err("message is truncated: %ld < %ld\n", size, sizeof(rp->ret));
		return -EINVAL;
	}
	return size;
}

static int check_map_file(const char *dentry, void *data)
{
	const char *base = data;

	return !strncmp(dentry, base, strlen(base));
}

static int service_collect_vmas(struct process_ctx_s *ctx, struct service *service)
{
	int err;
	uint64_t base;
	char buf[] = "/proc/XXXXXXXXXX/map_files";
	char path[PATH_MAX];
	char dentry[256];
	ssize_t res;

	err = process_read_data(ctx, service->handle, &base, sizeof(base));
	if (err)
		return err;

	sprintf(buf, "/proc/%d/map_files/", service->pid);
	sprintf(dentry, "%lx-", base);

	err = find_dentry(buf, check_map_file, dentry, dentry);
	if (err) {
		pr_err("failed to find dentry, starting with \"%s\" "
			"in %d map files\n", dentry, service->pid);
		return err;
	}

	strcat(buf, dentry);

	res = readlink(buf, path, sizeof(path) - 1);
	if (res == -1) {
		pr_perror("failed to read link %s", buf);
		return -errno;
	}
	if (res > (sizeof(path) - 1)) {
		pr_err("link size if too big: %ld", res);
		return -errno;
	}
	path[res] = '\0';

	err = collect_vmas_by_path(service->pid, &service->vmas, path);
	if (err)
		return err;

	if (list_empty(&service->vmas)) {
		pr_err("failed to collect service VMAs by path %s\n", path);
		return -ENOENT;
	}
	return 0;
}

static int service_disconnect(struct process_ctx_s *ctx, struct service *service)
{
	if (service->sock >= 0) {
		if (close(service->sock)) {
			pr_perror("failed ot close service socket %d",
					service->sock);
			return -errno;
		}
		pr_debug("  Disconnected from service socket\n");
		service->sock = -1;
	}
	return 0;
}

static int service_local_connect(struct service *service)
{
	int sock, err;
	struct sockaddr_un addr;

	memset(&addr, 0, sizeof(addr.sun_path));
	addr.sun_family = AF_UNIX;
	if (snprintf(&addr.sun_path[1], UNIX_PATH_MAX - 1,
				"NSB-SERVICE-%d", service->pid) > UNIX_PATH_MAX - 1) {
		printf("Not enough space for socket path\n");
		return ENOMEM;
	}

	sock = socket(AF_UNIX, SOCK_SEQPACKET, 0);
	if (sock == -1) {
		err = errno;
		printf("failed to create packet socket: %s\n",
				strerror(errno));
		return err;
	}

	if (connect(sock, (struct sockaddr *)&addr, sizeof(addr))) {
		err = -errno;
		pr_perror("failed to connect to service socket \"%s\"", &addr.sun_path[1]);
		close(sock);
		return err;
	}
	pr_debug("  Connected to service socket \"%s\"\n", &addr.sun_path[1]);
	service->sock = sock;
	return 0;
}

static int64_t service_sym_addr(struct service *service, const char *symbol)
{
	const struct vma_area *vma;
	int64_t value;


	vma = first_vma(&service->vmas);
	if (!vma && !vma->ei) {
		pr_err("WTF?!!\n");
		return -EINVAL;
	}

	value = elf_dyn_sym_value(vma->ei, symbol);
	if (value <= 0) {
		pr_err("failed to find symbol \"%s\" in %s\n", symbol, vma->path);
		return value;
	}

	return vma_start(vma) + value;
}

static int service_remote_accept(struct process_ctx_s *ctx, struct service *service)
{
	const char *symbol = "nsb_service_accept";
	int64_t address;
	uint64_t code_addr = ctx->remote_map;
	ssize_t size;
	void *code;

	address = service_sym_addr(service, symbol);
	if (address <= 0)
		return address;

	size = x86_64_call(address, code_addr, 0, 0, 0, 0, 0, 0, &code);
	if (size < 0) {
		pr_err("failed to construct %s call\n", symbol);
		return size;
	}

	return process_exec_code(ctx, code_addr, code, size);
}

static int service_run(struct process_ctx_s *ctx, struct service *service,
		       bool once)
{
	uint64_t code_addr = ctx->remote_map;
	ssize_t size;
	void *code;

	size = x86_64_call(service->runner, code_addr,
			   once, !once, 0, 0, 0, 0,
			   &code);
	if (size < 0) {
		pr_err("failed to construct runner call\n");
		return size;
	}

	if (once)
		return process_exec_code(ctx, code_addr, code, size);
	else
		return process_release_at(ctx, code_addr, code, size);
}

static int service_provide_sigframe(struct process_ctx_s *ctx, struct service *service)
{
	int err;
	struct nsb_service_request rq = {
		.cmd = NSB_SERVICE_CMD_EMERG_SIGFRAME,
	};
	struct nsb_service_response rs;
	ssize_t size;
	size_t rqlen;
	int64_t address;

	address = service_sym_addr(service, "nsb_service_run_loop");
	if (address <= 0)
		return address;
	service->runner = address;

	address = service_sym_addr(service, "emergency_sigframe");
	if (address <= 0)
		return address;

	size = process_emergency_sigframe(ctx, rq.data, (void *)address);
	if (size < 0)
		return size;

	rqlen = sizeof(rq.cmd) + size;

	err = nsb_service_send_request(service, &rq, rqlen);
	if (err)
		return err;

	err = service_run(ctx, service, true);
	if (err)
		return err;

	size = nsb_service_receive_response(service, &rs);
	if (size < 0)
		return size;

	return err;
}

static int service_release(struct process_ctx_s *ctx, struct service *service)
{
	int err;

	if (service->released)
		return 0;

	err = service_run(ctx, service, false);
	if (err)
		return err;

	pr_debug("  Service released\n");
	service->released = true;
	return 0;
}

static int service_interrupt(struct process_ctx_s *ctx, struct service *service)
{
	int err;
	const struct nsb_service_request rq = {
		.cmd = NSB_SERVICE_CMD_STOP,
		.data = { },
	};
	struct nsb_service_response rs;
	ssize_t size;
	size_t rqlen = sizeof(rq.cmd) + strlen(rq.data) + 1;

	if (!service->released)
		return 0;

	err = nsb_service_send_request(service, &rq, rqlen);
	if (err)
		return err;

	size = nsb_service_receive_response(service, &rs);
	if (size < 0)
		return size;

	err = process_acquire(ctx);
	if (err)
		return err;

	pr_debug("  Service caught\n");
	service->released = false;
	return 0;
}

static int service_connect(struct process_ctx_s *ctx, struct service *service)
{
	int err;

	err = service_local_connect(service);
	if (err)
		return err;

	err = service_remote_accept(ctx, service);
	if (err)
		return err;

	err = service_provide_sigframe(ctx, service);
	if (err)
		return err;

	err = service_release(ctx, service);
	if (err)
		return err;

	return 0;
}

int service_stop(struct process_ctx_s *ctx, struct service *service)
{
	int err;

	err = service_interrupt(ctx, service);
	if (err)
		return err;

	return service_disconnect(ctx, service);
}

int service_start(struct process_ctx_s *ctx, struct service *service)
{
	int err;

	err = service_collect_vmas(ctx, service);
	if (err)
		return err;

	err = service_connect(ctx, service);
	if (err)
		return err;

	return 0;
}

int service_read(const struct service *service,
		 void *dest, uint64_t rsrc, size_t n)
{
	struct nsb_service_request rq = {
		.cmd = NSB_SERVICE_CMD_READ,
	};
	struct nsb_service_response rs;
	struct nsb_service_data_rw *rw;
	size_t rqlen = sizeof(rq.cmd) + sizeof(*rw);
	ssize_t size;
	int err;

	if (n > NSB_SERVICE_RW_DATA_SIZE_MAX) {
		pr_err("requested too much: %ld > %ld\n",
				n, NSB_SERVICE_RW_DATA_SIZE_MAX);
		return -E2BIG;
	}

	rw = (void *)rq.data;
	rw->address = (void *)rsrc;
	rw->size = n;

	err = nsb_service_send_request(service, &rq, rqlen);
	if (err)
		return err;

	size = nsb_service_receive_response(service, &rs);
	if (size < 0)
		return size;

	if (rs.ret < 0) {
		errno = -rs.ret;
		pr_perror("read request failed");
		return rs.ret;
	}
	if (size - sizeof(int) != n) {
		pr_err("received differs from requested: %ld != %ld\n",
				size - sizeof(rs.ret), n);
		return -EFAULT;
	}

	memcpy(dest, rs.data, n);
	return 0;
}

int service_write(const struct service *service,
		  const void *src, uint64_t rdest, size_t n)
{
	struct nsb_service_request rq = {
		.cmd = NSB_SERVICE_CMD_WRITE,
	};
	struct nsb_service_response rs;
	struct nsb_service_data_rw *rw;
	size_t rqlen = sizeof(rq.cmd) + sizeof(*rw);
	ssize_t size;
	int err;

	if (n > NSB_SERVICE_RW_DATA_SIZE_MAX) {
		pr_err("requested too much: %ld > %ld\n",
				n, NSB_SERVICE_RW_DATA_SIZE_MAX);
		return -E2BIG;
	}

	rw = (void *)rq.data;
	rw->address = (void *)rdest;
	rw->size = n;

	memcpy(rs.data, src, n);

	err = nsb_service_send_request(service, &rq, rqlen);
	if (err)
		return err;

	size = nsb_service_receive_response(service, &rs);
	if (size < 0)
		return size;

	if (rs.ret < 0) {
		errno = -rs.ret;
		pr_perror("write request failed");
		return rs.ret;
	}
	return 0;
}
