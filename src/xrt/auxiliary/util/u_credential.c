// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Named secrets from the OS credential store (see u_credential.h).
 * @ingroup aux_util
 */

#include "util/u_credential.h"

#include "xrt/xrt_config_os.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(XRT_OS_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincred.h>
#elif defined(XRT_OS_MACOS)
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#elif defined(XRT_OS_LINUX_DESKTOP)
#include <dlfcn.h>
#include <pthread.h>
#include <time.h>
#endif

static u_credential_reader_fn g_test_reader = NULL;

void
u_credential_set_reader_for_tests(u_credential_reader_fn fn)
{
	g_test_reader = fn;
}

void
u_credential_wipe(void *buf, size_t size)
{
	volatile uint8_t *p = (volatile uint8_t *)buf;
	while (buf != NULL && size-- > 0) {
		*p++ = 0;
	}
}

const char *
u_credential_result_str(enum u_credential_result r)
{
	switch (r) {
	case U_CREDENTIAL_OK: return "ok";
	case U_CREDENTIAL_NOT_FOUND: return "not_found";
	case U_CREDENTIAL_DENIED: return "denied";
	case U_CREDENTIAL_UNSUPPORTED: return "unsupported";
	case U_CREDENTIAL_BAD_NAME: return "bad_name";
	case U_CREDENTIAL_TOO_LONG: return "too_long";
	case U_CREDENTIAL_ERROR: return "error";
	case U_CREDENTIAL_BAD_SECRET: return "bad_secret";
	default: return "?";
	}
}

bool
u_credential_name_valid(const char *name)
{
	if (name == NULL || name[0] == '\0') {
		return false;
	}
	size_t n = 0;
	for (const unsigned char *c = (const unsigned char *)name; *c != '\0'; c++, n++) {
		if (*c < 0x20 || *c == 0x7f || n >= U_CREDENTIAL_NAME_MAX) {
			return false;
		}
	}
	return true;
}

static void
msg(char *err, size_t err_size, const char *text, const char *name)
{
	if (err != NULL && err_size > 0) {
		(void)snprintf(err, err_size, text, name != NULL ? name : "");
	}
}

bool
u_credential_secret_valid_n(const char *s, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		const unsigned char c = (unsigned char)s[i];
		if (c < 0x20 || c == 0x7f) {
			return false; // CR, LF, NUL, ...: a secret goes into a header line
		}
	}
	return n > 0;
}

//! Copy @p n secret bytes into @p out, trimming a trailing CR / LF.
static enum u_credential_result
put_secret(const uint8_t *bytes, size_t n, char *out, size_t out_size)
{
	while (n > 0 && (bytes[n - 1] == '\n' || bytes[n - 1] == '\r')) {
		n--;
	}
	if (n == 0) {
		return U_CREDENTIAL_NOT_FOUND; // an empty secret is no secret
	}
	if (n + 1 > out_size || n > U_CREDENTIAL_SECRET_MAX) {
		return U_CREDENTIAL_TOO_LONG;
	}
	if (!u_credential_secret_valid_n((const char *)bytes, n)) {
		return U_CREDENTIAL_BAD_SECRET; // would inject into an HTTP header line
	}
	memcpy(out, bytes, n);
	out[n] = '\0';
	return U_CREDENTIAL_OK;
}


/*
 *
 * Windows: Credential Manager.
 *
 */

#if defined(XRT_OS_WINDOWS)

