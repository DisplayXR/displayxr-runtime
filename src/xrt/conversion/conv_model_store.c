// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The runtime-owned conversion model store (see conv_model_store.h).
 * @ingroup aux_conversion
 */

#include "conv_model_store.h"

#include "os/os_threading.h"
#include "os/os_time.h"
#include "util/u_json.h"
#include "util/u_credential.h"
#include "util/u_logging.h"
#include "util/u_sha256.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef XRT_OS_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <winhttp.h>
#else
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif


/*
 *
 * Small helpers.
 *
 */

static void
cpy(char *dst, size_t n, const char *src)
{
	if (dst == NULL || n == 0) {
		return;
	}
	(void)snprintf(dst, n, "%s", src != NULL ? src : "");
}

static bool
set(const char *s)
{
	return s != NULL && s[0] != '\0';
}

static bool
is_sha_hex(const char *s)
{
	if (s == NULL || strlen(s) != 64) {
		return false;
	}
	for (int i = 0; i < 64; i++) {
		if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) {
			return false;
		}
	}
	return true;
}

static void
lower_inplace(char *s)
{
	for (; s != NULL && *s != '\0'; s++) {
		*s = (char)tolower((unsigned char)*s);
	}
}

#ifdef XRT_OS_WINDOWS
#define SEP "\\"
#else
#define SEP "/"
#endif

static void
join(char *out, size_t n, const char *a, const char *b)
{
	const size_t la = strlen(a);
	const bool has_sep = la > 0 && (a[la - 1] == '/' || a[la - 1] == '\\');
	(void)snprintf(out, n, "%s%s%s", a, has_sep || la == 0 ? "" : SEP, b);
}

static void
dirname_of(const char *path, char *out, size_t n)
{
	cpy(out, n, path);
	char *last = NULL;
	for (char *c = out; *c != '\0'; c++) {
		if (*c == '/' || *c == '\\') {
			last = c;
		}
	}
	if (last != NULL) {
		*last = '\0';
	} else {
		cpy(out, n, ".");
	}
}

static uint64_t
now_ns(void)
{
	return os_monotonic_get_ns();
}

static void
sleep_ms(uint32_t ms)
{
	if (ms > 0) {
		os_nanosleep((int64_t)ms * 1000000);
	}
}

static uint64_t
process_id(void)
{
#ifdef XRT_OS_WINDOWS
	return (uint64_t)GetCurrentProcessId();
#else
	return (uint64_t)getpid();
#endif
}


/*
 *
 * File system (UTF-8 paths; wide on Windows).
 *
 */

struct fs_stat
{
	bool exists;
	bool is_dir;
	uint64_t size;
	uint64_t mtime;   //!< opaque
	uint64_t file_id; //!< opaque (volume + index / dev + inode)
};

#ifdef XRT_OS_WINDOWS
static bool
widen(const char *s, wchar_t *out, int n)
{
	return MultiByteToWideChar(CP_UTF8, 0, s, -1, out, n) > 0;
}

static bool
narrow(const wchar_t *s, char *out, int n)
{
	return WideCharToMultiByte(CP_UTF8, 0, s, -1, out, n, NULL, NULL) > 0;
}
#endif

