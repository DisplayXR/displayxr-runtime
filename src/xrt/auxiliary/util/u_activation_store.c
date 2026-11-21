// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Per-user pending-activation store (see u_activation_store.h).
 * @ingroup aux_util
 */

#include "u_activation_store.h"

#include "util/u_sha256.h"
#include "xrt/xrt_config_os.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef XRT_OS_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#include <sddl.h>
#else
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#define PATH_CAP 1024
#define RECORD_CAP 512
#define NAME_HEX 32 //!< hex characters of a record's file name (16 bytes of SHA-256)
#define LOG_CAP_BYTES (64 * 1024)
#define FUTURE_SLACK_S 120 //!< a record "created" further than this in the future is refused
#define FAILURES_KEPT 16

static const char k_pending_ext[] = ".pending";
static const char k_claim_tag[] = ".claim.";


/*
 *
 * Platform layer: a handful of file operations on UTF-8 paths.
 *
 */

#ifdef XRT_OS_WINDOWS

static wchar_t *
wide(const char *utf8)
{
	const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, NULL, 0);
	if (n <= 0) {
		return NULL;
	}
	wchar_t *w = (wchar_t *)calloc((size_t)n, sizeof(wchar_t));
	if (w != NULL && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, w, n) <= 0) {
		free(w);
		w = NULL;
	}
	return w;
}

static bool
fs_mkdir_one(const char *path)
{
	wchar_t *w = wide(path);
	if (w == NULL) {
		return false;
	}
	const bool ok = CreateDirectoryW(w, NULL) != 0 || GetLastError() == ERROR_ALREADY_EXISTS;
	free(w);
	return ok;
}