static enum u_credential_result
os_read(const char *name, char *out, size_t out_size, char *err, size_t err_size)
{
	wchar_t wname[U_CREDENTIAL_NAME_MAX + 1];
	if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, wname, U_CREDENTIAL_NAME_MAX + 1) <= 0) {
		msg(err, err_size, "credential name '%s' is not valid UTF-8", name);
		return U_CREDENTIAL_BAD_NAME;
	}
	PCREDENTIALW cred = NULL;
	if (!CredReadW(wname, CRED_TYPE_GENERIC, 0, &cred)) {
		const DWORD e = GetLastError();
		if (e == ERROR_NOT_FOUND) {
			msg(err, err_size,
			    "no generic credential '%s' in Windows Credential Manager "
			    "(cmdkey /generic:<name> /user:displayxr /pass:<token>)",
			    name);
			return U_CREDENTIAL_NOT_FOUND;
		}
		if (err != NULL && err_size > 0) {
			(void)snprintf(err, err_size, "CredReadW('%s') failed: error %lu", name, (unsigned long)e);
		}
		return U_CREDENTIAL_ERROR;
	}
	enum u_credential_result r;
	const uint8_t *blob = cred->CredentialBlob;
	const size_t n = cred->CredentialBlobSize;
	// cmdkey and the Credential Manager UI store UTF-16LE; other writers store
	// bytes. A blob of even length whose every odd byte is 0 is UTF-16 (a token
	// is ASCII), anything else is taken verbatim.
	bool utf16 = n >= 2 && (n % 2) == 0;
	for (size_t i = 1; utf16 && i < n; i += 2) {
		utf16 = blob[i] == 0;
	}
	if (utf16) {
		char tmp[U_CREDENTIAL_SECRET_MAX + 1];
		const int got = WideCharToMultiByte(CP_UTF8, 0, (const wchar_t *)blob, (int)(n / 2), tmp,
		                                    (int)sizeof(tmp) - 1, NULL, NULL);
		r = got > 0 ? put_secret((const uint8_t *)tmp, (size_t)got, out, out_size) : U_CREDENTIAL_ERROR;
		u_credential_wipe(tmp, sizeof(tmp));
	} else {
		r = blob != NULL ? put_secret(blob, n, out, out_size) : U_CREDENTIAL_NOT_FOUND;
	}
	SecureZeroMemory(cred->CredentialBlob, cred->CredentialBlobSize);
	CredFree(cred);
	if (r == U_CREDENTIAL_TOO_LONG) {
		msg(err, err_size, "credential '%s' is longer than the reader accepts", name);
	} else if (r != U_CREDENTIAL_OK) {
		msg(err, err_size, "credential '%s' is empty or unreadable", name);
	}
	return r;
}

static enum u_credential_result
os_write(const char *name, const char *secret, char *err, size_t err_size)
{
	wchar_t wname[U_CREDENTIAL_NAME_MAX + 1];
	wchar_t wsecret[U_CREDENTIAL_SECRET_MAX + 1];
	if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, wname, U_CREDENTIAL_NAME_MAX + 1) <= 0) {
		return U_CREDENTIAL_BAD_NAME;
	}
	const int n =
	    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, secret, -1, wsecret, U_CREDENTIAL_SECRET_MAX + 1);
	if (n <= 1) {
		return U_CREDENTIAL_TOO_LONG;
	}
	CREDENTIALW c;
	memset(&c, 0, sizeof(c));
	c.Type = CRED_TYPE_GENERIC;
	c.TargetName = wname;
	c.CredentialBlobSize = (DWORD)((n - 1) * sizeof(wchar_t)); // UTF-16LE, like cmdkey
	c.CredentialBlob = (LPBYTE)wsecret;
	c.Persist = CRED_PERSIST_LOCAL_MACHINE; // this user, on this machine, across logons
	c.UserName = (LPWSTR)L"displayxr";
	const BOOL ok = CredWriteW(&c, 0);
	SecureZeroMemory(wsecret, sizeof(wsecret));
	if (!ok) {
		if (err != NULL && err_size > 0) {
			(void)snprintf(err, err_size, "CredWriteW('%s') failed: error %lu", name,
			               (unsigned long)GetLastError());
		}
		return U_CREDENTIAL_ERROR;
	}
	return U_CREDENTIAL_OK;
}

static enum u_credential_result
os_delete(const char *name, char *err, size_t err_size)
{
	wchar_t wname[U_CREDENTIAL_NAME_MAX + 1];
	if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, wname, U_CREDENTIAL_NAME_MAX + 1) <= 0) {
		return U_CREDENTIAL_BAD_NAME;
	}
	if (!CredDeleteW(wname, CRED_TYPE_GENERIC, 0) && GetLastError() != ERROR_NOT_FOUND) {
		if (err != NULL && err_size > 0) {
			(void)snprintf(err, err_size, "CredDeleteW('%s') failed: error %lu", name,
			               (unsigned long)GetLastError());
		}
		return U_CREDENTIAL_ERROR;
	}
	return U_CREDENTIAL_OK;
}