static void
fs_stat(const char *path, struct fs_stat *st)
{
	memset(st, 0, sizeof(*st));
#ifdef XRT_OS_WINDOWS
	wchar_t w[CONV_STORE_PATH_MAX];
	if (!widen(path, w, CONV_STORE_PATH_MAX)) {
		return;
	}
	WIN32_FILE_ATTRIBUTE_DATA a;
	if (!GetFileAttributesExW(w, GetFileExInfoStandard, &a)) {
		return;
	}
	st->exists = true;
	st->is_dir = (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
	st->size = ((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow;
	st->mtime = ((uint64_t)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime;
	if (!st->is_dir) {
		HANDLE h = CreateFileW(w, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
		                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		if (h != INVALID_HANDLE_VALUE) {
			BY_HANDLE_FILE_INFORMATION fi;
			if (GetFileInformationByHandle(h, &fi)) {
				st->file_id = ((uint64_t)fi.nFileIndexHigh << 32 | fi.nFileIndexLow) ^
				              ((uint64_t)fi.dwVolumeSerialNumber << 48);
			}
			CloseHandle(h);
		}
	}
#else
	struct stat s;
	if (stat(path, &s) != 0) {
		return;
	}
	st->exists = true;
	st->is_dir = S_ISDIR(s.st_mode);
	st->size = (uint64_t)s.st_size;
#ifdef __APPLE__
	st->mtime = (uint64_t)s.st_mtimespec.tv_sec * 1000000000ull + (uint64_t)s.st_mtimespec.tv_nsec;
#else
	st->mtime = (uint64_t)s.st_mtim.tv_sec * 1000000000ull + (uint64_t)s.st_mtim.tv_nsec;
#endif
	st->file_id = ((uint64_t)s.st_dev << 40) ^ (uint64_t)s.st_ino;
#endif
}

static bool
fs_exists(const char *path)
{
	struct fs_stat st;
	fs_stat(path, &st);
	return st.exists;
}

static FILE *
fs_open(const char *path, const char *mode)
{
#ifdef XRT_OS_WINDOWS
	wchar_t w[CONV_STORE_PATH_MAX], m[8];
	if (!widen(path, w, CONV_STORE_PATH_MAX) || !widen(mode, m, 8)) {
		return NULL;
	}
	return _wfopen(w, m);
#else
	return fopen(path, mode);
#endif
}

static bool
fs_remove(const char *path)
{
#ifdef XRT_OS_WINDOWS
	wchar_t w[CONV_STORE_PATH_MAX];
	return widen(path, w, CONV_STORE_PATH_MAX) && DeleteFileW(w);
#else
	return unlink(path) == 0;
#endif
}

static bool
fs_mkdir(const char *path)
{
#ifdef XRT_OS_WINDOWS
	wchar_t w[CONV_STORE_PATH_MAX];
	return widen(path, w, CONV_STORE_PATH_MAX) && (CreateDirectoryW(w, NULL) || GetLastError() == ERROR_ALREADY_EXISTS);
#else
	return mkdir(path, 0755) == 0 || errno == EEXIST;
#endif
}

static bool
fs_mkdir_p(const char *path)
{
	char tmp[CONV_STORE_PATH_MAX];
	cpy(tmp, sizeof(tmp), path);
	const size_t n = strlen(tmp);
	for (size_t i = 1; i < n; i++) {
		if (tmp[i] == '/' || tmp[i] == '\\') {
			if (i == 2 && tmp[1] == ':') {
				continue; // "C:\"
			}
			const char c = tmp[i];
			tmp[i] = '\0';
			if (tmp[0] != '\0' && !fs_exists(tmp)) {
				(void)fs_mkdir(tmp);
			}
			tmp[i] = c;
		}
	}
	return fs_mkdir(tmp);
}

/*! Rename without replacing: 0 = renamed, 1 = the target exists, -1 = error. */
static int
fs_rename_noreplace(const char *from, const char *to)
{
#ifdef XRT_OS_WINDOWS
	wchar_t wf[CONV_STORE_PATH_MAX], wt[CONV_STORE_PATH_MAX];
	if (!widen(from, wf, CONV_STORE_PATH_MAX) || !widen(to, wt, CONV_STORE_PATH_MAX)) {
		return -1;
	}
	if (MoveFileExW(wf, wt, MOVEFILE_WRITE_THROUGH)) {
		return 0;
	}
	const DWORD e = GetLastError();
	return e == ERROR_ALREADY_EXISTS || e == ERROR_FILE_EXISTS ? 1 : -1;
#else
	// link() is the atomic no-replace rename on every POSIX file system we target.
	if (link(from, to) == 0) {
		(void)unlink(from);
		return 0;
	}
	if (errno == EEXIST) {
		return 1;
	}
	// File systems without hard links: fall back to a checked rename.
	if (fs_exists(to)) {
		return 1;
	}
	return rename(from, to) == 0 ? 0 : -1;
#endif
}

typedef void (*fs_list_fn)(void *user, const char *name, uint64_t size);

static void
fs_list(const char *dir, fs_list_fn fn, void *user)
{
#ifdef XRT_OS_WINDOWS
	char pat[CONV_STORE_PATH_MAX];
	join(pat, sizeof(pat), dir, "*");
	wchar_t w[CONV_STORE_PATH_MAX];
	if (!widen(pat, w, CONV_STORE_PATH_MAX)) {
		return;
	}
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW(w, &fd);
	if (h == INVALID_HANDLE_VALUE) {
		return;
	}
	do {
		if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
			continue;
		}
		char name[260 * 3];
		if (narrow(fd.cFileName, name, (int)sizeof(name))) {
			fn(user, name, ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow);
		}
	} while (FindNextFileW(h, &fd));
	FindClose(h);
#else
	DIR *d = opendir(dir);
	if (d == NULL) {
		return;
	}
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		if (e->d_name[0] == '.') {
			continue;
		}
		char p[CONV_STORE_PATH_MAX];
		join(p, sizeof(p), dir, e->d_name);
		struct fs_stat st;
		fs_stat(p, &st);
		if (st.exists && !st.is_dir) {
			fn(user, e->d_name, st.size);
		}
	}
	closedir(d);
#endif
}

static bool
fs_write_text(const char *path, const char *text)
{
	FILE *f = fs_open(path, "wb");
	if (f == NULL) {
		return false;
	}
	const size_t n = strlen(text);
	const bool ok = fwrite(text, 1, n, f) == n;
	return fclose(f) == 0 && ok;
}

static char *
fs_read_all(const char *path, size_t max)
{
	FILE *f = fs_open(path, "rb");
	if (f == NULL) {
		return NULL;
	}
	char *buf = malloc(max + 1);
	size_t n = buf != NULL ? fread(buf, 1, max, f) : 0;
	fclose(f);
	if (buf != NULL) {
		buf[n] = '\0';
	}
	return buf;
}


/*
 *
 * Root resolution.
 *
 */

bool
conv_store_root_from(const struct conv_store_root_inputs *in,
                     char *out,
                     size_t out_size,
                     char *source,
                     size_t source_size)
{
	char dummy[8];
	if (source == NULL) {
		source = dummy;
		source_size = sizeof(dummy);
	}
	cpy(out, out_size, "");
	cpy(source, source_size, "");
	if (in == NULL) {
		return false;
	}
	if (set(in->env_override)) {
		cpy(out, out_size, in->env_override);
		cpy(source, source_size, "env DXR_CONV_MODELS_ROOT");
		return true;
	}
	if (in->windows) {
		if (set(in->models_path)) {
			cpy(out, out_size, in->models_path);
			cpy(source, source_size, "HKLM Models\\Path");
			return true;
		}
		if (set(in->browser_models_path)) {
			cpy(out, out_size, in->browser_models_path);
			cpy(source, source_size, "HKLM Browser\\ModelsPath");
			return true;
		}
		if (set(in->program_data)) {
			(void)snprintf(out, out_size, "%s\\DisplayXR\\Models", in->program_data);
			cpy(source, source_size, "%ProgramData% default");
			return true;
		}
		return false;
	}
	if (set(in->xdg_data_home) && in->xdg_writable) {
		(void)snprintf(out, out_size, "%s/displayxr/models", in->xdg_data_home);
		cpy(source, source_size, "$XDG_DATA_HOME");
		return true;
	}
	cpy(out, out_size, "/var/lib/displayxr/models");
	cpy(source, source_size, "system default");
	return true;
}

#ifdef XRT_OS_WINDOWS
static bool
reg_sz_hklm(const wchar_t *sub, const wchar_t *value, char *out, size_t out_size)
{
	wchar_t buf[CONV_STORE_PATH_MAX];
	DWORD bytes = sizeof(buf);
	if (RegGetValueW(HKEY_LOCAL_MACHINE, sub, value, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_SUBKEY_WOW6464KEY,
	                 NULL, buf, &bytes) != ERROR_SUCCESS) {
		return false;
	}
	return narrow(buf, out, (int)out_size) && out[0] != '\0';
}
#endif

bool
conv_store_root_resolve(char *out, size_t out_size, char *source, size_t source_size)
{
	struct conv_store_root_inputs in;
	memset(&in, 0, sizeof(in));
	in.env_override = getenv("DXR_CONV_MODELS_ROOT");
#ifdef XRT_OS_WINDOWS
	in.windows = true;
	char models[CONV_STORE_PATH_MAX] = "", browser[CONV_STORE_PATH_MAX] = "", pd[CONV_STORE_PATH_MAX] = "";
	if (reg_sz_hklm(L"Software\\DisplayXR\\Models", L"Path", models, sizeof(models))) {
		in.models_path = models;
	}
	if (reg_sz_hklm(L"Software\\DisplayXR\\Browser", L"ModelsPath", browser, sizeof(browser))) {
		in.browser_models_path = browser;
	}
	wchar_t wpd[CONV_STORE_PATH_MAX];
	const DWORD n = GetEnvironmentVariableW(L"ProgramData", wpd, CONV_STORE_PATH_MAX);
	if (n > 0 && n < CONV_STORE_PATH_MAX && narrow(wpd, pd, (int)sizeof(pd))) {
		in.program_data = pd;
	} else {
		in.program_data = "C:\\ProgramData";
	}
#else
	in.windows = false;
	static char xdg[CONV_STORE_PATH_MAX];
	const char *x = getenv("XDG_DATA_HOME");
	const char *home = getenv("HOME");
	if (set(x)) {
		cpy(xdg, sizeof(xdg), x);
	} else if (set(home)) {
		(void)snprintf(xdg, sizeof(xdg), "%s/.local/share", home);
	} else {
		xdg[0] = '\0';
	}
	in.xdg_data_home = xdg;
	// Writable = the nearest existing ancestor of <xdg>/displayxr/models is.
	char probe[CONV_STORE_PATH_MAX];
	(void)snprintf(probe, sizeof(probe), "%s/displayxr/models", xdg);
	while (xdg[0] != '\0' && probe[0] != '\0' && !fs_exists(probe)) {
		char *slash = strrchr(probe, '/');
		if (slash == NULL || slash == probe) {
			break;
		}
		*slash = '\0';
	}
	in.xdg_writable = xdg[0] != '\0' && access(probe, W_OK) == 0;
#endif
	return conv_store_root_from(&in, out, out_size, source, source_size);
}


/*
 *
 * Pack manifests.
 *
 */

static void
jstr(const cJSON *o, const char *key, char *out, size_t n)
{
	const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
	cpy(out, n, cJSON_IsString(v) && v->valuestring != NULL ? v->valuestring : "");
}

static bool
jbool(const cJSON *o, const char *key, bool dflt)
{
	const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
	return cJSON_IsBool(v) ? cJSON_IsTrue(v) : dflt;
}

static bool
perr(char *err, size_t n, const char *what)
{
	cpy(err, n, what);
	return false;
}

const struct conv_pack_model *
conv_pack_find_model(const struct conv_pack *p, const char *name)
{
	for (uint32_t i = 0; p != NULL && name != NULL && i < p->model_count; i++) {
		if (strcmp(p->models[i].name, name) == 0) {
			return &p->models[i];
		}
	}
	return NULL;
}

bool
conv_pack_parse(const char *json, const char *base_dir, struct conv_pack *out, char *err, size_t err_size)
{
	char dummy[8];
	if (err == NULL) {
		err = dummy;
		err_size = sizeof(dummy);
	}
	cpy(err, err_size, "");
	memset(out, 0, sizeof(*out));
	cpy(out->base_dir, sizeof(out->base_dir), base_dir != NULL ? base_dir : "");
	cJSON *root = json != NULL ? cJSON_Parse(json) : NULL;
	if (root == NULL || !cJSON_IsObject(root)) {
		cJSON_Delete(root);
		return perr(err, err_size, "not a JSON object");
	}
	bool ok = false;
	const cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema");
	const cJSON *models = cJSON_GetObjectItemCaseSensitive(root, "models");
	if (!cJSON_IsNumber(schema) || schema->valueint != 2) {
		perr(err, err_size, "schema is not 2");
		goto done;
	}
	jstr(root, "pack", out->pack, sizeof(out->pack));
	jstr(root, "provider", out->provider, sizeof(out->provider));
	jstr(root, "generated", out->generated, sizeof(out->generated));
	jstr(root, "blobBaseUrl", out->blob_base_url, sizeof(out->blob_base_url));
	if (out->pack[0] == '\0' || strchr(out->pack, '/') != NULL) {
		perr(err, err_size, "missing or invalid 'pack'");
		goto done;
	}
	if (!cJSON_IsArray(models) || cJSON_GetArraySize(models) == 0) {
		perr(err, err_size, "'models' missing or empty");
		goto done;
	}
	const cJSON *m = NULL;
	cJSON_ArrayForEach(m, models)
	{
		if (out->model_count >= CONV_STORE_MAX_MODELS) {
			break;
		}
		struct conv_pack_model *pm = &out->models[out->model_count];
		jstr(m, "name", pm->name, sizeof(pm->name));
		jstr(m, "version", pm->version, sizeof(pm->version));
		jstr(m, "task", pm->task, sizeof(pm->task));
		if (pm->name[0] == '\0' || strchr(pm->name, '/') != NULL) {
			perr(err, err_size, "a model has no valid 'name'");
			goto done;
		}
		if (conv_pack_find_model(out, pm->name) != NULL) {
			(void)snprintf(err, err_size, "duplicate model '%s'", pm->name);
			goto done;
		}
		pm->prefetch = jbool(m, "prefetch", false);
		// ADR-057 §5: an optional per-model URL and auth block.
		jstr(m, "url", pm->url, sizeof(pm->url));
		const cJSON *auth = cJSON_GetObjectItemCaseSensitive(m, "auth");
		if (auth != NULL) {
			char kind[32] = {0};
			if (!cJSON_IsObject(auth)) {
				(void)snprintf(err, err_size, "model '%s': 'auth' is not an object", pm->name);
				goto done;
			}
			jstr(auth, "kind", kind, sizeof(kind));
			jstr(auth, "credential", pm->auth.credential, sizeof(pm->auth.credential));
			if (!conv_pack_auth_kind_parse(kind, &pm->auth.kind)) {
				(void)snprintf(err, err_size, "model '%s': auth.kind '%s' is not bearer or huggingface",
				               pm->name, kind);
				goto done;
			}
			if (!u_credential_name_valid(pm->auth.credential)) {
				(void)snprintf(err, err_size, "model '%s': auth.credential must name a credential",
				               pm->name);
				goto done;
			}
		}
		if (pm->url[0] != '\0' && strncmp(pm->url, "https://", 8) != 0 && strncmp(pm->url, "http://", 7) != 0 &&
		    strncmp(pm->url, "file://", 7) != 0) {
			(void)snprintf(err, err_size, "model '%s': url must be https://, http:// or file://", pm->name);
			goto done;
		}
		if (pm->auth.kind != CONV_PACK_AUTH_NONE && pm->url[0] != '\0' &&
		    strncmp(pm->url, "https://", 8) != 0) {
			(void)snprintf(err, err_size, "model '%s': an auth block needs an https:// url", pm->name);
			goto done;
		}
		// Commercial = AND over the model and every dependency; missing = false.
		const cJSON *lic = cJSON_GetObjectItemCaseSensitive(m, "license");
		pm->commercial = cJSON_IsObject(lic) && jbool(lic, "commercial", false);
		if (cJSON_IsObject(lic)) {
			jstr(lic, "spdx", pm->spdx, sizeof(pm->spdx));
			const cJSON *deps = cJSON_GetObjectItemCaseSensitive(lic, "dependencies");
			const cJSON *d = NULL;
			cJSON_ArrayForEach(d, deps)
			{
				pm->commercial = pm->commercial && jbool(d, "commercial", false);
			}
		}
		const cJSON *variants = cJSON_GetObjectItemCaseSensitive(m, "variants");
		if (!cJSON_IsArray(variants) || cJSON_GetArraySize(variants) == 0) {
			(void)snprintf(err, err_size, "model '%s' has no variants", pm->name);
			goto done;
		}
		const cJSON *v = NULL;
		cJSON_ArrayForEach(v, variants)
		{
			if (pm->variant_count >= CONV_STORE_MAX_VARIANTS) {
				break;
			}
			struct conv_pack_variant *pv = &pm->variants[pm->variant_count];
			jstr(v, "id", pv->id, sizeof(pv->id));
			jstr(v, "backend", pv->backend, sizeof(pv->backend));
			if (pv->id[0] == '\0') {
				(void)snprintf(pv->id, sizeof(pv->id), "v%u", pm->variant_count);
			}
			const cJSON *files = cJSON_GetObjectItemCaseSensitive(v, "files");
			if (!cJSON_IsArray(files) || cJSON_GetArraySize(files) == 0) {
				(void)snprintf(err, err_size, "model '%s' variant '%s' has no files", pm->name, pv->id);
				goto done;
			}
			const cJSON *f = NULL;
			cJSON_ArrayForEach(f, files)
			{
				if (pv->file_count >= CONV_STORE_MAX_FILES) {
					break;
				}
				struct conv_pack_file *pf = &pv->files[pv->file_count];
				jstr(f, "role", pf->role, sizeof(pf->role));
				jstr(f, "format", pf->format, sizeof(pf->format));
				jstr(f, "sha256", pf->sha256, sizeof(pf->sha256));
				lower_inplace(pf->sha256);
				if (pf->role[0] == '\0') {
					cpy(pf->role, sizeof(pf->role), "model");
				}
				const cJSON *size = cJSON_GetObjectItemCaseSensitive(f, "size");
				if (!is_sha_hex(pf->sha256) || !cJSON_IsNumber(size) || size->valuedouble <= 0) {
					(void)snprintf(err, err_size, "model '%s': a file without a 64-hex sha256 and a size",
					               pm->name);
					goto done;
				}
				pf->size = (uint64_t)size->valuedouble;
				pv->file_count++;
			}
			pm->variant_count++;
		}
		// The per-model url serves exactly one blob: every variant's `model` file.
		for (uint32_t vi = 0; pm->url[0] != '\0' && vi < pm->variant_count; vi++) {
			for (uint32_t fi = 0; fi < pm->variants[vi].file_count; fi++) {
				const struct conv_pack_file *pf = &pm->variants[vi].files[fi];
				if (strcmp(pf->role, "model") != 0) {
					continue;
				}
				if (pm->url_sha256[0] == '\0') {
					cpy(pm->url_sha256, sizeof(pm->url_sha256), pf->sha256);
				} else if (strcmp(pm->url_sha256, pf->sha256) != 0) {
					(void)snprintf(
					    err, err_size,
					    "model '%s': 'url' is ambiguous (its variants have different model files)",
					    pm->name);
					goto done;
				}
			}
		}
		if (pm->url[0] != '\0' && pm->url_sha256[0] == '\0') {
			(void)snprintf(err, err_size, "model '%s': 'url' given but no file has role 'model'", pm->name);
			goto done;
		}
		out->model_count++;
	}
	const cJSON *defaults = cJSON_GetObjectItemCaseSensitive(root, "defaults");
	const cJSON *d = NULL;
	cJSON_ArrayForEach(d, defaults)
	{
		if (out->default_count >= CONV_STORE_MAX_DEFAULTS || d->string == NULL) {
			break;
		}
		struct conv_pack_default *pd = &out->defaults[out->default_count];
		cpy(pd->task, sizeof(pd->task), d->string);
		jstr(d, "low", pd->low, sizeof(pd->low));
		jstr(d, "medium", pd->medium, sizeof(pd->medium));
		jstr(d, "high", pd->high, sizeof(pd->high));
		const char *names[3] = {pd->low, pd->medium, pd->high};
		for (int k = 0; k < 3; k++) {
			if (names[k][0] != '\0' && conv_pack_find_model(out, names[k]) == NULL) {
				(void)snprintf(err, err_size, "defaults.%s names unknown model '%s'", pd->task, names[k]);
				goto done;
			}
		}
		out->default_count++;
	}
	ok = true;
done:
	cJSON_Delete(root);
	return ok;
}

bool
conv_pack_load(const char *path, struct conv_pack *out, char *err, size_t err_size)
{
	char *text = fs_read_all(path, 4u * 1024u * 1024u);
	if (text == NULL) {
		memset(out, 0, sizeof(*out));
		if (err != NULL) {
			(void)snprintf(err, err_size, "cannot read %s", path);
		}
		return false;
	}
	char dir[CONV_STORE_PATH_MAX];
	dirname_of(path, dir, sizeof(dir));
	const bool ok = conv_pack_parse(text, dir, out, err, err_size);
	free(text);
	if (ok) {
		cpy(out->manifest_path, sizeof(out->manifest_path), path);
	}
	return ok;
}

static bool
has_scheme(const char *u)
{
	const char *c = strstr(u, "://");
	return c != NULL && c > u && (size_t)(c - u) < 10;
}

void
conv_pack_blob_url(const struct conv_pack *p, const struct conv_pack_file *f, char *out, size_t out_size)
{
	char base[CONV_STORE_PATH_MAX];
	if (has_scheme(p->blob_base_url)) {
		cpy(base, sizeof(base), p->blob_base_url);
	} else if (has_scheme(p->base_dir)) {
		// A remote manifest: relative to its URL's directory.
		(void)snprintf(base, sizeof(base), "%s/%s", p->base_dir,
		               strcmp(p->blob_base_url, ".") == 0 ? "" : p->blob_base_url);
	} else {
		// Relative (or empty): under the manifest's directory.
		char dir[CONV_STORE_PATH_MAX];
		if (p->blob_base_url[0] != '\0' && strcmp(p->blob_base_url, ".") != 0) {
			join(dir, sizeof(dir), p->base_dir, p->blob_base_url);
		} else {
			cpy(dir, sizeof(dir), p->base_dir);
		}
		(void)snprintf(base, sizeof(base), "file://%s", dir);
	}
	size_t n = strlen(base);
	while (n > 0 && (base[n - 1] == '/' || base[n - 1] == '\\')) {
		base[--n] = '\0';
	}
	(void)snprintf(out, out_size, "%s/%s%s%s", base, f->sha256, f->format[0] != '\0' ? "." : "", f->format);
}


bool
conv_pack_auth_kind_parse(const char *s, uint32_t *out)
{
	if (s != NULL && strcmp(s, "bearer") == 0) {
		*out = CONV_PACK_AUTH_BEARER;
		return true;
	}
	if (s != NULL && strcmp(s, "huggingface") == 0) {
		*out = CONV_PACK_AUTH_HUGGINGFACE;
		return true;
	}
	return false;
}

void
conv_pack_file_url(const struct conv_pack *p,
                   const struct conv_pack_model *m,
                   const struct conv_pack_file *f,
                   char *out,
                   size_t out_size)
{
	if (m != NULL && m->url[0] != '\0' && strcmp(m->url_sha256, f->sha256) == 0) {
		cpy(out, out_size, m->url);
		return;
	}
	conv_pack_blob_url(p, f, out, out_size);
}

//! The model's auth when @p f is the blob its url serves, else none (ADR-057 §5).
static const struct conv_pack_auth *
file_auth(const struct conv_pack_model *m, const struct conv_pack_file *f)
{
	if (m == NULL || m->auth.kind == CONV_PACK_AUTH_NONE) {
		return NULL;
	}
	// With its own url, the auth is that url's (the other files come from the
	// pack's blobBaseUrl, unauthenticated); without one, the model's files all
	// come from a gated blobBaseUrl and every one is fetched with it.
	if (m->url[0] != '\0' && strcmp(m->url_sha256, f->sha256) != 0) {
		return NULL;
	}
	return &m->auth;
}


/*
 *
 * The store object.
 *
 */

enum blob_st
{
	B_IDLE = 0,
	B_QUEUED,
	B_VERIFYING,
	B_DOWNLOADING,
	B_READY,
	B_FAILED,
	B_REFUSED, //!< ABSENT(policy)
};

struct blob
{
	bool used;
	char sha[CONV_STORE_SHA_HEX];
	uint64_t size;
	char url[CONV_STORE_PATH_MAX];
	struct conv_pack_auth auth; //!< ADR-057 §5: how to authenticate the fetch (kind NONE = public)
	uint32_t pack_mask;         //!< packs (by index) that reference it
	uint32_t st;
	uint32_t fail; //!< XRT_CONV_FAIL_* while FAILED
	uint64_t done; //!< bytes downloaded / hashed so far
	bool verified; //!< hash checked by this process for key below
	uint64_t key_id, key_mtime, key_size;
};

struct conv_model_store
{
	struct conv_model_store_config cfg;
	char blobs_dir[CONV_STORE_PATH_MAX];
	char packs_dir[CONV_STORE_PATH_MAX];

	struct os_mutex mtx;
	struct os_cond cond;
	struct os_thread thread;
	bool thread_started;
	bool stop;
	uint32_t busy; //!< jobs in flight on the worker

	uint32_t pack_count;
	struct conv_pack *packs; //!< [CONV_STORE_MAX_PACKS]
	struct blob blobs[CONV_STORE_MAX_BLOBS];
};

const char *
conv_store_blob_result_str(enum conv_store_blob_result r)
{
	switch (r) {
	case CONV_STORE_BLOB_OK: return "ok";
	case CONV_STORE_BLOB_SKIPPED: return "skipped";
	case CONV_STORE_BLOB_MISSING: return "missing";
	case CONV_STORE_BLOB_TAMPERED: return "tampered";
	case CONV_STORE_BLOB_TAMPERED_LOCKED: return "tampered-locked";
	case CONV_STORE_BLOB_BAD_DOWNLOAD: return "bad-download";
	case CONV_STORE_BLOB_NETWORK: return "network";
	case CONV_STORE_BLOB_POLICY: return "policy";
	case CONV_STORE_BLOB_IO: return "io";
	case CONV_STORE_BLOB_CANCELLED: return "cancelled";
	case CONV_STORE_BLOB_AUTH: return "auth";
	default: return "unknown";
	}
}

void
conv_model_store_config_defaults(struct conv_model_store_config *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->allow_downloads = true;
}

const char *
conv_model_store_root(const struct conv_model_store *s)
{
	return s != NULL ? s->cfg.root : "";
}

//! Blob slot for @p sha (created when @p create). mtx held.
static struct blob *
blob_get_locked(struct conv_model_store *s, const char *sha, uint64_t size, bool create)
{
	struct blob *free_slot = NULL;
	for (uint32_t i = 0; i < CONV_STORE_MAX_BLOBS; i++) {
		struct blob *b = &s->blobs[i];
		if (b->used && strcmp(b->sha, sha) == 0) {
			return b;
		}
		if (!b->used && free_slot == NULL) {
			free_slot = b;
		}
	}
	if (!create || free_slot == NULL) {
		return NULL;
	}
	memset(free_slot, 0, sizeof(*free_slot));
	free_slot->used = true;
	cpy(free_slot->sha, sizeof(free_slot->sha), sha);
	free_slot->size = size;
	return free_slot;
}

static void
blob_paths(const struct conv_model_store *s, const char *sha, char *blob, char *stamp)
{
	join(blob, CONV_STORE_PATH_MAX, s->blobs_dir, sha);
	if (stamp != NULL) {
		char name[CONV_STORE_SHA_HEX + 4];
		(void)snprintf(name, sizeof(name), "%s.ok", sha);
		join(stamp, CONV_STORE_PATH_MAX, s->blobs_dir, name);
	}
}

struct part_scan
{
	const char *sha;
	uint64_t largest;
	bool remove_all;
	const char *dir;
};

static void
part_scan_fn(void *user, const char *name, uint64_t size)
{
	struct part_scan *ps = (struct part_scan *)user;
	const size_t n = strlen(name);
	if (strncmp(name, ps->sha, 64) != 0 || name[64] != '.' || n < 70 || strcmp(name + n - 5, ".part") != 0) {
		return;
	}
	if (size > ps->largest) {
		ps->largest = size;
	}
	if (ps->remove_all) {
		char p[CONV_STORE_PATH_MAX];
		join(p, sizeof(p), ps->dir, name);
		(void)fs_remove(p);
	}
}

void
conv_model_store_blob_disk(struct conv_model_store *s, const char *sha256, struct conv_store_blob_disk *out)
{
	memset(out, 0, sizeof(*out));
	if (s == NULL || !is_sha_hex(sha256)) {
		return;
	}
	char blob[CONV_STORE_PATH_MAX], stamp[CONV_STORE_PATH_MAX];
	blob_paths(s, sha256, blob, stamp);
	struct fs_stat st;
	fs_stat(blob, &st);
	out->present = st.exists && !st.is_dir;
	out->size = st.size;
	out->stamped = fs_exists(stamp);
	struct part_scan ps = {sha256, 0, false, s->blobs_dir};
	fs_list(s->blobs_dir, part_scan_fn, &ps);
	out->part_bytes = ps.largest;
}


/*
 *
 * Root + ACL.
 *
 */

#ifdef XRT_OS_WINDOWS
static bool
apply_store_acl(const char *root)
{
	wchar_t w[CONV_STORE_PATH_MAX];
	if (!widen(root, w, CONV_STORE_PATH_MAX)) {
		return false;
	}
	PSECURITY_DESCRIPTOR sd = NULL;
	if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"" CONV_STORE_ROOT_SDDL, SDDL_REVISION_1, &sd,
	                                                          NULL)) {
		return false;
	}
	BOOL present = FALSE, defaulted = FALSE;
	PACL dacl = NULL;
	bool ok = GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted) && present;
	if (ok) {
		ok = SetNamedSecurityInfoW(w, SE_FILE_OBJECT,
		                           DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, NULL, NULL,
		                           dacl, NULL) == ERROR_SUCCESS;
	}
	LocalFree(sd);
	return ok;
}
#endif