//! Is @p dir a real directory (not a reparse point someone planted)?
static bool
fs_dir_ok(const char *dir)
{
	wchar_t *w = wide(dir);
	if (w == NULL) {
		return false;
	}
	const DWORD a = GetFileAttributesW(w);
	free(w);
	return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
	       (a & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

static bool
fs_write_new(const char *path, const char *data, size_t n)
{
	wchar_t *w = wide(path);
	if (w == NULL) {
		return false;
	}
	HANDLE h = CreateFileW(w, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
	free(w);
	if (h == INVALID_HANDLE_VALUE) {
		return false;
	}
	DWORD wrote = 0;
	const bool ok = WriteFile(h, data, (DWORD)n, &wrote, NULL) != 0 && wrote == (DWORD)n;
	CloseHandle(h);
	return ok;
}

static int
fs_read(const char *path, char *buf, size_t cap)
{
	wchar_t *w = wide(path);
	if (w == NULL) {
		return -1;
	}
	HANDLE h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
	                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
	free(w);
	if (h == INVALID_HANDLE_VALUE) {
		return -1;
	}
	BY_HANDLE_FILE_INFORMATION info;
	if (GetFileInformationByHandle(h, &info) == 0 ||
	    (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0 ||
	    info.nFileSizeHigh != 0 || info.nFileSizeLow >= cap) {
		CloseHandle(h);
		return -1;
	}
	DWORD got = 0;
	const bool ok = ReadFile(h, buf, (DWORD)(cap - 1), &got, NULL) != 0;
	CloseHandle(h);
	if (!ok) {
		return -1;
	}
	buf[got] = '\0';
	return (int)got;
}

static bool
fs_rename(const char *from, const char *to, bool replace)
{
	wchar_t *wf = wide(from);
	wchar_t *wt = wide(to);
	const bool ok = wf != NULL && wt != NULL && MoveFileExW(wf, wt, replace ? MOVEFILE_REPLACE_EXISTING : 0) != 0;
	free(wf);
	free(wt);
	return ok;
}

static void
fs_unlink(const char *path)
{
	wchar_t *w = wide(path);
	if (w != NULL) {
		(void)DeleteFileW(w);
		free(w);
	}
}

static void
fs_append_capped(const char *path, const char *line, size_t n)
{
	wchar_t *w = wide(path);
	if (w == NULL) {
		return;
	}
	HANDLE h = CreateFileW(w, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS,
	                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
	free(w);
	if (h == INVALID_HANDLE_VALUE) {
		return;
	}
	LARGE_INTEGER size;
	LARGE_INTEGER zero;
	zero.QuadPart = 0;
	if (GetFileSizeEx(h, &size) != 0 && size.QuadPart > LOG_CAP_BYTES) {
		(void)SetFilePointerEx(h, zero, NULL, FILE_BEGIN);
		(void)SetEndOfFile(h);
	}
	(void)SetFilePointerEx(h, zero, NULL, FILE_END);
	DWORD wrote = 0;
	(void)WriteFile(h, line, (DWORD)n, &wrote, NULL);
	CloseHandle(h);
}

typedef void (*fs_list_fn)(const char *name, void *ud);

static bool
fs_list(const char *dir, fs_list_fn fn, void *ud)
{
	char pattern[PATH_CAP];
	if (snprintf(pattern, sizeof(pattern), "%s\\*", dir) >= (int)sizeof(pattern)) {
		return false;
	}
	wchar_t *w = wide(pattern);
	if (w == NULL) {
		return false;
	}
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW(w, &fd);
	free(w);
	if (h == INVALID_HANDLE_VALUE) {
		return GetLastError() == ERROR_FILE_NOT_FOUND;
	}
	do {
		if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
			continue;
		}
		char name[256];
		if (WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, (int)sizeof(name), NULL, NULL) > 0) {
			fn(name, ud);
		}
	} while (FindNextFileW(h, &fd) != 0);
	FindClose(h);
	return true;
}

static bool
user_identity(char *out, size_t cap)
{
	bool ok = false;
	HANDLE tok = NULL;
	if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok) == 0) {
		return false;
	}
	BYTE buf[256];
	DWORD len = 0;
	if (GetTokenInformation(tok, TokenUser, buf, sizeof(buf), &len) != 0) {
		LPSTR sid = NULL;
		if (ConvertSidToStringSidA(((TOKEN_USER *)buf)->User.Sid, &sid) != 0 && sid != NULL) {
			ok = snprintf(out, cap, "%s", sid) < (int)cap;
			LocalFree(sid);
		}
	}
	CloseHandle(tok);
	return ok;
}

static bool
random_bytes(uint8_t *out, size_t n)
{
	return BCryptGenRandom(NULL, out, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}

static unsigned long
process_id(void)
{
	return (unsigned long)GetCurrentProcessId();
}

#define DIR_SEP '\\'

#else // POSIX

static bool
fs_mkdir_one(const char *path)
{
	return mkdir(path, S_IRWXU) == 0 || errno == EEXIST;
}

//! A real directory, owned by the caller, closed to group and others (tightened when it is ours).
static bool
fs_dir_ok(const char *dir)
{
	struct stat st;
	if (lstat(dir, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != geteuid()) {
		return false;
	}
	if ((st.st_mode & (S_IRWXG | S_IRWXO)) != 0 && chmod(dir, S_IRWXU) != 0) {
		return false;
	}
	return true;
}

static bool
fs_write_new(const char *path, const char *data, size_t n)
{
	const int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, S_IRUSR | S_IWUSR);
	if (fd < 0) {
		return false;
	}
	const ssize_t w = write(fd, data, n);
	const bool ok = w == (ssize_t)n;
	close(fd);
	return ok;
}

static int
fs_read(const char *path, char *buf, size_t cap)
{
	const int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0) {
		return -1;
	}
	struct stat st;
	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
	    (st.st_mode & (S_IRWXG | S_IRWXO)) != 0 || st.st_size < 0 || (size_t)st.st_size >= cap) {
		close(fd);
		return -1;
	}
	const ssize_t got = read(fd, buf, cap - 1);
	close(fd);
	if (got < 0) {
		return -1;
	}
	buf[got] = '\0';
	return (int)got;
}

static bool
fs_rename(const char *from, const char *to, bool replace)
{
	(void)replace; // rename(2) replaces; the claim name is unique, so "no replace" holds by construction
	return rename(from, to) == 0;
}

static void
fs_unlink(const char *path)
{
	(void)unlink(path);
}