/*
 *
 * macOS: the login Keychain, never with UI.
 *
 */

#elif defined(XRT_OS_MACOS)

static enum u_credential_result
os_read(const char *name, char *out, size_t out_size, char *err, size_t err_size)
{
	CFStringRef service = CFStringCreateWithCString(kCFAllocatorDefault, name, kCFStringEncodingUTF8);
	if (service == NULL) {
		msg(err, err_size, "credential name '%s' is not valid UTF-8", name);
		return U_CREDENTIAL_BAD_NAME;
	}
	CFMutableDictionaryRef q = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
	                                                     &kCFTypeDictionaryValueCallBacks);
	CFDictionarySetValue(q, kSecClass, kSecClassGenericPassword);
	CFDictionarySetValue(q, kSecAttrService, service);
	CFDictionarySetValue(q, kSecReturnData, kCFBooleanTrue);
	CFDictionarySetValue(q, kSecMatchLimit, kSecMatchLimitOne);
	// A service or a conversion host has nobody to answer a prompt: fail instead.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
	CFDictionarySetValue(q, kSecUseAuthenticationUI, kSecUseAuthenticationUIFail);
#pragma clang diagnostic pop
	CFTypeRef data = NULL;
	// kSecUseAuthenticationUIFail does not reach the file-based login keychain:
	// an item whose partition list / access list lacks this binary still makes
	// securityd show "<app> wants to use your confidential information" and the
	// call blocks until someone answers (measured: it hung a headless run). Turn
	// user interaction off around the call so it fails with
	// errSecInteractionNotAllowed instead. Process-global, restored at once.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
	Boolean ui_was = true;
	const bool ui_known = SecKeychainGetUserInteractionAllowed(&ui_was) == errSecSuccess;
	(void)SecKeychainSetUserInteractionAllowed(false);
	const OSStatus st = SecItemCopyMatching(q, &data);
	if (ui_known) {
		(void)SecKeychainSetUserInteractionAllowed(ui_was);
	}
#pragma clang diagnostic pop
	CFRelease(q);
	CFRelease(service);

	enum u_credential_result r;
	if (st == errSecSuccess && data != NULL && CFGetTypeID(data) == CFDataGetTypeID()) {
		r = put_secret(CFDataGetBytePtr((CFDataRef)data), (size_t)CFDataGetLength((CFDataRef)data), out,
		               out_size);
		if (r == U_CREDENTIAL_TOO_LONG) {
			msg(err, err_size, "credential '%s' is longer than the reader accepts", name);
		} else if (r != U_CREDENTIAL_OK) {
			msg(err, err_size, "credential '%s' is empty", name);
		}
	} else if (st == errSecItemNotFound) {
		msg(err, err_size,
		    "no generic password with service '%s' in the Keychain "
		    "(store it with: displayxr-cli conv credential set <name>)",
		    name);
		r = U_CREDENTIAL_NOT_FOUND;
	} else if (st == errSecInteractionNotAllowed || st == errSecAuthFailed || st == errSecUserCanceled) {
		msg(err, err_size,
		    "the Keychain item '%s' exists but this program may not read it without a prompt (an item made "
		    "with the `security` tool trusts Apple's tools only): store it with displayxr-cli conv credential "
		    "set <name>",
		    name);
		r = U_CREDENTIAL_DENIED;
	} else {
		if (err != NULL && err_size > 0) {
			(void)snprintf(err, err_size, "SecItemCopyMatching('%s') failed: OSStatus %d", name, (int)st);
		}
		r = U_CREDENTIAL_ERROR;
	}
	if (data != NULL) {
		CFRelease(data);
	}
	return r;
}

static CFMutableDictionaryRef
mac_query(CFStringRef service)
{
	CFMutableDictionaryRef q = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
	                                                     &kCFTypeDictionaryValueCallBacks);
	CFDictionarySetValue(q, kSecClass, kSecClassGenericPassword);
	CFDictionarySetValue(q, kSecAttrService, service);
	return q;
}