bool
conv_model_store_ensure_root(struct conv_model_store *s)
{
	if (s == NULL || s->cfg.root[0] == '\0') {
		return false;
	}
	if (!fs_exists(s->cfg.root)) {
		if (!fs_mkdir_p(s->cfg.root)) {
			U_LOG_W("[conv-store] cannot create the model store root %s", s->cfg.root);
			return false;
		}
#ifdef XRT_OS_WINDOWS
		if (!apply_store_acl(s->cfg.root)) {
			U_LOG_W("[conv-store] created %s but could not apply the store ACL (error %lu)", s->cfg.root,
			        (unsigned long)GetLastError());
		}
#endif
		char marker[CONV_STORE_PATH_MAX];
		join(marker, sizeof(marker), s->cfg.root, CONV_STORE_OWNER_MARKER);
		(void)fs_write_text(marker, "displayxr-runtime\n");
		U_LOG_W("[conv-store] created the model store at %s", s->cfg.root);
	}
	const bool ok = fs_mkdir(s->blobs_dir);
	(void)fs_mkdir(s->packs_dir);
	return ok && fs_exists(s->blobs_dir);
}


/*
 *
 * Hash + transfer.
 *
 */

struct xfer
{
	struct conv_model_store *s;
	struct blob *b; //!< progress target (may be NULL: sync op without a table slot)
	const char *sha;
	uint64_t total;
	uint64_t last_ns;
	uint64_t last_done;
};