static void
fs_append_capped(const char *path, const char *line, size_t n)
{
	const int fd = open(path, O_WRONLY | O_APPEND | O_CREAT | O_NOFOLLOW | O_CLOEXEC, S_IRUSR | S_IWUSR);
	if (fd < 0) {
		return;
	}
	struct stat st;
	if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == geteuid()) {
		if (st.st_size > LOG_CAP_BYTES) {
			(void)ftruncate(fd, 0);
		}
		const ssize_t w = write(fd, line, n);
		(void)w;
	}
	close(fd);
}

typedef void (*fs_list_fn)(const char *name, void *ud);

static bool
fs_list(const char *dir, fs_list_fn fn, void *ud)
{
	DIR *d = opendir(dir);
	if (d == NULL) {
		return false;
	}
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		if (e->d_name[0] == '.') {
			continue; // ".", "..": record names never start with a dot
		}
		fn(e->d_name, ud);
	}
	closedir(d);
	return true;
}

static bool
user_identity(char *out, size_t cap)
{
	return snprintf(out, cap, "uid:%lu", (unsigned long)geteuid()) < (int)cap;
}

static bool
random_bytes(uint8_t *out, size_t n)
{
	const int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		return false;
	}
	size_t got = 0;
	while (got < n) {
		const ssize_t r = read(fd, out + got, n - got);
		if (r <= 0) {
			break;
		}
		got += (size_t)r;
	}
	close(fd);
	return got == n;
}

static unsigned long
process_id(void)
{
	return (unsigned long)getpid();
}

#define DIR_SEP '/'

#endif

//! Create @p dir and its missing parents (each owner-only where we create it).
static void
fs_mkdir_p(const char *dir)
{
	char tmp[PATH_CAP];
	const size_t n = strlen(dir);
	if (n == 0 || n >= sizeof(tmp)) {
		return;
	}
	memcpy(tmp, dir, n + 1);
	for (size_t i = 1; i < n; i++) {
		if (tmp[i] == '/' || tmp[i] == DIR_SEP) {
			const char c = tmp[i];
			tmp[i] = '\0';
			(void)fs_mkdir_one(tmp);
			tmp[i] = c;
		}
	}
	(void)fs_mkdir_one(tmp);
}

static bool
join(char *out, size_t cap, const char *dir, const char *name)
{
	return snprintf(out, cap, "%s%c%s", dir, DIR_SEP, name) < (int)cap;
}


/*
 *
 * Names, records.
 *
 */

static void
hex(const uint8_t *in, size_t n, char *out)
{
	static const char d[] = "0123456789abcdef";
	for (size_t i = 0; i < n; i++) {
		out[2 * i] = d[in[i] >> 4];
		out[2 * i + 1] = d[in[i] & 15];
	}
	out[2 * n] = '\0';
}

//! The file-name stem of @p nonce: 32 hex characters of a domain-separated SHA-256.
static void
nonce_name(const char *nonce, char out[NAME_HEX + 1])
{
	static const char domain[] = "displayxr-activation-nonce-v1:";
	struct u_sha256 s;
	uint8_t digest[32];
	u_sha256_init(&s);
	u_sha256_update(&s, domain, sizeof(domain) - 1);
	u_sha256_update(&s, nonce, strlen(nonce));
	u_sha256_final(&s, digest);
	hex(digest, NAME_HEX / 2, out);
}

void
u_activation_nonce_id(const char *nonce, char out[U_ACTIVATION_NONCE_ID_CHARS + 1])
{
	out[0] = '\0';
	if (!u_activation_nonce_valid(nonce)) {
		return;
	}
	char name[NAME_HEX + 1];
	nonce_name(nonce, name);
	memcpy(out, name, U_ACTIVATION_NONCE_ID_CHARS);
	out[U_ACTIVATION_NONCE_ID_CHARS] = '\0';
}