static enum u_credential_result
os_delete(const char *name, char *err, size_t err_size)
{
	CFStringRef service = CFStringCreateWithCString(kCFAllocatorDefault, name, kCFStringEncodingUTF8);
	if (service == NULL) {
		return U_CREDENTIAL_BAD_NAME;
	}
	CFMutableDictionaryRef q = mac_query(service);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
	Boolean ui_was = true;
	const bool ui_known = SecKeychainGetUserInteractionAllowed(&ui_was) == errSecSuccess;
	(void)SecKeychainSetUserInteractionAllowed(false);
	const OSStatus st = SecItemDelete(q);
	if (ui_known) {
		(void)SecKeychainSetUserInteractionAllowed(ui_was);
	}
#pragma clang diagnostic pop
	CFRelease(q);
	CFRelease(service);
	if (st != errSecSuccess && st != errSecItemNotFound) {
		if (err != NULL && err_size > 0) {
			(void)snprintf(err, err_size, "SecItemDelete('%s') failed: OSStatus %d", name, (int)st);
		}
		return U_CREDENTIAL_ERROR;
	}
	return U_CREDENTIAL_OK;
}

static enum u_credential_result
os_write(const char *name, const char *secret, char *err, size_t err_size)
{
	// Replace, never update in place: an item someone else created may not be
	// ours to change, and a fresh item made by this binary trusts it.
	(void)os_delete(name, NULL, 0);
	CFStringRef service = CFStringCreateWithCString(kCFAllocatorDefault, name, kCFStringEncodingUTF8);
	char label_text[U_CREDENTIAL_NAME_MAX + 16];
	(void)snprintf(label_text, sizeof(label_text), "DisplayXR %s", name);
	CFStringRef label = CFStringCreateWithCString(kCFAllocatorDefault, label_text, kCFStringEncodingUTF8);
	CFDataRef data = CFDataCreate(kCFAllocatorDefault, (const UInt8 *)secret, (CFIndex)strlen(secret));
	if (service == NULL || label == NULL || data == NULL) {
		if (service != NULL) {
			CFRelease(service);
		}
		if (label != NULL) {
			CFRelease(label);
		}
		if (data != NULL) {
			CFRelease(data);
		}
		return U_CREDENTIAL_BAD_NAME;
	}
	CFMutableDictionaryRef q = mac_query(service);
	CFDictionarySetValue(q, kSecAttrAccount, CFSTR("displayxr"));
	CFDictionarySetValue(q, kSecAttrLabel, label);
	CFDictionarySetValue(q, kSecValueData, data);
	const OSStatus st = SecItemAdd(q, NULL);
	CFRelease(q);
	CFRelease(data);
	CFRelease(label);
	CFRelease(service);
	if (st != errSecSuccess) {
		if (err != NULL && err_size > 0) {
			(void)snprintf(err, err_size, "SecItemAdd('%s') failed: OSStatus %d", name, (int)st);
		}
		return U_CREDENTIAL_ERROR;
	}
	return U_CREDENTIAL_OK;
}


/*
 *
 * Linux: the Secret Service through libsecret, loaded at run time (no build
 * or package dependency; absent = UNSUPPORTED).
 *
 */

#elif defined(XRT_OS_LINUX_DESKTOP)

#define LINUX_LOOKUP_TIMEOUT_S 10

typedef void *(*cancellable_new_fn)(void);
typedef void (*cancellable_cancel_fn)(void *);
typedef void (*object_unref_fn)(void *);

//! A watchdog that cancels a libsecret lookup after LINUX_LOOKUP_TIMEOUT_S.
struct lookup_watch
{
	void *cancellable; //!< GCancellable*, NULL when GIO's entry points are unavailable
	cancellable_cancel_fn cancel;
	object_unref_fn unref;
	pthread_t thread;
	pthread_mutex_t mtx;
	pthread_cond_t cond;
	bool done, started;
};