static bool
cancelled(struct conv_model_store *s)
{
	os_mutex_lock(&s->mtx);
	const bool c = s->stop;
	os_mutex_unlock(&s->mtx);
	return c;
}

static void
xfer_progress(struct xfer *x, uint64_t done, bool last)
{
	if (x->b != NULL) {
		os_mutex_lock(&x->s->mtx);
		x->b->done = done;
		os_mutex_unlock(&x->s->mtx);
	}
	if (x->s->cfg.on_progress == NULL) {
		return;
	}
	// 2 Hz / 5 %, plus the last chunk.
	const uint64_t t = now_ns();
	const uint64_t step = x->total / 20u;
	if (last || (t - x->last_ns >= 500000000ull && done - x->last_done >= (step > 0 ? step : 1))) {
		x->last_ns = t;
		x->last_done = done;
		x->s->cfg.on_progress(x->s->cfg.user, x->sha, done, x->total);
	}
}

static uint32_t
chunk_of(const struct conv_model_store *s)
{
	return s->cfg.chunk_bytes != 0 ? s->cfg.chunk_bytes : 1024u * 1024u;
}

//! Hash @p path; false on read error / cancel. *bytes = bytes read.
static bool
hash_file(struct conv_model_store *s, const char *path, char hex[CONV_STORE_SHA_HEX], uint64_t *bytes)
{
	FILE *f = fs_open(path, "rb");
	if (f == NULL) {
		return false;
	}
	const uint32_t chunk = chunk_of(s);
	uint8_t *buf = malloc(chunk);
	if (buf == NULL) {
		fclose(f);
		return false;
	}
	struct u_sha256 h;
	u_sha256_init(&h);
	uint64_t total = 0;
	size_t n;
	bool ok = true;
	while ((n = fread(buf, 1, chunk, f)) > 0) {
		u_sha256_update(&h, buf, n);
		total += n;
		if (cancelled(s)) {
			ok = false;
			break;
		}
	}
	ok = ok && !ferror(f);
	fclose(f);
	free(buf);
	uint8_t d[32];
	u_sha256_final(&h, d);
	for (int i = 0; i < 32; i++) {
		(void)snprintf(hex + i * 2, 3, "%02x", d[i]);
	}
	if (bytes != NULL) {
		*bytes = total;
	}
	return ok;
}

struct sink
{
	FILE *f;       //!< file sink, or NULL: memory below
	char *mem;     //!< memory sink (limit + 1 bytes)
	struct u_sha256 h;
	uint64_t written;
	uint64_t limit; //!< expected size: more is a bad download
	bool over;
};

static bool
sink_write(struct sink *k, const void *data, size_t n)
{
	if (k->written + n > k->limit) {
		k->over = true;
		return false;
	}
	if (k->f != NULL ? fwrite(data, 1, n, k->f) != n : k->mem == NULL) {
		return false;
	}
	if (k->f == NULL) {
		memcpy(k->mem + k->written, data, n);
	}
	u_sha256_update(&k->h, data, n);
	k->written += n;
	return true;
}

//! file:// URL -> local path ("file:///C:/x" -> "C:/x", "file:///x" -> "/x", "file://C:\x" -> "C:\x").
static void
file_url_path(const char *url, char *out, size_t n)
{
	const char *p = url + strlen("file://");
	if (p[0] == '/' && p[1] != '\0' && p[2] == ':') {
		p++; // "/C:/..."
	}
	cpy(out, n, p);
}

static enum conv_store_blob_result
fetch_file(struct conv_model_store *s, const char *url, struct sink *k, struct xfer *x)
{
	char path[CONV_STORE_PATH_MAX];
	file_url_path(url, path, sizeof(path));
	FILE *in = fs_open(path, "rb");
	if (in == NULL) {
		U_LOG_W("[conv-store] cannot open %s", path);
		return CONV_STORE_BLOB_NETWORK;
	}
	const uint32_t chunk = chunk_of(s);
	uint8_t *buf = malloc(chunk);
	enum conv_store_blob_result r = CONV_STORE_BLOB_OK;
	size_t n;
	while (buf != NULL && (n = fread(buf, 1, chunk, in)) > 0) {
		if (!sink_write(k, buf, n)) {
			r = k->over ? CONV_STORE_BLOB_BAD_DOWNLOAD : CONV_STORE_BLOB_IO;
			break;
		}
		xfer_progress(x, k->written, false);
		sleep_ms(s->cfg.throttle_ms);
		if (cancelled(s)) {
			r = CONV_STORE_BLOB_CANCELLED;
			break;
		}
	}
	if (buf == NULL || (r == CONV_STORE_BLOB_OK && ferror(in))) {
		r = CONV_STORE_BLOB_NETWORK;
	}
	free(buf);
	fclose(in);
	return r;
}

#ifdef XRT_OS_WINDOWS
static enum conv_store_blob_result
fetch_http(struct conv_model_store *s, const char *url, const char *bearer, struct sink *k, struct xfer *x)
{
	enum conv_store_blob_result r = CONV_STORE_BLOB_NETWORK;
	HINTERNET ses = WinHttpOpen(L"DisplayXR-ModelStore/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
	                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (ses == NULL) {
		ses = WinHttpOpen(L"DisplayXR-ModelStore/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
		                  WINHTTP_NO_PROXY_BYPASS, 0);
	}
	HINTERNET con = NULL, req = NULL;
	if (ses == NULL) {
		goto out;
	}
	(void)WinHttpSetTimeouts(ses, 15000, 15000, 30000, 30000);

	// ADR-057 §5: with a credential, redirects are followed HERE, not by
	// WinHTTP, so the token goes only to the host the pack named (a gated
	// repository 302s to a CDN that must never see it). Without one, WinHTTP
	// follows them itself, as before.
	wchar_t cur[2048];
	if (!widen(url, cur, 2048)) {
		goto out;
	}
	wchar_t origin_host[256] = {0};
	DWORD status = 0;
	for (int hop = 0; hop < 6; hop++) {
		URL_COMPONENTS uc;
		memset(&uc, 0, sizeof(uc));
		uc.dwStructSize = sizeof(uc);
		wchar_t host[256], path[2048];
		uc.lpszHostName = host;
		uc.dwHostNameLength = 256;
		uc.lpszUrlPath = path;
		uc.dwUrlPathLength = 2048;
		if (!WinHttpCrackUrl(cur, 0, 0, &uc)) {
			goto out;
		}
		if (hop == 0) {
			(void)wcsncpy(origin_host, host, 255);
		}
		const bool send_token =
		    bearer != NULL && uc.nScheme == INTERNET_SCHEME_HTTPS && _wcsicmp(host, origin_host) == 0;
		if (req != NULL) {
			WinHttpCloseHandle(req);
			req = NULL;
		}
		if (con != NULL) {
			WinHttpCloseHandle(con);
			con = NULL;
		}
		con = WinHttpConnect(ses, host, uc.nPort, 0);
		if (con == NULL) {
			goto out;
		}
		req = WinHttpOpenRequest(con, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
		                         uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
		if (req == NULL) {
			goto out;
		}
		wchar_t hdr[U_CREDENTIAL_SECRET_MAX + 32];
		hdr[0] = L'\0';
		if (bearer != NULL) {
			DWORD never = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
			(void)WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &never, sizeof(never));
		}
		if (send_token) {
			wchar_t wtok[U_CREDENTIAL_SECRET_MAX + 1];
			if (!widen(bearer, wtok, U_CREDENTIAL_SECRET_MAX + 1)) {
				goto out;
			}
			(void)_snwprintf(hdr, sizeof(hdr) / sizeof(hdr[0]) - 1, L"Authorization: Bearer %ls", wtok);
			hdr[sizeof(hdr) / sizeof(hdr[0]) - 1] = L'\0';
			SecureZeroMemory(wtok, sizeof(wtok));
		}
		const BOOL sent =
		    WinHttpSendRequest(req, hdr[0] != L'\0' ? hdr : WINHTTP_NO_ADDITIONAL_HEADERS,
		                       hdr[0] != L'\0' ? (DWORD)-1L : 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
		SecureZeroMemory(hdr, sizeof(hdr));
		if (!sent || !WinHttpReceiveResponse(req, NULL)) {
			goto out;
		}
		DWORD sz = sizeof(status);
		if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
		                         WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX)) {
			goto out;
		}
		const bool redirect = status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
		if (!redirect || bearer == NULL) {
			break;
		}
		DWORD lsz = sizeof(cur);
		if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, cur, &lsz,
		                         WINHTTP_NO_HEADER_INDEX)) {
			goto out;
		}
		if (wcsstr(cur, L"://") == NULL) {
			U_LOG_W("[conv-store] %s: a relative redirect is not followed with a credential", url);
			goto out;
		}
	}
	if (status == 401 || status == 403) {
		U_LOG_W("[conv-store] %s: HTTP %lu (the credential was refused or is missing)", url,
		        (unsigned long)status);
		r = bearer != NULL ? CONV_STORE_BLOB_AUTH : CONV_STORE_BLOB_NETWORK;
		goto out;
	}
	if (status != 200) {
		U_LOG_W("[conv-store] %s: HTTP %lu", url, (unsigned long)status);
		goto out;
	}
	const uint32_t chunk = chunk_of(s);
	uint8_t *buf = malloc(chunk);
	r = CONV_STORE_BLOB_OK;
	for (;;) {
		DWORD got = 0;
		if (buf == NULL || !WinHttpReadData(req, buf, chunk, &got)) {
			r = CONV_STORE_BLOB_NETWORK;
			break;
		}
		if (got == 0) {
			break;
		}
		if (!sink_write(k, buf, got)) {
			r = k->over ? CONV_STORE_BLOB_BAD_DOWNLOAD : CONV_STORE_BLOB_IO;
			break;
		}
		xfer_progress(x, k->written, false);
		sleep_ms(s->cfg.throttle_ms);
		if (cancelled(s)) {
			r = CONV_STORE_BLOB_CANCELLED;
			break;
		}
	}
	free(buf);