static void
base64url(const uint8_t *in, size_t n, char *out)
{
	static const char a[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
	size_t o = 0;
	for (size_t i = 0; i < n; i += 3) {
		const uint32_t b0 = in[i];
		const uint32_t b1 = i + 1 < n ? in[i + 1] : 0;
		const uint32_t b2 = i + 2 < n ? in[i + 2] : 0;
		const uint32_t v = (b0 << 16) | (b1 << 8) | b2;
		out[o++] = a[(v >> 18) & 63];
		out[o++] = a[(v >> 12) & 63];
		if (i + 1 < n) {
			out[o++] = a[(v >> 6) & 63];
		}
		if (i + 2 < n) {
			out[o++] = a[v & 63];
		}
	}
	out[o] = '\0';
}

//! The value of `key=` in the record text @p txt into @p out; false when absent, empty or too long.
static bool
record_field(const char *txt, const char *key, char *out, size_t cap)
{
	const size_t kn = strlen(key);
	const char *p = txt;
	while (*p != '\0') {
		const char *eol = strchr(p, '\n');
		const size_t ln = eol != NULL ? (size_t)(eol - p) : strlen(p);
		if (ln > kn && memcmp(p, key, kn) == 0 && p[kn] == '=') {
			const size_t vn = ln - kn - 1;
			if (vn == 0 || vn >= cap) {
				return false;
			}
			memcpy(out, p + kn + 1, vn);
			out[vn] = '\0';
			return true;
		}
		if (eol == NULL) {
			break;
		}
		p = eol + 1;
	}
	return false;
}

static bool
parse_i64(const char *s, int64_t *out)
{
	if (s[0] == '\0') {
		return false;
	}
	int64_t v = 0;
	for (const char *p = s; *p != '\0'; p++) {
		if (*p < '0' || *p > '9' || v > (INT64_MAX - 9) / 10) {
			return false;
		}
		v = v * 10 + (*p - '0');
	}
	*out = v;
	return true;
}

//! Record text -> @p out + the user it was begun by. False for anything this module did not write.
static bool
record_parse(const char *txt, struct u_activation_record *out, char *user, size_t user_cap)
{
	char v[8];
	char num[24];
	memset(out, 0, sizeof(*out));
	if (!record_field(txt, "v", v, sizeof(v)) || strcmp(v, "1") != 0) {
		return false;
	}
	if (!record_field(txt, "class", out->cls, sizeof(out->cls)) || !u_activation_class_valid(out->cls) ||
	    !record_field(txt, "id", out->id, sizeof(out->id)) || !u_activation_id_valid(out->id) ||
	    !record_field(txt, "user", user, user_cap)) {
		return false;
	}
	if (!record_field(txt, "created", num, sizeof(num)) || !parse_i64(num, &out->created_unix_s) ||
	    !record_field(txt, "expires", num, sizeof(num)) || !parse_i64(num, &out->expires_unix_s)) {
		return false;
	}
	return out->expires_unix_s >= out->created_unix_s;
}

static bool
record_live(const struct u_activation_record *r, int64_t now)
{
	return now <= r->expires_unix_s && r->created_unix_s <= now + FUTURE_SLACK_S;
}

static bool
ends_with(const char *s, const char *suffix)
{
	const size_t n = strlen(s);
	const size_t m = strlen(suffix);
	return n >= m && strcmp(s + n - m, suffix) == 0;
}


/*
 *
 * API.
 *
 */

const char *
u_activation_store_result_str(enum u_activation_store_result r)
{
	switch (r) {
	case U_ACTIVATION_STORE_OK: return "ok";
	case U_ACTIVATION_STORE_NOT_FOUND: return "not-found";
	case U_ACTIVATION_STORE_EXPIRED: return "expired";
	case U_ACTIVATION_STORE_WRONG_USER: return "wrong-user";
	case U_ACTIVATION_STORE_CORRUPT: return "corrupt";
	case U_ACTIVATION_STORE_TOO_MANY: return "too-many";
	case U_ACTIVATION_STORE_BAD_ARGUMENT: return "bad-argument";
	default: return "io-error";
	}
}

int64_t
u_activation_now_unix_s(void)
{
	return (int64_t)time(NULL);
}

bool
u_activation_store_default_dir(char *out, size_t cap)
{
	if (out == NULL || cap == 0) {
		return false;
	}
	out[0] = '\0';
#if defined(XRT_OS_WINDOWS)
	const wchar_t *base = _wgetenv(L"LOCALAPPDATA");
	if (base == NULL || base[0] == L'\0') {
		return false;
	}
	char b[PATH_CAP];
	if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, base, -1, b, (int)sizeof(b), NULL, NULL) <= 0) {
		return false;
	}
	return snprintf(out, cap, "%s\\DisplayXR\\Activation", b) < (int)cap;