static void *
lookup_watch_main(void *ptr)
{
	struct lookup_watch *w = (struct lookup_watch *)ptr;
	struct timespec until;
	clock_gettime(CLOCK_REALTIME, &until);
	until.tv_sec += LINUX_LOOKUP_TIMEOUT_S;
	pthread_mutex_lock(&w->mtx);
	while (!w->done) {
		if (pthread_cond_timedwait(&w->cond, &w->mtx, &until) != 0) {
			break; // timed out
		}
	}
	if (!w->done) {
		w->cancel(w->cancellable); // the lookup returns with a "cancelled" error
	}
	pthread_mutex_unlock(&w->mtx);
	return NULL;
}

static void
lookup_watch_start(void *lib, struct lookup_watch *w)
{
	// GIO / GObject are libsecret dependencies: resolvable through its handle.
	cancellable_new_fn mk = (cancellable_new_fn)dlsym(lib, "g_cancellable_new");
	w->cancel = (cancellable_cancel_fn)dlsym(lib, "g_cancellable_cancel");
	w->unref = (object_unref_fn)dlsym(lib, "g_object_unref");
	if (mk == NULL || w->cancel == NULL || w->unref == NULL || (w->cancellable = mk()) == NULL) {
		w->cancellable = NULL;
		return;
	}
	pthread_mutex_init(&w->mtx, NULL);
	pthread_cond_init(&w->cond, NULL);
	w->started = pthread_create(&w->thread, NULL, lookup_watch_main, w) == 0;
}

static void
lookup_watch_stop(struct lookup_watch *w)
{
	if (w->cancellable == NULL) {
		return;
	}
	if (w->started) {
		pthread_mutex_lock(&w->mtx);
		w->done = true;
		pthread_cond_signal(&w->cond);
		pthread_mutex_unlock(&w->mtx);
		pthread_join(w->thread, NULL);
	}
	pthread_cond_destroy(&w->cond);
	pthread_mutex_destroy(&w->mtx);
	w->unref(w->cancellable);
	w->cancellable = NULL;
}

/* The parts of libsecret's / GLib's ABI used here (stable since libsecret 0.7). */
typedef struct
{
	const char *name;
	int type; //!< SECRET_SCHEMA_ATTRIBUTE_STRING = 0
} dxr_secret_schema_attribute;

typedef struct
{
	const char *name;
	int flags; //!< SECRET_SCHEMA_DONT_MATCH_NAME = 1 << 1
	dxr_secret_schema_attribute attributes[32];
	int reserved;
	void *reserved1, *reserved2, *reserved3, *reserved4, *reserved5, *reserved6, *reserved7;
} dxr_secret_schema;

typedef struct
{
	uint32_t domain;
	int code;
	char *message;
} dxr_gerror;

typedef char *(*lookup_fn)(const dxr_secret_schema *, void *, dxr_gerror **, ...);
typedef void (*free_fn)(char *);
typedef void (*gerror_free_fn)(dxr_gerror *);