out:
	if (req != NULL) {
		WinHttpCloseHandle(req);
	}
	if (con != NULL) {
		WinHttpCloseHandle(con);
	}
	if (ses != NULL) {
		WinHttpCloseHandle(ses);
	}
	return r;
}
#endif

static enum conv_store_blob_result
fetch_url(
    struct conv_model_store *s, const char *url, const struct conv_pack_auth *auth, struct sink *k, struct xfer *x)
{
	if (strncmp(url, "file://", 7) == 0) {
		return fetch_file(s, url, k, x); // a local file needs no credential
	}
	// ADR-057 §5: a gated source's token, read from the OS credential store now
	// and wiped after the fetch. Never sent over plain http.
	char token[U_CREDENTIAL_SECRET_MAX + 1];
	token[0] = '\0';
	if (auth != NULL && auth->kind != CONV_PACK_AUTH_NONE) {
		if (strncmp(url, "https://", 8) != 0) {
			U_LOG_W("[conv-store] refusing to send credential '%s' to a non-https URL", auth->credential);
			return CONV_STORE_BLOB_AUTH;
		}
		char err[256];
		const enum u_credential_result cr =
		    u_credential_read(auth->credential, token, sizeof(token), err, sizeof(err));
		if (cr != U_CREDENTIAL_OK) {
			U_LOG_W("[conv-store] model credential: %s", err);
			return CONV_STORE_BLOB_AUTH;
		}
	}
	enum conv_store_blob_result r = CONV_STORE_BLOB_NETWORK;
#ifdef XRT_OS_WINDOWS
	if (strncmp(url, "https://", 8) == 0 || strncmp(url, "http://", 7) == 0) {
		r = fetch_http(s, url, token[0] != '\0' ? token : NULL, k, x);
	} else
#endif
	{
		U_LOG_W("[conv-store] no downloader for %s on this platform", url);
	}
	u_credential_wipe(token, sizeof(token));
	return r;
}


/*
 *
 * Manifest by URL.
 *
 */

bool
conv_pack_load_url(const char *url_or_path, struct conv_pack *out, char *err, size_t err_size)
{
	if (url_or_path == NULL) {
		return false;
	}
	if (!has_scheme(url_or_path)) {
		return conv_pack_load(url_or_path, out, err, err_size);
	}
	if (strncmp(url_or_path, "file://", 7) == 0) {
		char path[CONV_STORE_PATH_MAX];
		file_url_path(url_or_path, path, sizeof(path));
		return conv_pack_load(path, out, err, err_size);
	}
	// Remote: into memory (a manifest is small), through a throwaway store
	// object for the downloader's chunking and cancel check.
	struct conv_model_store_config cfg;
	conv_model_store_config_defaults(&cfg);
	cpy(cfg.root, sizeof(cfg.root), ".");
	struct conv_model_store *s = NULL;
	if (!conv_model_store_create(&cfg, &s)) {
		return false;
	}
	const uint64_t max = 4u * 1024u * 1024u;
	struct sink k;
	memset(&k, 0, sizeof(k));
	k.limit = max;
	k.mem = malloc((size_t)max + 1);
	u_sha256_init(&k.h);
	struct xfer x = {s, NULL, "manifest", max, 0, 0};
	const enum conv_store_blob_result r =
	    k.mem != NULL ? fetch_url(s, url_or_path, NULL, &k, &x) : CONV_STORE_BLOB_IO;
	conv_model_store_destroy(&s);
	if (r != CONV_STORE_BLOB_OK) {
		free(k.mem);
		if (err != NULL) {
			(void)snprintf(err, err_size, "cannot fetch %s (%s)", url_or_path, conv_store_blob_result_str(r));
		}
		memset(out, 0, sizeof(*out));
		return false;
	}
	k.mem[k.written] = '\0';
	char dir[CONV_STORE_PATH_MAX];
	cpy(dir, sizeof(dir), url_or_path);
	char *slash = strrchr(dir, '/');
	if (slash != NULL) {
		*slash = '\0';
	}
	const bool ok = conv_pack_parse(k.mem, dir, out, err, err_size);
	free(k.mem);
	return ok;
}


/*
 *
 * The write protocol (one blob).
 *
 */

static void
remember_verified(struct conv_model_store *s, const char *sha, uint64_t size, const char *blob_path)
{
	struct fs_stat st;
	fs_stat(blob_path, &st);
	os_mutex_lock(&s->mtx);
	struct blob *b = blob_get_locked(s, sha, size, true);
	if (b != NULL) {
		b->verified = true;
		b->key_id = st.file_id;
		b->key_mtime = st.mtime;
		b->key_size = st.size;
	}
	os_mutex_unlock(&s->mtx);
}

static bool
write_stamp(const char *stamp, const char *sha, uint64_t size)
{
	char text[128];
	(void)snprintf(text, sizeof(text), "%s %llu\n", sha, (unsigned long long)size);
	return fs_write_text(stamp, text);
}

/*!
 * Hash the blob at @p blob; OK (stamped, remembered) or TAMPERED(_LOCKED)
 * after deleting it. Used for a stamped blob, an unstamped right-size blob,
 * and the winner of a rename race.
 */
static enum conv_store_blob_result
check_present(struct conv_model_store *s, const char *sha, uint64_t size, const char *blob, const char *stamp)
{
	char hex[CONV_STORE_SHA_HEX];
	uint64_t n = 0;
	if (!hash_file(s, blob, hex, &n)) {
		return cancelled(s) ? CONV_STORE_BLOB_CANCELLED : CONV_STORE_BLOB_IO;
	}
	if (n == size && strcmp(hex, sha) == 0) {
		if (!fs_exists(stamp)) {
			(void)write_stamp(stamp, sha, size);
		}
		remember_verified(s, sha, size, blob);
		return CONV_STORE_BLOB_OK;
	}
	U_LOG_W("[conv-store] blob %.12s… does not hash to its name (%llu bytes): rejected", sha,
	        (unsigned long long)n);
	(void)fs_remove(stamp);
	if (!fs_remove(blob)) {
		return CONV_STORE_BLOB_TAMPERED_LOCKED;
	}
	return CONV_STORE_BLOB_TAMPERED;
}

static void
nonce_of(const struct conv_model_store *s, char *out, size_t n)
{
	if (s->cfg.nonce[0] != '\0') {
		cpy(out, n, s->cfg.nonce);
		return;
	}
	(void)snprintf(out, n, "%llu-%llx", (unsigned long long)process_id(),
	               (unsigned long long)(now_ns() ^ ((uintptr_t)s << 7)) & 0xffffffffull);
}

/*!
 * The protocol for one blob, on the calling thread. @p b = the table slot
 * that receives progress (may be NULL).
 */
static enum conv_store_blob_result
blob_protocol(struct conv_model_store *s,
              const char *sha,
              uint64_t size,
              const char *url,
              const struct conv_pack_auth *auth,
              bool skip_stamped,
              bool allow_downloads,
              struct blob *b)
{
	char blob[CONV_STORE_PATH_MAX], stamp[CONV_STORE_PATH_MAX];
	blob_paths(s, sha, blob, stamp);
	struct fs_stat st;
	fs_stat(blob, &st);
	bool tampered = false;
	if (st.exists) {
		const bool stamped = fs_exists(stamp);
		if (st.size == size) {
			if (skip_stamped && stamped) {
				return CONV_STORE_BLOB_SKIPPED;
			}
			if (b != NULL) {
				os_mutex_lock(&s->mtx);
				b->st = B_VERIFYING;
				os_mutex_unlock(&s->mtx);
			}
			const enum conv_store_blob_result r = check_present(s, sha, size, blob, stamp);
			if (r != CONV_STORE_BLOB_TAMPERED) {
				return r;
			}
			tampered = true;
		} else {
			// Wrong length: never a model. A stamp beside it is a planted / truncated blob.
			U_LOG_W("[conv-store] blob %.12s… is %llu bytes, expected %llu: rejected", sha,
			        (unsigned long long)st.size, (unsigned long long)size);
			(void)fs_remove(stamp);
			if (!fs_remove(blob)) {
				return CONV_STORE_BLOB_TAMPERED_LOCKED;
			}
			tampered = true;
		}
	}
	if (!allow_downloads) {
		return tampered ? CONV_STORE_BLOB_TAMPERED : CONV_STORE_BLOB_POLICY;
	}
	if (url == NULL || url[0] == '\0') {
		return CONV_STORE_BLOB_NETWORK;
	}
	if (!conv_model_store_ensure_root(s)) {
		return CONV_STORE_BLOB_IO;
	}
	if (b != NULL) {
		os_mutex_lock(&s->mtx);
		b->st = B_DOWNLOADING;
		b->done = 0;
		os_mutex_unlock(&s->mtx);
	}

	char nonce[64], name[CONV_STORE_SHA_HEX + 72], part[CONV_STORE_PATH_MAX];
	nonce_of(s, nonce, sizeof(nonce));
	(void)snprintf(name, sizeof(name), "%s.%s.part", sha, nonce);
	join(part, sizeof(part), s->blobs_dir, name);
	struct sink k;
	memset(&k, 0, sizeof(k));
	k.limit = size;
	k.f = fs_open(part, "wb");
	if (k.f == NULL) {
		U_LOG_W("[conv-store] cannot write %s", part);
		return CONV_STORE_BLOB_IO;
	}
	u_sha256_init(&k.h);
	struct xfer x = {s, b, sha, size, 0, 0};
	xfer_progress(&x, 0, false);
	enum conv_store_blob_result r = fetch_url(s, url, auth, &k, &x);
	const bool closed = fclose(k.f) == 0;
	if (r == CONV_STORE_BLOB_OK && !closed) {
		r = CONV_STORE_BLOB_IO;
	}
	if (r == CONV_STORE_BLOB_OK) {
		xfer_progress(&x, k.written, true);
		// Size first, then the hash (the browser contract's order).
		uint8_t d[32];
		u_sha256_final(&k.h, d);
		char hex[CONV_STORE_SHA_HEX];
		for (int i = 0; i < 32; i++) {
			(void)snprintf(hex + i * 2, 3, "%02x", d[i]);
		}
		if (k.written != size || strcmp(hex, sha) != 0) {
			U_LOG_W("[conv-store] download of %.12s… failed verification (%llu of %llu bytes%s)", sha,
			        (unsigned long long)k.written, (unsigned long long)size,
			        k.written == size ? ", wrong hash" : "");
			r = CONV_STORE_BLOB_BAD_DOWNLOAD;
		}
	}
	if (r != CONV_STORE_BLOB_OK) {
		(void)fs_remove(part);
		return r;
	}
	const int rn = fs_rename_noreplace(part, blob);
	if (rn == 1) {
		// Another writer won: theirs, after the check.
		(void)fs_remove(part);
		return check_present(s, sha, size, blob, stamp);
	}
	if (rn != 0) {
		(void)fs_remove(part);
		return CONV_STORE_BLOB_IO;
	}
	if (!write_stamp(stamp, sha, size)) {
		return CONV_STORE_BLOB_IO;
	}
	remember_verified(s, sha, size, blob);
	return CONV_STORE_BLOB_OK;
}