#else
	const char *home = getenv("HOME");
#if defined(__APPLE__)
	if (home == NULL || home[0] != '/') {
		return false;
	}
	return snprintf(out, cap, "%s/Library/Application Support/DisplayXR/activation", home) < (int)cap;
#else
	const char *state = getenv("XDG_STATE_HOME");
	if (state != NULL && state[0] == '/') {
		return snprintf(out, cap, "%s/displayxr/activation", state) < (int)cap;
	}
	if (home == NULL || home[0] != '/') {
		return false;
	}
	return snprintf(out, cap, "%s/.local/state/displayxr/activation", home) < (int)cap;
#endif
#endif
}

struct prune_ctx
{
	const char *dir;
	int64_t now;
	int live;
};

static void
prune_one(const char *name, void *ud)
{
	struct prune_ctx *c = (struct prune_ctx *)ud;
	char path[PATH_CAP];
	if (!join(path, sizeof(path), c->dir, name)) {
		return;
	}
	if (strstr(name, k_claim_tag) != NULL) {
		fs_unlink(path); // a handler died between claiming and deleting
		return;
	}
	if (!ends_with(name, k_pending_ext)) {
		return; // the log, the failure counter
	}
	char txt[RECORD_CAP];
	char user[128];
	struct u_activation_record r;
	if (fs_read(path, txt, sizeof(txt)) < 0 || !record_parse(txt, &r, user, sizeof(user)) ||
	    !record_live(&r, c->now)) {
		fs_unlink(path);
		return;
	}
	c->live++;
}

int
u_activation_store_prune(const char *dir, int64_t now_unix_s)
{
	if (dir == NULL || !fs_dir_ok(dir)) {
		return -1;
	}
	struct prune_ctx c = {dir, now_unix_s, 0};
	if (!fs_list(dir, prune_one, &c)) {
		return -1;
	}
	return c.live;
}

enum u_activation_store_result
u_activation_store_begin(const char *dir,
                         const char *cls,
                         const char *id,
                         int64_t now_unix_s,
                         int64_t ttl_s,
                         char out_nonce[U_ACTIVATION_NONCE_CHARS + 1],
                         struct u_activation_record *out)
{
	if (out_nonce != NULL) {
		out_nonce[0] = '\0';
	}
	if (out != NULL) {
		memset(out, 0, sizeof(*out));
	}
	if (dir == NULL || dir[0] == '\0' || out_nonce == NULL || !u_activation_class_valid(cls) ||
	    !u_activation_id_valid(id) || ttl_s <= 0 || ttl_s > 24 * 3600 || now_unix_s <= 0) {
		return U_ACTIVATION_STORE_BAD_ARGUMENT;
	}
	fs_mkdir_p(dir);
	const int live = u_activation_store_prune(dir, now_unix_s);
	if (live < 0) {
		return U_ACTIVATION_STORE_IO_ERROR;
	}
	if (live >= U_ACTIVATION_MAX_PENDING) {
		return U_ACTIVATION_STORE_TOO_MANY;
	}
	char user[128];
	uint8_t raw[32];
	if (!user_identity(user, sizeof(user)) || !random_bytes(raw, sizeof(raw))) {
		return U_ACTIVATION_STORE_IO_ERROR;
	}
	char nonce[U_ACTIVATION_NONCE_CHARS + 1];
	base64url(raw, sizeof(raw), nonce);
	memset(raw, 0, sizeof(raw));

