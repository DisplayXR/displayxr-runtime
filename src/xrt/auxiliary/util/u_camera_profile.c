// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
#include "u_camera_profile.h"
#include "u_file.h"
#include "u_json.h"
#include "xrt/xrt_config_os.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef XRT_OS_WINDOWS
#include "xrt/xrt_windows.h"
#elif defined(XRT_OS_MACOS)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif

#define PROFILE_MAX_BYTES (64 * 1024)
#define PROFILE_PATH_SIZE 8192

static bool
invalid(char *error, size_t size, const char *reason)
{
	if (error != NULL && size > 0)
		snprintf(error, size, "%s", reason);
	return false;
}

static double
bounded(double value, double low, double high)
{
	return value < low ? low : value > high ? high : value;
}

//! cJSON accepts some non-JSON numbers and truncates decoded strings at an
//! escaped NUL. Check those lexical cases before its object/schema validation.
//! This is not a second JSON parser: cJSON still validates the complete shape.
static bool
valid_tokens(const char *text)
{
	const unsigned char *p = (const unsigned char *)text;
	while (*p) {
		if (*p == '"') {
			p++;
			while (*p && *p != '"') {
				if (*p == '\\') {
					if (!p[1])
						return false;
					if (p[1] == 'u') {
						bool zero = true;
						for (int i = 2; i < 6; i++) {
							unsigned char c = p[i];
							if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
							      (c >= 'A' && c <= 'F')))
								return false;
							zero &= c == '0';
						}
						if (zero)
							return false;
					}
					p += 2;
				} else {
					if (*p < 0x20)
						return false;
					p++;
				}
			}
			if (!*p)
				return false;
			p++;
			continue;
		}
		if (*p == '-' || *p == '+' || *p == '.' || (*p >= '0' && *p <= '9')) {
			if (*p == '-')
				p++;
			if (*p == '0')
				p++;
			else {
				if (*p < '1' || *p > '9')
					return false;
				do {
					p++;
				} while (*p >= '0' && *p <= '9');
			}
			if (*p == '.') {
				p++;
				if (*p < '0' || *p > '9')
					return false;
				do {
					p++;
				} while (*p >= '0' && *p <= '9');
			}
			if (*p == 'e' || *p == 'E') {
				p++;
				if (*p == '+' || *p == '-')
					p++;
				if (*p < '0' || *p > '9')
					return false;
				do {
					p++;
				} while (*p >= '0' && *p <= '9');
			}
			if (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\t' && *p != '\r' &&
			    *p != '\n')
				return false;
			continue;
		}
		if (*p < 0x20 && *p != '\t' && *p != '\r' && *p != '\n')
			return false;
		p++;
	}
	return true;
}

bool
u_camera_profile_parse(const char *json, float aspect, struct u_camera_profile *out, char *error, size_t size)
{
	if (json == NULL || out == NULL || strlen(json) > PROFILE_MAX_BYTES)
		return invalid(error, size, "missing or oversized camera profile");
	if (!valid_tokens(json))
		return invalid(error, size, "invalid JSON number, control character or NUL escape");
	const char *end = NULL;
	cJSON *root = cJSON_ParseWithOpts(json, &end, true);
	if (!cJSON_IsObject(root)) {
		cJSON_Delete(root);
		return invalid(error, size, "camera profile must be one JSON object");
	}
	struct u_camera_profile result = U_CAMERA_PROFILE_DEFAULT;
	unsigned seen = 0;
	bool ok = true;
	const char *reason = "invalid camera profile";
	const cJSON *item;
	cJSON_ArrayForEach(item, root)
	{
		const char *key = item->string;
		unsigned bit = 0;
		if (strcmp(key, "ipdFactor") == 0)
			bit = 1;
		else if (strcmp(key, "parallaxFactor") == 0)
			bit = 2;
		else if (strcmp(key, "convergenceDiopters") == 0 || strcmp(key, "convergenceMeters") == 0)
			bit = 4;
		else if (strcmp(key, "verticalFov") == 0 || strcmp(key, "horizontalFovDeg") == 0)
			bit = 8;
		else if (strcmp(key, "metersToVirtual") == 0 || strcmp(key, "renderedBaselineMeters") == 0)
			bit = 16;
		if (bit == 0 || (seen & bit)) {
			ok = false;
			reason = "unknown, duplicate or conflicting camera profile key";
			break;
		}
		seen |= bit;
		if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble)) {
			ok = false;
			reason = "camera profile values must be finite numbers";
			break;
		}
		double value = item->valuedouble;
		if (bit == 1)
			result.ipd_factor = (float)bounded(value, 0, 1.0e4);
		else if (bit == 2)
			result.parallax_factor = (float)bounded(value, 0, 1.0e4);
		else if (bit == 4) {
			if (strcmp(key, "convergenceMeters") == 0) {
				if (value <= 0) {
					ok = false;
					reason = "convergenceMeters must be positive";
					break;
				}
				value = 1.0 / value;
			}
			result.inv_convergence_distance = (float)bounded(value, 0, 20);
		} else if (bit == 8) {
			if (strcmp(key, "horizontalFovDeg") == 0) {
				if (!(aspect > 0) || !isfinite(aspect)) {
					ok = false;
					reason = "horizontalFovDeg needs valid canvas dimensions";
					break;
				}
				if (!(value > 0 && value < 180)) {
					ok = false;
					reason = "horizontalFovDeg must be between 0 and 180 degrees";
					break;
				}
				const double horizontal = value * (3.141592653589793 / 180.0);
				value = 2.0 * atan(tan(horizontal * 0.5) / aspect);
			}
			result.half_tan_vfov = (float)tan(bounded(value, 0.01, 3.13) * 0.5);
		} else {
			if (strcmp(key, "renderedBaselineMeters") == 0) {
				if (value <= 0) {
					ok = false;
					reason = "renderedBaselineMeters must be positive";
					break;
				}
				value /= 0.063;
			}
			result.m2v = value > 0 ? (float)bounded(value, 0.0001, 100000) : 1.0f;
		}
	}
	cJSON_Delete(root);
	if (!ok)
		return invalid(error, size, reason);
	*out = result;
	if (error != NULL && size > 0)
		error[0] = '\0';
	return true;
}