static uint32_t
fail_of(enum conv_store_blob_result r)
{
	switch (r) {
	case CONV_STORE_BLOB_NETWORK: return XRT_CONV_FAIL_NETWORK;
	// A missing / refused credential is fixed by the user adding it; a new
	// request retries, like a network failure (ADR-057 §5).
	case CONV_STORE_BLOB_AUTH: return XRT_CONV_FAIL_NETWORK;
	case CONV_STORE_BLOB_IO: return XRT_CONV_FAIL_LOAD;
	default: return XRT_CONV_FAIL_MODEL;
	}
}

static void
record_pack(struct conv_model_store *s, const struct conv_pack *p)
{
	char name[96], path[CONV_STORE_PATH_MAX];
	(void)snprintf(name, sizeof(name), "%s.models.json", p->pack);
	join(path, sizeof(path), s->packs_dir, name);
	if (fs_exists(path)) {
		return;
	}
	(void)fs_mkdir(s->packs_dir);
	char *text = p->manifest_path[0] != '\0' ? fs_read_all(p->manifest_path, 4u * 1024u * 1024u) : NULL;
	if (text != NULL) {
		(void)fs_write_text(path, text);
		free(text);
		return;
	}
	char minimal[256];
	(void)snprintf(minimal, sizeof(minimal), "{ \"schema\": 2, \"pack\": \"%s\", \"provider\": \"%s\" }\n", p->pack,
	               p->provider);
	(void)fs_write_text(path, minimal);
}


/*
 *
 * Worker.
 *
 */

static void *
worker_main(void *ptr)
{
	struct conv_model_store *s = (struct conv_model_store *)ptr;
	os_mutex_lock(&s->mtx);
	while (!s->stop) {
		struct blob *job = NULL;
		for (uint32_t i = 0; i < CONV_STORE_MAX_BLOBS; i++) {
			if (s->blobs[i].used && s->blobs[i].st == B_QUEUED) {
				job = &s->blobs[i];
				break;
			}
		}
		if (job == NULL) {
			os_cond_broadcast(&s->cond); // idle waiters
			os_cond_wait(&s->cond, &s->mtx);
			continue;
		}
		char sha[CONV_STORE_SHA_HEX], url[CONV_STORE_PATH_MAX];
		cpy(sha, sizeof(sha), job->sha);
		cpy(url, sizeof(url), job->url);
		const struct conv_pack_auth auth = job->auth;
		const uint64_t size = job->size;
		const bool allow = s->cfg.allow_downloads;
		const uint32_t mask = job->pack_mask;
		job->st = B_VERIFYING;
		s->busy++;
		os_mutex_unlock(&s->mtx);

		const struct conv_pack_auth *ap = auth.kind != CONV_PACK_AUTH_NONE ? &auth : NULL;
		enum conv_store_blob_result r = blob_protocol(s, sha, size, url, ap, false, allow, job);
		if (r == CONV_STORE_BLOB_TAMPERED && allow) {
			r = blob_protocol(s, sha, size, url, ap, false, allow, job); // re-fetch once
		}
		if (r == CONV_STORE_BLOB_OK) {
			for (uint32_t p = 0; p < s->pack_count; p++) {
				if ((mask & (1u << p)) != 0) {
					record_pack(s, &s->packs[p]);
				}
			}
		}

		os_mutex_lock(&s->mtx);
		s->busy--;
		if (r == CONV_STORE_BLOB_OK) {
			job->st = B_READY;
			job->done = size;
			job->fail = 0;
		} else if (r == CONV_STORE_BLOB_POLICY) {
			job->st = B_REFUSED;
		} else if (r == CONV_STORE_BLOB_CANCELLED) {
			job->st = B_IDLE;
		} else {
			job->st = B_FAILED;
			job->fail = fail_of(r);
			U_LOG_W("[conv-store] blob %.12s…: %s", sha, conv_store_blob_result_str(r));
		}
	}
	os_cond_broadcast(&s->cond);
	os_mutex_unlock(&s->mtx);
	return NULL;
}

bool
conv_model_store_create(const struct conv_model_store_config *cfg, struct conv_model_store **out)
{
	*out = NULL;
	struct conv_model_store *s = calloc(1, sizeof(*s));
	if (s == NULL) {
		return false;
	}
	s->packs = calloc(CONV_STORE_MAX_PACKS, sizeof(*s->packs));
	if (s->packs == NULL) {
		free(s);
		return false;
	}
	if (cfg != NULL) {
		s->cfg = *cfg;
	} else {
		conv_model_store_config_defaults(&s->cfg);
	}
	if (s->cfg.root[0] == '\0') {
		(void)conv_store_root_resolve(s->cfg.root, sizeof(s->cfg.root), NULL, 0);
	}
	join(s->blobs_dir, sizeof(s->blobs_dir), s->cfg.root, "blobs");
	join(s->packs_dir, sizeof(s->packs_dir), s->cfg.root, "packs");
	if (os_mutex_init(&s->mtx) != 0 || os_cond_init(&s->cond) != 0 || os_thread_init(&s->thread) != 0) {
		free(s->packs);
		free(s);
		return false;
	}
	if (os_thread_start(&s->thread, worker_main, s) != 0) {
		os_thread_destroy(&s->thread);
		os_cond_destroy(&s->cond);
		os_mutex_destroy(&s->mtx);
		free(s->packs);
		free(s);
		return false;
	}
	s->thread_started = true;
	*out = s;
	return true;
}

void
conv_model_store_destroy(struct conv_model_store **ps)
{
	if (ps == NULL || *ps == NULL) {
		return;
	}
	struct conv_model_store *s = *ps;
	*ps = NULL;
	os_mutex_lock(&s->mtx);
	s->stop = true;
	os_cond_broadcast(&s->cond);
	os_mutex_unlock(&s->mtx);
	if (s->thread_started) {
		os_thread_join(&s->thread);
		os_thread_destroy(&s->thread);
	}
	os_cond_destroy(&s->cond);
	os_mutex_destroy(&s->mtx);
	free(s->packs);
	free(s);
}

void
conv_model_store_set_policy(struct conv_model_store *s, bool allow_downloads, bool allow_research_only)
{
	if (s == NULL) {
		return;
	}
	os_mutex_lock(&s->mtx);
	const bool changed = s->cfg.allow_downloads != allow_downloads ||
	                     s->cfg.allow_research_only != allow_research_only;
	s->cfg.allow_downloads = allow_downloads;
	s->cfg.allow_research_only = allow_research_only;
	if (changed) {
		for (uint32_t i = 0; i < CONV_STORE_MAX_BLOBS; i++) {
			if (s->blobs[i].used && s->blobs[i].st == B_REFUSED) {
				s->blobs[i].st = B_IDLE;
			}
		}
	}
	os_mutex_unlock(&s->mtx);
}

bool
conv_model_store_add_pack(struct conv_model_store *s, const struct conv_pack *p)
{
	if (s == NULL || p == NULL || p->pack[0] == '\0') {
		return false;
	}
	os_mutex_lock(&s->mtx);
	uint32_t idx = s->pack_count;
	for (uint32_t i = 0; i < s->pack_count; i++) {
		if (strcmp(s->packs[i].pack, p->pack) == 0) {
			idx = i;
			break;
		}
	}
	if (idx >= CONV_STORE_MAX_PACKS) {
		os_mutex_unlock(&s->mtx);
		U_LOG_W("[conv-store] more than %d packs: '%s' ignored", CONV_STORE_MAX_PACKS, p->pack);
		return false;
	}
	s->packs[idx] = *p;
	if (idx == s->pack_count) {
		s->pack_count++;
	}
	// Index every blob the pack references (URL + which packs use it).
	for (uint32_t m = 0; m < p->model_count; m++) {
		for (uint32_t v = 0; v < p->models[m].variant_count; v++) {
			const struct conv_pack_variant *pv = &p->models[m].variants[v];
			for (uint32_t f = 0; f < pv->file_count; f++) {
				struct blob *b = blob_get_locked(s, pv->files[f].sha256, pv->files[f].size, true);
				if (b == NULL) {
					continue;
				}
				b->pack_mask |= 1u << idx;
				if (b->url[0] == '\0') {
					conv_pack_file_url(p, &p->models[m], &pv->files[f], b->url, sizeof(b->url));
					const struct conv_pack_auth *a = file_auth(&p->models[m], &pv->files[f]);
					if (a != NULL) {
						b->auth = *a;
					}
				}
			}
		}
	}
	os_mutex_unlock(&s->mtx);
	return true;
}

bool
conv_model_store_add_pack_file(struct conv_model_store *s, const char *path)
{
	struct conv_pack *p = calloc(1, sizeof(*p));
	if (p == NULL) {
		return false;
	}
	char err[160];
	bool ok = conv_pack_load(path, p, err, sizeof(err));
	if (!ok) {
		U_LOG_W("[conv-store] pack manifest %s rejected: %s", path, err);
	} else {
		ok = conv_model_store_add_pack(s, p);
	}
	free(p);
	return ok;
}

uint32_t
conv_model_store_pack_count(struct conv_model_store *s)
{
	if (s == NULL) {
		return 0;
	}
	os_mutex_lock(&s->mtx);
	const uint32_t n = s->pack_count;
	os_mutex_unlock(&s->mtx);
	return n;
}

bool
conv_model_store_get_pack(struct conv_model_store *s, uint32_t i, struct conv_pack *out)
{
	if (s == NULL) {
		return false;
	}
	os_mutex_lock(&s->mtx);
	const bool ok = i < s->pack_count;
	if (ok) {
		*out = s->packs[i];
	}
	os_mutex_unlock(&s->mtx);
	return ok;
}


/*
 *
 * Ref lookup.
 *
 */

struct ref_files
{
	uint32_t count;
	const struct conv_pack_file *files[CONV_STORE_MAX_FILES];
	const struct conv_pack_model *model; //!< for the licence gate (NULL: none found)
	uint32_t main;                       //!< index of the "model" role file
	struct conv_pack_file sha_only;      //!< storage for a bare-sha ref
};

//! Resolve @p ref to its files. mtx held.
static bool
ref_lookup_locked(struct conv_model_store *s, const char *ref, struct ref_files *out)
{
	memset(out, 0, sizeof(*out));
	if (ref == NULL || ref[0] == '\0') {
		return false;
	}
	if (is_sha_hex(ref)) {
		for (uint32_t p = 0; p < s->pack_count; p++) {
			for (uint32_t m = 0; m < s->packs[p].model_count; m++) {
				const struct conv_pack_model *pm = &s->packs[p].models[m];
				for (uint32_t v = 0; v < pm->variant_count; v++) {
					for (uint32_t f = 0; f < pm->variants[v].file_count; f++) {
						if (strcmp(pm->variants[v].files[f].sha256, ref) == 0) {
							out->count = 1;
							out->files[0] = &pm->variants[v].files[f];
							out->model = pm;
							return true;
						}
					}
				}
			}
		}
		return false;
	}
	char buf[256];
	cpy(buf, sizeof(buf), ref);
	char *model = strchr(buf, '/');
	if (model == NULL) {
		return false;
	}
	*model++ = '\0';
	char *variant = strchr(model, '/');
	if (variant != NULL) {
		*variant++ = '\0';
	}
	for (uint32_t p = 0; p < s->pack_count; p++) {
		if (strcmp(s->packs[p].pack, buf) != 0) {
			continue;
		}
		const struct conv_pack_model *pm = conv_pack_find_model(&s->packs[p], model);
		if (pm == NULL) {
			return false;
		}
		const struct conv_pack_variant *pv = NULL;
		for (uint32_t v = 0; v < pm->variant_count; v++) {
			if (variant == NULL || strcmp(pm->variants[v].id, variant) == 0) {
				pv = &pm->variants[v];
				break;
			}
		}
		if (pv == NULL) {
			return false;
		}
		out->model = pm;
		bool have_main = false;
		for (uint32_t f = 0; f < pv->file_count; f++) {
			out->files[out->count] = &pv->files[f];
			if (!have_main && strcmp(pv->files[f].role, "model") == 0) {
				out->main = out->count;
				have_main = true;
			}
			out->count++;
		}
		return out->count > 0;
	}
	return false;
}