	char name[NAME_HEX + 1];
	nonce_name(nonce, name);
	char file[NAME_HEX + sizeof(k_pending_ext)];
	(void)snprintf(file, sizeof(file), "%s%s", name, k_pending_ext);
	char path[PATH_CAP];
	char txt[RECORD_CAP];
	const int n = snprintf(txt, sizeof(txt), "v=1\nclass=%s\nid=%s\nuser=%s\ncreated=%lld\nexpires=%lld\n", cls, id,
	                       user, (long long)now_unix_s, (long long)(now_unix_s + ttl_s));
	if (!join(path, sizeof(path), dir, file) || n <= 0 || n >= (int)sizeof(txt) ||
	    !fs_write_new(path, txt, (size_t)n)) {
		memset(nonce, 0, sizeof(nonce));
		return U_ACTIVATION_STORE_IO_ERROR;
	}
	memcpy(out_nonce, nonce, sizeof(nonce));
	memset(nonce, 0, sizeof(nonce));
	if (out != NULL) {
		(void)snprintf(out->cls, sizeof(out->cls), "%s", cls);
		(void)snprintf(out->id, sizeof(out->id), "%s", id);
		memcpy(out->nonce_id, name, U_ACTIVATION_NONCE_ID_CHARS);
		out->nonce_id[U_ACTIVATION_NONCE_ID_CHARS] = '\0';
		out->created_unix_s = now_unix_s;
		out->expires_unix_s = now_unix_s + ttl_s;
	}
	return U_ACTIVATION_STORE_OK;
}

enum u_activation_store_result
u_activation_store_consume(const char *dir, const char *nonce, int64_t now_unix_s, struct u_activation_record *out)
{
	static unsigned long s_claims = 0;
	if (out != NULL) {
		memset(out, 0, sizeof(*out));
	}
	if (dir == NULL || dir[0] == '\0' || !u_activation_nonce_valid(nonce)) {
		return U_ACTIVATION_STORE_BAD_ARGUMENT;
	}
	if (!fs_dir_ok(dir)) {
		return U_ACTIVATION_STORE_NOT_FOUND; // nothing was ever begun here
	}
	char name[NAME_HEX + 1];
	nonce_name(nonce, name);
	char file[NAME_HEX + 64];
	char pending[PATH_CAP];
	char claim[PATH_CAP];
	(void)snprintf(file, sizeof(file), "%s%s", name, k_pending_ext);
	if (!join(pending, sizeof(pending), dir, file)) {
		return U_ACTIVATION_STORE_BAD_ARGUMENT;
	}
	(void)snprintf(file, sizeof(file), "%s%s%lu.%lu", name, k_claim_tag, process_id(), ++s_claims);
	if (!join(claim, sizeof(claim), dir, file)) {
		return U_ACTIVATION_STORE_BAD_ARGUMENT;
	}
	// The claim: of two handlers racing on one nonce, exactly one rename
	// succeeds; the other sees no pending record.
	if (!fs_rename(pending, claim, false)) {
		return U_ACTIVATION_STORE_NOT_FOUND;
	}
	char txt[RECORD_CAP];
	const int got = fs_read(claim, txt, sizeof(txt));
	fs_unlink(claim); // spent, whatever follows

	struct u_activation_record r;
	char rec_user[128];
	char user[128];
	if (got < 0 || !record_parse(txt, &r, rec_user, sizeof(rec_user))) {
		return U_ACTIVATION_STORE_CORRUPT;
	}
	if (!user_identity(user, sizeof(user)) || strcmp(user, rec_user) != 0) {
		return U_ACTIVATION_STORE_WRONG_USER;
	}
	if (!record_live(&r, now_unix_s)) {
		return U_ACTIVATION_STORE_EXPIRED;
	}
	memcpy(r.nonce_id, name, U_ACTIVATION_NONCE_ID_CHARS);
	r.nonce_id[U_ACTIVATION_NONCE_ID_CHARS] = '\0';
	if (out != NULL) {
		*out = r;
	}
	return U_ACTIVATION_STORE_OK;
}