static FILE *
open_profile(const char *path)
{
#ifdef XRT_OS_WINDOWS
	wchar_t wide[PROFILE_PATH_SIZE];
	if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, PROFILE_PATH_SIZE) == 0) {
		errno = EINVAL;
		return NULL;
	}
	return _wfopen(wide, L"rb");
#else
	return fopen(path, "rb");
#endif
}

enum u_camera_profile_result
u_camera_profile_load(const char *exe,
                      const char *dir,
                      const char *override,
                      float aspect,
                      struct u_camera_profile *out,
                      char *error,
                      size_t size)
{
	if (error != NULL && size > 0)
		error[0] = '\0';
	bool explicit_override = override != NULL && override[0] != '\0';
	if (explicit_override) {
		const char *first = override;
		while (isspace((unsigned char)*first))
			first++;
		if (*first == '{')
			return u_camera_profile_parse(override, aspect, out, error, size) ? U_CAMERA_PROFILE_LOADED
			                                                                  : U_CAMERA_PROFILE_INVALID;
	}
	char path[PROFILE_PATH_SIZE];
	if (explicit_override) {
		if (strlen(override) >= sizeof(path)) {
			invalid(error, size, "camera profile path is too long");
			return U_CAMERA_PROFILE_INVALID;
		}
		memcpy(path, override, strlen(override) + 1);
	} else {
		if (dir == NULL || dir[0] == '\0' || exe == NULL || exe[0] == '\0')
			return U_CAMERA_PROFILE_NONE;
		if (strpbrk(exe, "/\\") != NULL || strcmp(exe, ".") == 0 || strcmp(exe, "..") == 0) {
			invalid(error, size, "camera profile needs an executable basename");
			return U_CAMERA_PROFILE_INVALID;
		}
		int n = snprintf(path, sizeof(path), "%s/%s.json", dir, exe);
		if (n < 0 || (size_t)n >= sizeof(path)) {
			invalid(error, size, "camera profile path is too long");
			return U_CAMERA_PROFILE_INVALID;
		}
	}
	FILE *file = open_profile(path);
	if (file == NULL) {
		if (!explicit_override && errno == ENOENT)
			return U_CAMERA_PROFILE_NONE;
		invalid(error, size, "cannot read camera profile file");
		return U_CAMERA_PROFILE_INVALID;
	}
	char *json = malloc(PROFILE_MAX_BYTES + 2);
	if (json == NULL) {
		fclose(file);
		invalid(error, size, "camera profile allocation failed");
		return U_CAMERA_PROFILE_INVALID;
	}
	size_t count = fread(json, 1, PROFILE_MAX_BYTES + 1, file);
	bool read_ok = !ferror(file) && count <= PROFILE_MAX_BYTES && memchr(json, '\0', count) == NULL;
	fclose(file);
	json[count] = '\0';
	bool ok = read_ok && u_camera_profile_parse(json, aspect, out, error, size);
	free(json);
	if (!read_ok)
		invalid(error, size, "camera profile file is oversized or contains invalid bytes");
	return ok ? U_CAMERA_PROFILE_LOADED : U_CAMERA_PROFILE_INVALID;
}