static enum u_credential_result
os_read(const char *name, char *out, size_t out_size, char *err, size_t err_size)
{
	void *lib = dlopen("libsecret-1.so.0", RTLD_NOW | RTLD_LOCAL);
	if (lib == NULL) {
		msg(err, err_size,
		    "no credential store: libsecret (libsecret-1.so.0) is not installed, so credential '%s' "
		    "cannot be read on this system",
		    name);
		return U_CREDENTIAL_UNSUPPORTED;
	}
	lookup_fn lookup = (lookup_fn)dlsym(lib, "secret_password_lookup_sync");
	free_fn pfree = (free_fn)dlsym(lib, "secret_password_free");
	gerror_free_fn gfree = (gerror_free_fn)dlsym(lib, "g_error_free"); // GLib, a libsecret dependency
	if (lookup == NULL || pfree == NULL) {
		dlclose(lib);
		msg(err, err_size, "libsecret is too old to read credential '%s'", name);
		return U_CREDENTIAL_UNSUPPORTED;
	}
	dxr_secret_schema schema;
	memset(&schema, 0, sizeof(schema));
	schema.name = "org.displayxr.Credential";
	schema.flags = 1 << 1; // DONT_MATCH_NAME: an item stored by `secret-tool store ... service <name>` matches
	schema.attributes[0].name = "service";
	schema.attributes[0].type = 0;
	dxr_gerror *gerr = NULL;
	// Bounded: libsecret has no "never prompt" switch for a lookup, and a locked
	// collection may raise an unlock prompt that nobody answers in a service.
	// A GCancellable cancelled by a watchdog after LINUX_LOOKUP_TIMEOUT_S ends it.
	struct lookup_watch lw;
	memset(&lw, 0, sizeof(lw));
	lookup_watch_start(lib, &lw);
	char *secret = lookup(&schema, lw.cancellable, &gerr, "service", name, (const char *)NULL);
	lookup_watch_stop(&lw);
	enum u_credential_result r;
	if (secret != NULL) {
		const size_t n = strlen(secret);
		r = put_secret((const uint8_t *)secret, n, out, out_size);
		pfree(secret); // secret_password_free wipes it
		if (r == U_CREDENTIAL_TOO_LONG) {
			msg(err, err_size, "credential '%s' is longer than the reader accepts", name);
		} else if (r != U_CREDENTIAL_OK) {
			msg(err, err_size, "credential '%s' is empty", name);
		}
	} else if (gerr != NULL) {
		if (err != NULL && err_size > 0) {
			(void)snprintf(err, err_size, "the Secret Service could not read '%s': %s", name,
			               gerr->message != NULL ? gerr->message : "unknown error");
		}
		if (gfree != NULL) {
			gfree(gerr);
		}
		r = U_CREDENTIAL_ERROR;
	} else {
		msg(err, err_size,
		    "no secret with attribute service='%s' in the Secret Service "
		    "(secret-tool store --label='DisplayXR <name>' service <name>)",
		    name);
		r = U_CREDENTIAL_NOT_FOUND;
	}
	dlclose(lib);
	return r;
}

typedef int (*store_fn)(
    const dxr_secret_schema *, const char *, const char *, const char *, void *, dxr_gerror **, ...);
typedef int (*clear_fn)(const dxr_secret_schema *, void *, dxr_gerror **, ...);

static void
linux_schema(dxr_secret_schema *schema)
{
	memset(schema, 0, sizeof(*schema));
	schema->name = "org.displayxr.Credential";
	schema->flags = 1 << 1;
	schema->attributes[0].name = "service";
	schema->attributes[0].type = 0;
}

static enum u_credential_result
os_write(const char *name, const char *secret, char *err, size_t err_size)
{
	void *lib = dlopen("libsecret-1.so.0", RTLD_NOW | RTLD_LOCAL);
	store_fn store = lib != NULL ? (store_fn)dlsym(lib, "secret_password_store_sync") : NULL;
	if (store == NULL) {
		if (lib != NULL) {
			dlclose(lib);
		}
		msg(err, err_size, "no credential store: libsecret is not installed (credential '%s')", name);
		return U_CREDENTIAL_UNSUPPORTED;
	}
	gerror_free_fn gfree = (gerror_free_fn)dlsym(lib, "g_error_free");
	dxr_secret_schema schema;
	linux_schema(&schema);
	char label[U_CREDENTIAL_NAME_MAX + 16];
	(void)snprintf(label, sizeof(label), "DisplayXR %s", name);
	dxr_gerror *gerr = NULL;
	const int ok = store(&schema, "default", label, secret, NULL, &gerr, "service", name, (const char *)NULL);
	enum u_credential_result r = ok ? U_CREDENTIAL_OK : U_CREDENTIAL_ERROR;
	if (!ok && err != NULL && err_size > 0) {
		(void)snprintf(err, err_size, "the Secret Service could not store '%s': %s", name,
		               gerr != NULL && gerr->message != NULL ? gerr->message : "unknown error");
	}
	if (gerr != NULL && gfree != NULL) {
		gfree(gerr);
	}
	dlclose(lib);
	return r;
}