//! Disk + cache state of one blob for resolve. mtx held (stat only).
static uint32_t
blob_state_locked(struct conv_model_store *s, struct blob *b, const struct conv_pack_file *f, uint64_t *done)
{
	*done = 0;
	if (b != NULL) {
		switch (b->st) {
		case B_QUEUED:
		case B_DOWNLOADING: *done = b->done; return XRT_CONV_STATE_DOWNLOADING;
		case B_VERIFYING: *done = f->size; return XRT_CONV_STATE_WARMING;
		case B_FAILED: return XRT_CONV_STATE_FAILED;
		case B_REFUSED: return XRT_CONV_STATE_ABSENT;
		default: break;
		}
	}
	char blob[CONV_STORE_PATH_MAX], stamp[CONV_STORE_PATH_MAX];
	blob_paths(s, f->sha256, blob, stamp);
	struct fs_stat st;
	fs_stat(blob, &st);
	if (!st.exists || st.size != f->size || !fs_exists(stamp)) {
		if (b != NULL && b->st == B_READY) {
			b->st = B_IDLE; // wiped under us (an older browser uninstaller): fetch again on request
			b->verified = false;
		}
		return XRT_CONV_STATE_ABSENT;
	}
	*done = f->size;
	if (b != NULL && b->verified && b->key_id == st.file_id && b->key_mtime == st.mtime && b->key_size == st.size) {
		return XRT_CONV_STATE_READY;
	}
	if (b != NULL && b->verified) {
		b->verified = false; // replaced since we hashed it
		if (b->st == B_READY) {
			b->st = B_IDLE;
		}
	}
	return XRT_CONV_STATE_INSTALLED;
}

static void
loc_init(struct xrt_conv_model_location *out)
{
	const uint32_t size = out->struct_size;
	if (size >= sizeof(uint32_t) * 2) {
		out->state = XRT_CONV_STATE_ABSENT;
	}
	if (XRT_CONV_COVERS(out, struct xrt_conv_model_location, progress)) {
		out->progress = -1.0f;
	}
	if (XRT_CONV_COVERS(out, struct xrt_conv_model_location, failure_reason)) {
		out->failure_reason = 0;
	}
	if (XRT_CONV_COVERS(out, struct xrt_conv_model_location, path)) {
		out->path[0] = '\0';
	}
}

static bool
policy_refuses_locked(struct conv_model_store *s, const struct ref_files *rf)
{
	return rf->model != NULL && !rf->model->commercial && !s->cfg.allow_research_only;
}

bool
conv_model_store_resolve(struct conv_model_store *s, const char *ref, struct xrt_conv_model_location *out)
{
	if (out == NULL || out->struct_size < sizeof(uint32_t) * 2) {
		return false;
	}
	loc_init(out);
	if (s == NULL) {
		if (XRT_CONV_COVERS(out, struct xrt_conv_model_location, failure_reason)) {
			out->failure_reason = XRT_CONV_FAIL_MODEL;
		}
		return false;
	}
	os_mutex_lock(&s->mtx);
	struct ref_files rf;
	if (!ref_lookup_locked(s, ref, &rf)) {
		os_mutex_unlock(&s->mtx);
		if (XRT_CONV_COVERS(out, struct xrt_conv_model_location, failure_reason)) {
			out->failure_reason = XRT_CONV_FAIL_MODEL;
		}
		return false;
	}
	// Aggregate over the files: FAILED > ABSENT(policy) > ABSENT > DOWNLOADING > WARMING > INSTALLED > READY.
	uint32_t worst = XRT_CONV_STATE_READY, fail = 0;
	bool refused = false, absent = false, downloading = false, warming = false, installed = false;
	uint64_t done = 0, total = 0;
	for (uint32_t i = 0; i < rf.count; i++) {
		struct blob *b = blob_get_locked(s, rf.files[i]->sha256, rf.files[i]->size, false);
		uint64_t d = 0;
		const uint32_t st = blob_state_locked(s, b, rf.files[i], &d);
		done += d;
		total += rf.files[i]->size;
		switch (st) {
		case XRT_CONV_STATE_FAILED:
			worst = XRT_CONV_STATE_FAILED;
			fail = b != NULL ? b->fail : XRT_CONV_FAIL_MODEL;
			break;
		case XRT_CONV_STATE_ABSENT:
			absent = true;
			refused |= b != NULL && b->st == B_REFUSED;
			break;
		case XRT_CONV_STATE_DOWNLOADING: downloading = true; break;
		case XRT_CONV_STATE_WARMING: warming = true; break;
		case XRT_CONV_STATE_INSTALLED: installed = true; break;
		default: break;
		}
	}
	if (worst != XRT_CONV_STATE_FAILED) {
		worst = refused || (absent && !downloading) ? XRT_CONV_STATE_ABSENT
		        : downloading || absent             ? XRT_CONV_STATE_DOWNLOADING
		        : warming                           ? XRT_CONV_STATE_WARMING
		        : installed                         ? XRT_CONV_STATE_INSTALLED
		                                            : XRT_CONV_STATE_READY;
		if (worst == XRT_CONV_STATE_ABSENT && refused) {
			fail = XRT_CONV_FAIL_POLICY;
		}
	}
	// A research-only model without the opt-in is never handed out.
	if (worst == XRT_CONV_STATE_READY && policy_refuses_locked(s, &rf)) {
		worst = XRT_CONV_STATE_ABSENT;
		fail = XRT_CONV_FAIL_POLICY;
	}
	out->state = worst;
	if (XRT_CONV_COVERS(out, struct xrt_conv_model_location, progress)) {
		out->progress = worst == XRT_CONV_STATE_DOWNLOADING && total > 0 ? (float)((double)done / (double)total)
		                : worst == XRT_CONV_STATE_READY                 ? 1.0f
		                                                                : -1.0f;
	}
	if (XRT_CONV_COVERS(out, struct xrt_conv_model_location, failure_reason)) {
		out->failure_reason = fail;
	}
	if (worst == XRT_CONV_STATE_READY && XRT_CONV_COVERS(out, struct xrt_conv_model_location, path)) {
		char p[CONV_STORE_PATH_MAX];
		blob_paths(s, rf.files[rf.main]->sha256, p, NULL);
		cpy(out->path, sizeof(out->path), p);
	}
	os_mutex_unlock(&s->mtx);
	return true;
}

bool
conv_model_store_request(struct conv_model_store *s, const char *ref)
{
	if (s == NULL) {
		return false;
	}
	os_mutex_lock(&s->mtx);
	struct ref_files rf;
	if (!ref_lookup_locked(s, ref, &rf)) {
		os_mutex_unlock(&s->mtx);
		U_LOG_W("[conv-store] model_request('%s'): no registered pack has it", ref != NULL ? ref : "");
		return false;
	}
	const bool research_refused = policy_refuses_locked(s, &rf);
	bool ok = !research_refused;
	bool queued = false;
	for (uint32_t i = 0; i < rf.count; i++) {
		const struct conv_pack_file *f = rf.files[i];
		struct blob *b = blob_get_locked(s, f->sha256, f->size, true);
		if (b == NULL) {
			ok = false;
			continue;
		}
		if (research_refused) {
			b->st = B_REFUSED;
			continue;
		}
		uint64_t d = 0;
		const uint32_t st = blob_state_locked(s, b, f, &d);
		if (st == XRT_CONV_STATE_READY || b->st == B_QUEUED || b->st == B_DOWNLOADING || b->st == B_VERIFYING) {
			continue; // ready, or one job already serves it
		}
		if (st == XRT_CONV_STATE_ABSENT && !s->cfg.allow_downloads) {
			b->st = B_REFUSED;
			ok = false;
			continue;
		}
		b->st = B_QUEUED;
		b->done = 0;
		queued = true;
	}
	if (queued) {
		os_cond_broadcast(&s->cond);
	}
	os_mutex_unlock(&s->mtx);
	if (research_refused) {
		U_LOG_W("[conv-store] model_request('%s') refused: research-only model and AllowResearchOnly is off", ref);
	} else if (!ok) {
		U_LOG_W("[conv-store] model_request('%s') refused: downloads are off by policy", ref);
	}
	return ok;
}

bool
conv_model_store_wait_idle(struct conv_model_store *s, uint32_t timeout_ms)
{
	if (s == NULL) {
		return true;
	}
	const uint64_t deadline = now_ns() + (uint64_t)timeout_ms * 1000000ull;
	os_mutex_lock(&s->mtx);
	for (;;) {
		bool pending = s->busy > 0;
		for (uint32_t i = 0; !pending && i < CONV_STORE_MAX_BLOBS; i++) {
			pending = s->blobs[i].used && s->blobs[i].st == B_QUEUED;
		}
		const uint64_t t = now_ns();
		if (!pending || t >= deadline || s->stop) {
			os_mutex_unlock(&s->mtx);
			return !pending;
		}
		const uint64_t left = deadline - t;
		(void)os_cond_wait_timeout_ns(&s->cond, &s->mtx, left < 50000000ull ? left : 50000000ull);
	}
}


/*
 *
 * Synchronous operations.
 *
 */

enum conv_store_blob_result
conv_model_store_fetch_model(struct conv_model_store *s,
                             const struct conv_pack *p,
                             const struct conv_pack_model *m,
                             const char *variant,
                             bool skip_stamped)
{
	if (s == NULL || p == NULL || m == NULL) {
		return CONV_STORE_BLOB_IO;
	}
	os_mutex_lock(&s->mtx);
	const bool allow = s->cfg.allow_downloads;
	const bool research_ok = s->cfg.allow_research_only;
	os_mutex_unlock(&s->mtx);
	if (!m->commercial && !research_ok) {
		return CONV_STORE_BLOB_POLICY;
	}
	enum conv_store_blob_result worst = CONV_STORE_BLOB_OK;
	bool all_skipped = true;
	for (uint32_t v = 0; v < m->variant_count; v++) {
		const struct conv_pack_variant *pv = &m->variants[v];
		if (variant != NULL && strcmp(variant, pv->id) != 0) {
			continue;
		}
		for (uint32_t f = 0; f < pv->file_count; f++) {
			char url[CONV_STORE_PATH_MAX];
			conv_pack_file_url(p, m, &pv->files[f], url, sizeof(url));
			const struct conv_pack_auth *auth = file_auth(m, &pv->files[f]);
			enum conv_store_blob_result r = blob_protocol(s, pv->files[f].sha256, pv->files[f].size, url,
			                                              auth, skip_stamped, allow, NULL);
			if (r == CONV_STORE_BLOB_TAMPERED && allow) {
				r = blob_protocol(s, pv->files[f].sha256, pv->files[f].size, url, auth, skip_stamped,
				                  allow, NULL);
			}
			all_skipped = all_skipped && r == CONV_STORE_BLOB_SKIPPED;
			if (r != CONV_STORE_BLOB_OK && r != CONV_STORE_BLOB_SKIPPED && worst == CONV_STORE_BLOB_OK) {
				worst = r;
			}
		}
	}
	if (worst == CONV_STORE_BLOB_OK) {
		(void)conv_model_store_ensure_root(s);
		record_pack(s, p);
		return all_skipped ? CONV_STORE_BLOB_SKIPPED : CONV_STORE_BLOB_OK;
	}
	return worst;
}