//! Read the failure times still inside the window into @p out (up to @p max); returns the count.
static int
failures_read(const char *dir, int64_t now, int64_t *out, int max)
{
	char path[PATH_CAP];
	char txt[RECORD_CAP];
	if (!join(path, sizeof(path), dir, "failures") || fs_read(path, txt, sizeof(txt)) < 0) {
		return 0;
	}
	int n = 0;
	const char *p = txt;
	while (*p != '\0' && n < max) {
		const char *eol = strchr(p, '\n');
		const size_t ln = eol != NULL ? (size_t)(eol - p) : strlen(p);
		char num[24];
		int64_t t = 0;
		if (ln > 0 && ln < sizeof(num)) {
			memcpy(num, p, ln);
			num[ln] = '\0';
			// A time in the future (a clock set back) counts: it cannot be used to clear the counter.
			if (parse_i64(num, &t) && t > now - U_ACTIVATION_FAIL_WINDOW_S) {
				out[n++] = t;
			}
		}
		if (eol == NULL) {
			break;
		}
		p = eol + 1;
	}
	return n;
}

void
u_activation_store_note_failure(const char *dir, int64_t now_unix_s)
{
	if (dir == NULL || dir[0] == '\0') {
		return;
	}
	fs_mkdir_p(dir);
	if (!fs_dir_ok(dir)) {
		return;
	}
	int64_t t[FAILURES_KEPT];
	int n = failures_read(dir, now_unix_s, t, FAILURES_KEPT);
	if (n == FAILURES_KEPT) {
		memmove(t, t + 1, sizeof(t[0]) * (FAILURES_KEPT - 1));
		n--;
	}
	t[n++] = now_unix_s;
	char txt[RECORD_CAP];
	size_t o = 0;
	for (int i = 0; i < n; i++) {
		const int w = snprintf(txt + o, sizeof(txt) - o, "%lld\n", (long long)t[i]);
		if (w <= 0 || (size_t)w >= sizeof(txt) - o) {
			break;
		}
		o += (size_t)w;
	}
	char path[PATH_CAP];
	char tmp[PATH_CAP];
	char file[64];
	(void)snprintf(file, sizeof(file), "failures.tmp.%lu", process_id());
	if (!join(path, sizeof(path), dir, "failures") || !join(tmp, sizeof(tmp), dir, file)) {
		return;
	}
	fs_unlink(tmp);
	if (fs_write_new(tmp, txt, o) && !fs_rename(tmp, path, true)) {
		fs_unlink(tmp);
	}
}

bool
u_activation_store_rate_limited(const char *dir, int64_t now_unix_s)
{
	if (dir == NULL || dir[0] == '\0' || !fs_dir_ok(dir)) {
		return false;
	}
	int64_t t[FAILURES_KEPT];
	return failures_read(dir, now_unix_s, t, FAILURES_KEPT) >= U_ACTIVATION_FAIL_LIMIT;
}

//! Copy @p in into @p out keeping only [A-Za-z0-9._-]; anything else becomes '?'. "" -> "-".
static void
log_field(const char *in, char *out, size_t cap)
{
	size_t o = 0;
	for (const char *p = in != NULL ? in : ""; *p != '\0' && o + 1 < cap; p++) {
		const unsigned char c = (unsigned char)*p;
		const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
		                c == '.' || c == '_' || c == '-';
		out[o++] = ok ? (char)c : '?';
	}
	if (o == 0 && cap > 1) {
		out[o++] = '-';
	}
	out[o] = '\0';
}

void
u_activation_store_log(
    const char *dir, int64_t now_unix_s, const char *nonce_id, const char *cls, const char *id, const char *outcome)
{
	if (dir == NULL || dir[0] == '\0' || !fs_dir_ok(dir)) {
		return;
	}
	char f_nonce[16];
	char f_cls[U_ACTIVATION_CLASS_MAX];
	char f_id[U_ACTIVATION_ID_MAX];
	char f_out[48];
	log_field(nonce_id, f_nonce, sizeof(f_nonce));
	log_field(cls, f_cls, sizeof(f_cls));
	log_field(id, f_id, sizeof(f_id));
	log_field(outcome, f_out, sizeof(f_out));
	char line[256];
	const int n = snprintf(line, sizeof(line), "t=%lld nonce=%s class=%s id=%s outcome=%s\n", (long long)now_unix_s,
	                       f_nonce, f_cls, f_id, f_out);
	char path[PATH_CAP];
	if (n <= 0 || n >= (int)sizeof(line) || !join(path, sizeof(path), dir, "activation.log")) {
		return;
	}
	fs_append_capped(path, line, (size_t)n);
}