static enum u_credential_result
os_delete(const char *name, char *err, size_t err_size)
{
	void *lib = dlopen("libsecret-1.so.0", RTLD_NOW | RTLD_LOCAL);
	clear_fn clear = lib != NULL ? (clear_fn)dlsym(lib, "secret_password_clear_sync") : NULL;
	if (clear == NULL) {
		if (lib != NULL) {
			dlclose(lib);
		}
		msg(err, err_size, "no credential store: libsecret is not installed (credential '%s')", name);
		return U_CREDENTIAL_UNSUPPORTED;
	}
	gerror_free_fn gfree = (gerror_free_fn)dlsym(lib, "g_error_free");
	dxr_secret_schema schema;
	linux_schema(&schema);
	dxr_gerror *gerr = NULL;
	(void)clear(&schema, NULL, &gerr, "service", name, (const char *)NULL); // false = nothing to clear
	enum u_credential_result r = U_CREDENTIAL_OK;
	if (gerr != NULL) {
		if (err != NULL && err_size > 0) {
			(void)snprintf(err, err_size, "the Secret Service could not delete '%s': %s", name,
			               gerr->message != NULL ? gerr->message : "unknown error");
		}
		if (gfree != NULL) {
			gfree(gerr);
		}
		r = U_CREDENTIAL_ERROR;
	}
	dlclose(lib);
	return r;
}

#else

static enum u_credential_result
os_read(const char *name, char *out, size_t out_size, char *err, size_t err_size)
{
	(void)out;
	(void)out_size;
	msg(err, err_size, "no credential store on this platform (credential '%s')", name);
	return U_CREDENTIAL_UNSUPPORTED;
}

static enum u_credential_result
os_write(const char *name, const char *secret, char *err, size_t err_size)
{
	(void)secret;
	msg(err, err_size, "no credential store on this platform (credential '%s')", name);
	return U_CREDENTIAL_UNSUPPORTED;
}

static enum u_credential_result
os_delete(const char *name, char *err, size_t err_size)
{
	msg(err, err_size, "no credential store on this platform (credential '%s')", name);
	return U_CREDENTIAL_UNSUPPORTED;
}

#endif

enum u_credential_result
u_credential_write(const char *name, const char *secret, char *err, size_t err_size)
{
	if (err != NULL && err_size > 0) {
		err[0] = '\0';
	}
	if (!u_credential_name_valid(name)) {
		msg(err, err_size, "invalid credential name '%s'", name);
		return U_CREDENTIAL_BAD_NAME;
	}
	if (secret != NULL && secret[0] != '\0' && !u_credential_secret_valid_n(secret, strlen(secret))) {
		msg(err, err_size, "the secret for '%s' contains control characters (CR / LF / ...): refused", name);
		return U_CREDENTIAL_BAD_SECRET;
	}
	if (secret == NULL || secret[0] == '\0' || strlen(secret) > U_CREDENTIAL_SECRET_MAX) {
		msg(err, err_size, "the secret for '%s' is empty or too long", name);
		return U_CREDENTIAL_TOO_LONG;
	}
	return os_write(name, secret, err, err_size);
}

enum u_credential_result
u_credential_delete(const char *name, char *err, size_t err_size)
{
	if (err != NULL && err_size > 0) {
		err[0] = '\0';
	}
	if (!u_credential_name_valid(name)) {
		msg(err, err_size, "invalid credential name '%s'", name);
		return U_CREDENTIAL_BAD_NAME;
	}
	return os_delete(name, err, err_size);
}

enum u_credential_result
u_credential_read(const char *name, char *out, size_t out_size, char *err, size_t err_size)
{
	if (err != NULL && err_size > 0) {
		err[0] = '\0';
	}
	if (out == NULL || out_size == 0) {
		return U_CREDENTIAL_ERROR;
	}
	memset(out, 0, out_size);
	if (!u_credential_name_valid(name)) {
		msg(err, err_size, "invalid credential name '%s'", name);
		return U_CREDENTIAL_BAD_NAME;
	}
	enum u_credential_result r;
	if (g_test_reader != NULL) {
		r = g_test_reader(name, out, out_size);
		if (r == U_CREDENTIAL_OK && !u_credential_secret_valid_n(out, strnlen(out, out_size))) {
			r = U_CREDENTIAL_BAD_SECRET;
		}
		if (r != U_CREDENTIAL_OK) {
			msg(err, err_size, "test store: no usable credential '%s'", name);
		}
	} else {
		r = os_read(name, out, out_size, err, err_size);
	}
	if (r != U_CREDENTIAL_OK) {
		u_credential_wipe(out, out_size);
	}
	return r;
}
