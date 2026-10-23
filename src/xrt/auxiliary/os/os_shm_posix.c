// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  os_shm.h on Linux (desktop) and macOS: shm_open, mode 0600.
 * @ingroup aux_os
 */

#include "os/os_shm.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static bool
name_ok(const char *name)
{
	return name != NULL && name[0] == '/' && strlen(name) <= OS_SHM_NAME_MAX && strchr(name + 1, '/') == NULL;
}

static int
map_fd(int fd, const char *name, size_t size, bool writable, bool creator, struct os_shm *out)
{
	void *p = mmap(NULL, size, writable ? (PROT_READ | PROT_WRITE) : PROT_READ, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED) {
		return OS_SHM_ERR_FAILED;
	}
	out->map = p;
	out->size = size;
	out->native = (uint64_t)fd;
	out->creator = creator;
	snprintf(out->name, sizeof(out->name), "%s", name);
	return 0;
}

int
os_shm_create(const char *name, size_t size, struct os_shm *out)
{
	if (out == NULL) {
		return OS_SHM_ERR_INVALID;
	}
	memset(out, 0, sizeof(*out));
	if (!name_ok(name) || size == 0) {
		return OS_SHM_ERR_INVALID;
	}
	// A crashed creator's name: unlink it (its openers keep their mapping).
	(void)shm_unlink(name);
	const int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
	if (fd < 0) {
		return OS_SHM_ERR_FAILED;
	}
	(void)fcntl(fd, F_SETFD, FD_CLOEXEC);
	if (ftruncate(fd, (off_t)size) != 0) {
		close(fd);
		(void)shm_unlink(name);
		return OS_SHM_ERR_FAILED;
	}
	const int r = map_fd(fd, name, size, true, true, out);
	if (r != 0) {
		close(fd);
		(void)shm_unlink(name);
	}
	return r;
}

int
os_shm_open(const char *name, size_t size, bool writable, struct os_shm *out)
{
	if (out == NULL) {
		return OS_SHM_ERR_INVALID;
	}
	memset(out, 0, sizeof(*out));
	if (!name_ok(name) || size == 0) {
		return OS_SHM_ERR_INVALID;
	}
	const int fd = shm_open(name, writable ? O_RDWR : O_RDONLY, 0);
	if (fd < 0) {
		return errno == ENOENT ? OS_SHM_ERR_NOT_FOUND : OS_SHM_ERR_FAILED;
	}
	(void)fcntl(fd, F_SETFD, FD_CLOEXEC);
	struct stat sb;
	if (fstat(fd, &sb) != 0 || sb.st_size < 0 || (size_t)sb.st_size < size) {
		close(fd); // a short section would SIGBUS on access
		return OS_SHM_ERR_SHORT;
	}
	const int r = map_fd(fd, name, size, writable, false, out);
	if (r != 0) {
		close(fd);
	}
	return r;
}

void
os_shm_close(struct os_shm *shm)
{
	if (shm == NULL || shm->map == NULL) {
		return;
	}
	(void)munmap(shm->map, shm->size);
	close((int)shm->native);
	if (shm->creator) {
		(void)shm_unlink(shm->name);
	}
	memset(shm, 0, sizeof(*shm));
}