static bool
environment(const char *name, char *out, size_t capacity)
{
	out[0] = '\0';
#ifdef XRT_OS_WINDOWS
	wchar_t wide_name[64];
	if (!MultiByteToWideChar(CP_UTF8, 0, name, -1, wide_name, 64))
		return false;
	SetLastError(ERROR_SUCCESS);
	DWORD count = GetEnvironmentVariableW(wide_name, NULL, 0);
	if (count == 0)
		return GetLastError() == ERROR_ENVVAR_NOT_FOUND || GetLastError() == ERROR_SUCCESS;
	if (count > 32768 || count > capacity)
		return false;
	wchar_t *wide_value = malloc((size_t)count * sizeof(*wide_value));
	if (wide_value == NULL)
		return false;
	SetLastError(ERROR_SUCCESS);
	DWORD copied = GetEnvironmentVariableW(wide_name, wide_value, count);
	if (copied == 0) {
		DWORD error = GetLastError();
		free(wide_value);
		return error == ERROR_ENVVAR_NOT_FOUND || error == ERROR_SUCCESS;
	}
	bool ok = copied < count && WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide_value, -1, out,
	                                                (int)capacity, NULL, NULL) > 0;
	free(wide_value);
	return ok;
#else
	const char *value = getenv(name);
	if (value == NULL)
		return true;
	if (strlen(value) >= capacity)
		return false;
	memcpy(out, value, strlen(value) + 1);
	return true;
#endif
}

enum u_camera_profile_result
u_camera_profile_load_for_process(float aspect, struct u_camera_profile *out, char *error, size_t size)
{
	char *override = malloc(PROFILE_MAX_BYTES + 1);
	if (override == NULL) {
		invalid(error, size, "camera profile allocation failed");
		return U_CAMERA_PROFILE_INVALID;
	}
	if (!environment("DXR_LEGACY_CAMERA_RIG", override, PROFILE_MAX_BYTES + 1)) {
		free(override);
		invalid(error, size, "DXR_LEGACY_CAMERA_RIG could not be read within the supported size/encoding");
		return U_CAMERA_PROFILE_INVALID;
	}
	if (override[0] != '\0') {
		enum u_camera_profile_result result =
		    u_camera_profile_load(NULL, NULL, override, aspect, out, error, size);
		free(override);
		return result;
	}
	free(override);
	char executable[PROFILE_PATH_SIZE], dir[PROFILE_PATH_SIZE];
#ifdef XRT_OS_WINDOWS
	wchar_t wide[PROFILE_PATH_SIZE];
	DWORD count = GetModuleFileNameW(NULL, wide, PROFILE_PATH_SIZE);
	if (count == 0 || count >= PROFILE_PATH_SIZE ||
	    !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, executable, sizeof(executable), NULL, NULL)) {
		invalid(error, size, "cannot determine executable basename");
		return U_CAMERA_PROFILE_INVALID;
	}
	char root[PROFILE_PATH_SIZE];
	if (!environment("ProgramData", root, sizeof(root)) || root[0] == '\0')
		return U_CAMERA_PROFILE_NONE;
	int n = snprintf(dir, sizeof(dir), "%s/DisplayXR/app-profiles", root);
#else
#ifdef XRT_OS_MACOS
	uint32_t executable_size = sizeof(executable);
	if (_NSGetExecutablePath(executable, &executable_size) != 0) {
#else
	ssize_t count = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
	if (count <= 0 || count >= (ssize_t)sizeof(executable) - 1) {
#endif
		invalid(error, size, "cannot determine executable basename");
		return U_CAMERA_PROFILE_INVALID;
	}
#ifndef XRT_OS_MACOS
	executable[count] = '\0';
#endif
	int n = u_file_get_path_in_config_dir("app-profiles", dir, sizeof(dir));
#endif
	if (n < 0 || (size_t)n >= sizeof(dir))
		return U_CAMERA_PROFILE_NONE;
	const char *base = executable;
	for (const char *p = executable; *p; p++)
		if (*p == '/' || *p == '\\')
			base = p + 1;
	return u_camera_profile_load(base, dir, NULL, aspect, out, error, size);
}