enum conv_store_blob_result
conv_model_store_verify_blob(struct conv_model_store *s, const char *sha256, uint64_t size)
{
	if (s == NULL || !is_sha_hex(sha256)) {
		return CONV_STORE_BLOB_IO;
	}
	char blob[CONV_STORE_PATH_MAX], stamp[CONV_STORE_PATH_MAX];
	blob_paths(s, sha256, blob, stamp);
	struct fs_stat st;
	fs_stat(blob, &st);
	if (!st.exists) {
		return CONV_STORE_BLOB_MISSING;
	}
	if (st.size != size) {
		(void)fs_remove(stamp);
		return fs_remove(blob) ? CONV_STORE_BLOB_TAMPERED : CONV_STORE_BLOB_TAMPERED_LOCKED;
	}
	return check_present(s, sha256, size, blob, stamp);
}

struct prune_ctx
{
	struct conv_model_store *s;
	uint32_t deleted;
	uint64_t bytes;
};

//! Is @p sha referenced by any pack? mtx held.
static bool
referenced_locked(struct conv_model_store *s, const char *sha)
{
	for (uint32_t i = 0; i < CONV_STORE_MAX_BLOBS; i++) {
		if (s->blobs[i].used && s->blobs[i].pack_mask != 0 && strcmp(s->blobs[i].sha, sha) == 0) {
			return true;
		}
	}
	return false;
}

static void
prune_fn(void *user, const char *name, uint64_t size)
{
	struct prune_ctx *c = (struct prune_ctx *)user;
	char sha[CONV_STORE_SHA_HEX];
	cpy(sha, sizeof(sha), name);
	if (!is_sha_hex(sha)) {
		return; // not ours
	}
	os_mutex_lock(&c->s->mtx);
	const bool keep = referenced_locked(c->s, sha);
	os_mutex_unlock(&c->s->mtx);
	if (keep) {
		return;
	}
	char p[CONV_STORE_PATH_MAX];
	join(p, sizeof(p), c->s->blobs_dir, name);
	if (fs_remove(p) && strlen(name) == 64) {
		c->deleted++;
		c->bytes += size;
	}
}

uint32_t
conv_model_store_prune(struct conv_model_store *s, uint64_t *bytes_out)
{
	struct prune_ctx c = {s, 0, 0};
	if (s != NULL) {
		fs_list(s->blobs_dir, prune_fn, &c);
	}
	if (bytes_out != NULL) {
		*bytes_out = c.bytes;
	}
	return c.deleted;
}


/*
 *
 * Per-pack summary.
 *
 */

static bool
file_on_disk(struct conv_model_store *s, const struct conv_pack_file *f)
{
	char blob[CONV_STORE_PATH_MAX], stamp[CONV_STORE_PATH_MAX];
	blob_paths(s, f->sha256, blob, stamp);
	struct fs_stat st;
	fs_stat(blob, &st);
	return st.exists && st.size == f->size && fs_exists(stamp);
}

static bool
model_on_disk(struct conv_model_store *s, const struct conv_pack_model *m)
{
	for (uint32_t v = 0; v < m->variant_count; v++) {
		bool all = true;
		for (uint32_t f = 0; f < m->variants[v].file_count && all; f++) {
			all = file_on_disk(s, &m->variants[v].files[f]);
		}
		if (all) {
			return true;
		}
	}
	return false;
}

bool
conv_model_store_pack_summary(struct conv_model_store *s, uint32_t i, struct conv_store_pack_summary *out)
{
	memset(out, 0, sizeof(*out));
	struct conv_pack *p = calloc(1, sizeof(*p));
	if (p == NULL || !conv_model_store_get_pack(s, i, p)) {
		free(p);
		return false;
	}
	cpy(out->pack, sizeof(out->pack), p->pack);
	cpy(out->provider, sizeof(out->provider), p->provider);
	out->models = p->model_count;
	bool any_prefetch = false;
	for (uint32_t m = 0; m < p->model_count; m++) {
		any_prefetch |= p->models[m].prefetch;
	}
	bool default_installed = true;
	bool downloading = false;
	// Distinct blobs (variants may share them).
	const char *seen[CONV_STORE_MAX_MODELS * CONV_STORE_MAX_VARIANTS * CONV_STORE_MAX_FILES];
	uint32_t seen_n = 0;
	for (uint32_t m = 0; m < p->model_count; m++) {
		const struct conv_pack_model *pm = &p->models[m];
		const bool on = model_on_disk(s, pm);
		out->models_installed += on ? 1u : 0u;
		out->research_models += pm->commercial ? 0u : 1u;
		if (!any_prefetch || pm->prefetch) {
			default_installed = default_installed && on;
		}
		for (uint32_t v = 0; v < pm->variant_count; v++) {
			for (uint32_t f = 0; f < pm->variants[v].file_count; f++) {
				const struct conv_pack_file *pf = &pm->variants[v].files[f];
				bool dup = false;
				for (uint32_t k = 0; k < seen_n && !dup; k++) {
					dup = strcmp(seen[k], pf->sha256) == 0;
				}
				if (dup) {
					continue;
				}
				seen[seen_n++] = pf->sha256;
				out->bytes_total += pf->size;
				if (file_on_disk(s, pf)) {
					out->bytes_installed += pf->size;
					continue;
				}
				os_mutex_lock(&s->mtx);
				struct blob *b = blob_get_locked(s, pf->sha256, pf->size, false);
				const bool live = b != NULL && (b->st == B_QUEUED || b->st == B_DOWNLOADING);
				const uint64_t done = live ? b->done : 0;
				os_mutex_unlock(&s->mtx);
				uint64_t part = 0;
				if (!live) {
					struct conv_store_blob_disk d;
					conv_model_store_blob_disk(s, pf->sha256, &d);
					part = d.part_bytes;
				}
				if (live || part > 0) {
					downloading = true;
					out->download_done += live ? done : part;
					out->download_total += pf->size;
				}
			}
		}
	}
	char name[96], rec[CONV_STORE_PATH_MAX];
	(void)snprintf(name, sizeof(name), "%s.models.json", p->pack);
	join(rec, sizeof(rec), s->packs_dir, name);
	const bool recorded = fs_exists(rec);
	const char *state = downloading                  ? "downloading"
	                    : default_installed          ? "installed"
	                    : recorded                   ? "evicted"
	                    : out->bytes_installed > 0   ? "partial"
	                                                 : "absent";
	cpy(out->state, sizeof(out->state), state);
	free(p);
	return true;
}


/*
 *
 * Host glue.
 *
 */

static struct
{
	bool init;
	struct os_mutex mtx;
	struct conv_model_store *store;
} g_host_store;

#ifdef XRT_OS_WINDOWS
static INIT_ONCE g_host_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK
host_once_fn(PINIT_ONCE o, PVOID p, PVOID *c)
{
	(void)o;
	(void)p;
	(void)c;
	g_host_store.init = os_mutex_init(&g_host_store.mtx) == 0;
	return TRUE;
}
#else
#include <pthread.h>
static pthread_once_t g_host_once = PTHREAD_ONCE_INIT;
static void
host_once_fn(void)
{
	g_host_store.init = os_mutex_init(&g_host_store.mtx) == 0;
}
#endif

static bool
host_lock(void)
{
#ifdef XRT_OS_WINDOWS
	(void)InitOnceExecuteOnce(&g_host_once, host_once_fn, NULL, NULL);
#else
	(void)pthread_once(&g_host_once, host_once_fn);
#endif
	if (!g_host_store.init) {
		return false;
	}
	os_mutex_lock(&g_host_store.mtx);
	return true;
}

static bool
is_absolute(const char *p)
{
	return p[0] == '/' || p[0] == '\\' || (p[0] != '\0' && p[1] == ':');
}

void
conv_store_pack_path(const char *pack, const char *binary_path, char *out, size_t out_size)
{
	if (pack == NULL || pack[0] == '\0') {
		cpy(out, out_size, "");
		return;
	}
	if (is_absolute(pack) || binary_path == NULL || binary_path[0] == '\0') {
		cpy(out, out_size, pack);
		return;
	}
	char dir[CONV_STORE_PATH_MAX];
	dirname_of(binary_path, dir, sizeof(dir));
	join(out, out_size, dir, pack);
}

void
conv_store_host_configure(const char *const *pack_paths,
                          uint32_t pack_count,
                          bool allow_downloads,
                          bool allow_research_only)
{
	if (!host_lock()) {
		return;
	}
	if (g_host_store.store == NULL) {
		struct conv_model_store_config cfg;
		conv_model_store_config_defaults(&cfg);
		char source[64];
		if (!conv_store_root_resolve(cfg.root, sizeof(cfg.root), source, sizeof(source))) {
			U_LOG_W("[conv-store] no model store root on this machine");
		} else {
			U_LOG_W("[conv-store] model store root: %s (%s)", cfg.root, source);
		}
		cfg.allow_downloads = allow_downloads;
		cfg.allow_research_only = allow_research_only;
		const char *throttle = getenv("DXR_CONV_STORE_THROTTLE_MS");
		if (throttle != NULL) {
			cfg.throttle_ms = (uint32_t)strtoul(throttle, NULL, 10);
			cfg.chunk_bytes = cfg.throttle_ms > 0 ? 4096u : 0u;
		}
		if (!conv_model_store_create(&cfg, &g_host_store.store)) {
			U_LOG_W("[conv-store] could not create the model store");
		}
	} else {
		conv_model_store_set_policy(g_host_store.store, allow_downloads, allow_research_only);
	}
	for (uint32_t i = 0; g_host_store.store != NULL && i < pack_count; i++) {
		if (pack_paths[i] != NULL && pack_paths[i][0] != '\0') {
			(void)conv_model_store_add_pack_file(g_host_store.store, pack_paths[i]);
		}
	}
	os_mutex_unlock(&g_host_store.mtx);
}

struct conv_model_store *
conv_store_host_get(void)
{
	if (!host_lock()) {
		return NULL;
	}
	struct conv_model_store *s = g_host_store.store;
	os_mutex_unlock(&g_host_store.mtx);
	return s;
}

bool
conv_store_host_model_resolve(const char *ref, struct xrt_conv_model_location *out)
{
	return conv_model_store_resolve(conv_store_host_get(), ref, out);
}

bool
conv_store_host_model_request(const char *ref)
{
	return conv_model_store_request(conv_store_host_get(), ref);
}

void
conv_store_host_shutdown(void)
{
	if (!host_lock()) {
		return;
	}
	conv_model_store_destroy(&g_host_store.store);
	os_mutex_unlock(&g_host_store.mtx);
}
