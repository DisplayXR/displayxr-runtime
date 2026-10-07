// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_stereo_camera R3 (ADR-043): the consent policy against fake
 *         store / environment vtables, the keyed persistentId, the SHA-256 /
 *         HMAC it rests on, and the client-class rules the service applies at
 *         admission (CAMERA_CONSUMER outside the PRESENT_OWNER quota).
 */

#include "catch_amalgamated.hpp"

#include "util/u_camera_consent.h"
#include "util/u_client_class.h"
#include "util/u_sha256.h"
#include "xrt/xrt_instance.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

struct fake_entry_info
{
	u_camera_consent_delegation_scope scope = U_CAMERA_CONSENT_DELEGATION_SYSTEM;
	std::string signer;
};

//! insert(exe) registers a SYSTEM-level, signer-less entry (the installer's case).
struct fake_delegating_list
{
	std::map<std::string, fake_entry_info> m;
	void
	insert(const std::string &exe,
	       u_camera_consent_delegation_scope scope = U_CAMERA_CONSENT_DELEGATION_SYSTEM,
	       const std::string &signer = "")
	{
		m[exe] = fake_entry_info{scope, signer};
	}
};

struct fake_store
{
	std::map<std::string, u_camera_consent_stored> apps;
	fake_delegating_list delegating;
	bool sharing = true;
	bool have_secret = true;
	uint8_t secret[32] = {1, 2, 3};
	int sets = 0;
	bool set_fails = false;
};

bool
fs_get(void *ctx, const char *exe, u_camera_consent_stored *out)
{
	auto *s = (fake_store *)ctx;
	auto it = s->apps.find(exe);
	if (it == s->apps.end()) {
		return false;
	}
	*out = it->second;
	return true;
}
bool
fs_set(void *ctx, const char *exe, u_camera_consent_stored v)
{
	auto *s = (fake_store *)ctx;
	s->sets++;
	if (s->set_fails) {
		return false;
	}
	if (v == U_CAMERA_CONSENT_STORED_NONE) {
		s->apps.erase(exe);
	} else {
		s->apps[exe] = v;
	}
	return true;
}
bool
fs_get_delegation(void *ctx, const char *exe, u_camera_consent_delegation *out)
{
	auto &m = ((fake_store *)ctx)->delegating.m;
	auto it = m.find(exe);
	if (it == m.end()) {
		return false;
	}
	out->scope = it->second.scope;
	snprintf(out->signer, sizeof(out->signer), "%s", it->second.signer.c_str());
	return true;
}
bool
fs_sharing(void *ctx)
{
	return ((fake_store *)ctx)->sharing;
}
bool
fs_set_sharing(void *ctx, bool on)
{
	((fake_store *)ctx)->sharing = on;
	return true;
}
bool
fs_secret(void *ctx, uint8_t out[32])
{
	auto *s = (fake_store *)ctx;
	if (!s->have_secret) {
		return false;
	}
	memcpy(out, s->secret, 32);
	return true;
}

const u_camera_consent_store_ops fake_store_ops = {fs_get,     fs_set,         fs_get_delegation,
                                                   fs_sharing, fs_set_sharing, fs_secret};

struct fake_env
{
	bool os_allowed = true;
	u_camera_consent_prompt_answer answer = U_CAMERA_CONSENT_PROMPT_UNAVAILABLE;
	int prompts = 0;
	std::string last_exe;
	long last_pid = 0;
	//! Executables whose path a non-admin process could replace (default: none).
	std::set<std::string> user_writable;
	//! Validly signed executables -> their signer CN.
	std::map<std::string, std::string> signers;
	int signer_queries = 0;
	std::vector<std::string> notices;
};

bool
fe_os(void *ctx, const char *exe)
{
	(void)exe;
	return ((fake_env *)ctx)->os_allowed;
}
u_camera_consent_prompt_answer
fe_prompt(void *ctx, const char *exe, const char *app, long pid, uint32_t timeout_ms)
{
	(void)app;
	(void)timeout_ms;
	auto *e = (fake_env *)ctx;
	e->prompts++;
	e->last_exe = exe;
	e->last_pid = pid;
	return e->answer;
}

bool
fe_user_writable(void *ctx, const char *exe)
{
	return ((fake_env *)ctx)->user_writable.count(exe) > 0;
}
bool
fe_signer(void *ctx, const char *exe, char *out, size_t cap)
{
	auto *e = (fake_env *)ctx;
	e->signer_queries++;
	auto it = e->signers.find(exe);
	if (it == e->signers.end()) {
		return false;
	}
	snprintf(out, cap, "%s", it->second.c_str());
	return true;
}
void
fe_notice(void *ctx, const char *exe, const char *app)
{
	(void)app;
	((fake_env *)ctx)->notices.push_back(exe);
}

const u_camera_consent_env_ops fake_env_ops = {fe_os, fe_prompt, fe_user_writable, fe_signer, fe_notice};

struct rig
{
	fake_store store;
	fake_env env;
	u_camera_consent c;
	rig()
	{
		u_camera_consent_init(&c, &fake_store_ops, &store, &fake_env_ops, &env);
	}
	u_camera_consent_decision
	eval(const char *exe = "/apps/caller", long pid = 100, uint32_t flags = 0)
	{
		u_camera_consent_decision d;
		u_camera_consent_evaluate(&c, exe, "Caller", pid, flags, &d);
		return d;
	}
};

} // namespace

TEST_CASE("camera consent: sharing off wins over everything", "[camera_consent]")
{
	rig r;
	r.c.dev_override = true;
	r.store.delegating.insert("/apps/caller");
	SECTION("kill switch")
	{
		r.c.kill_switch = true;
		auto d = r.eval();
		CHECK(d.verdict == U_CAMERA_CONSENT_DISABLED);
		CHECK(d.why == U_CAMERA_CONSENT_WHY_KILL_SWITCH);
	}
	SECTION("user toggle")
	{
		r.store.sharing = false;
		auto d = r.eval();
		CHECK(d.verdict == U_CAMERA_CONSENT_DISABLED);
		CHECK(d.why == U_CAMERA_CONSENT_WHY_SHARING_OFF);
		CHECK(!u_camera_consent_sharing_enabled(&r.c));
	}
	CHECK(r.env.prompts == 0);
}

TEST_CASE("camera consent: dev override allows without a prompt, once logged", "[camera_consent]")
{
	rig r;
	r.c.dev_override = true;
	auto d = r.eval();
	CHECK(d.verdict == U_CAMERA_CONSENT_ALLOWED);
	CHECK(d.why == U_CAMERA_CONSENT_WHY_DEV_OVERRIDE);
	CHECK(!d.delegating);
	CHECK(r.c.dev_override_logged);
	CHECK(r.env.prompts == 0);
	CHECK(r.store.sets == 0);
}

TEST_CASE("camera consent: no verifiable executable is refused, never prompted", "[camera_consent]")
{
	rig r;
	r.env.answer = U_CAMERA_CONSENT_PROMPT_ALLOW;
	auto d = r.eval("");
	CHECK(d.verdict == U_CAMERA_CONSENT_REFUSED);
	CHECK(d.why == U_CAMERA_CONSENT_WHY_NO_IDENTITY);
	CHECK(r.env.prompts == 0);
}

// Spec §7.1: delegation means "no runtime prompt and no stored decision needed"
// — nothing more. Every refusal that applies to any app applies to it first.
TEST_CASE("camera consent: a delegating client passes without a prompt or a store entry", "[camera_consent]")
{
	rig r;
	r.store.delegating.insert("/opt/browser/browser");
	r.env.answer = U_CAMERA_CONSENT_PROMPT_DENY; // would refuse if it were asked
	auto d = r.eval("/opt/browser/browser");
	CHECK(d.verdict == U_CAMERA_CONSENT_ALLOWED);
	CHECK(d.why == U_CAMERA_CONSENT_WHY_DELEGATING);
	CHECK(d.delegating);
	CHECK(d.delegated);
	CHECK(d.delegation_scope == U_CAMERA_CONSENT_DELEGATION_SYSTEM);
	CHECK(r.env.prompts == 0);
	CHECK(r.store.sets == 0);
	CHECK(r.store.apps.empty());
	CHECK(r.env.notices.empty()); // a machine-level (installer) entry is silent
}

/*
 * Spec v3: XR_STEREO_CAMERA_CLIENT_DECLINE_DELEGATION_BIT_DXR. The client says
 * "do not take my own prompt as consent this time" — the delegation step is
 * skipped and the executable is an ordinary app: stored Allow, Allow once,
 * the runtime prompt. Every refusal still applies first.
 */
TEST_CASE("camera consent: DECLINE_DELEGATION skips the delegation step", "[camera_consent][decline]")
{
	const char *b = "/opt/browser/browser";
	const uint32_t decline = U_CAMERA_CONSENT_FLAG_DECLINE_DELEGATION;
	rig r;
	r.store.delegating.insert(b);
	SECTION("nothing stored: the runtime prompt decides")
	{
		r.env.answer = U_CAMERA_CONSENT_PROMPT_DENY;
		auto d = r.eval(b, 100, decline);
		CHECK(d.verdict == U_CAMERA_CONSENT_REFUSED);
		CHECK(d.why == U_CAMERA_CONSENT_WHY_PROMPT_DENY);
		CHECK(r.env.prompts == 1);
		CHECK(d.delegating); // still REGISTERED: the RAW refusal keeps applying
		CHECK(!d.delegated); // but not allowed by delegation
		CHECK(d.delegation_skip == U_CAMERA_CONSENT_DELEGATION_DECLINED);
		// The prompt's Deny is now stored, and it wins the next time too —
		// with or without the flag.
		d = r.eval(b, 100, 0);
		CHECK(d.why == U_CAMERA_CONSENT_WHY_STORED_DENY);
	}
	SECTION("prompt unavailable: refused")
	{
		auto d = r.eval(b, 100, decline);
		CHECK(d.verdict == U_CAMERA_CONSENT_REFUSED);
		CHECK(d.why == U_CAMERA_CONSENT_WHY_PROMPT_UNAVAILABLE);
	}
	SECTION("a stored Allow allows")
	{
		r.store.apps[b] = U_CAMERA_CONSENT_STORED_ALLOW;
		auto d = r.eval(b, 100, decline);
		CHECK(d.verdict == U_CAMERA_CONSENT_ALLOWED);
		CHECK(d.why == U_CAMERA_CONSENT_WHY_STORED_ALLOW);
		CHECK(!d.delegated);
		CHECK(r.env.prompts == 0);
	}
	SECTION("Allow once allows the same process again")
	{
		r.env.answer = U_CAMERA_CONSENT_PROMPT_ALLOW_ONCE;
		auto d = r.eval(b, 7, decline);
		CHECK(d.why == U_CAMERA_CONSENT_WHY_PROMPT_ALLOW_ONCE);
		d = r.eval(b, 7, decline);
		CHECK(d.why == U_CAMERA_CONSENT_WHY_ALLOW_ONCE);
		CHECK(r.env.prompts == 1);
	}
	SECTION("a stored Deny still wins")
	{
		r.store.apps[b] = U_CAMERA_CONSENT_STORED_DENY;
		r.env.answer = U_CAMERA_CONSENT_PROMPT_ALLOW;
		auto d = r.eval(b, 100, decline);
		CHECK(d.verdict == U_CAMERA_CONSENT_REFUSED);
		CHECK(d.why == U_CAMERA_CONSENT_WHY_STORED_DENY);
		CHECK(r.env.prompts == 0);
	}
	SECTION("without the flag the same executable is delegated")
	{
		auto d = r.eval(b, 100, 0);
		CHECK(d.why == U_CAMERA_CONSENT_WHY_DELEGATING);
		CHECK(d.delegated);
	}
	SECTION("unknown flag bits are ignored")
	{
		auto d = r.eval(b, 100, 0xfffffffeu);
		CHECK(d.why == U_CAMERA_CONSENT_WHY_DELEGATING);
	}
	SECTION("an unregistered executable is unaffected by the flag")
	{
		r.env.answer = U_CAMERA_CONSENT_PROMPT_ALLOW;
		auto d = r.eval("/apps/caller", 100, decline);
		CHECK(d.why == U_CAMERA_CONSENT_WHY_PROMPT_ALLOW);
		CHECK(d.delegation_skip == U_CAMERA_CONSENT_DELEGATION_APPLIES);
	}
}

/*
 * Spec §7.1.1: a delegating entry whose executable sits in a user-writable
 * location is trusted only with a recorded signer that the executable's valid
 * signature matches. Otherwise step 6 is skipped exactly as if not delegating.
 */
TEST_CASE("camera consent: user-writable delegating entries need a matching signer", "[camera_consent][signer]")
{
	const char *b = "C:\\Users\\u\\AppData\\Local\\Browser\\browser.exe";
	rig r;
	r.env.user_writable.insert(b);
	r.env.answer = U_CAMERA_CONSENT_PROMPT_UNAVAILABLE;
	SECTION("no signer recorded: not delegated, falls through to the prompt")
	{
		r.store.delegating.insert(b, U_CAMERA_CONSENT_DELEGATION_USER);
		r.env.signers[b] = "Browser Corp"; // validly signed, but the entry names nobody
		auto d = r.eval(b);
		CHECK(d.verdict == U_CAMERA_CONSENT_REFUSED);
		CHECK(d.why == U_CAMERA_CONSENT_WHY_PROMPT_UNAVAILABLE);
		CHECK(r.env.prompts == 1);
		CHECK(d.delegating);
		CHECK(!d.delegated);
		CHECK(d.delegation_skip == U_CAMERA_CONSENT_DELEGATION_NO_SIGNER);
		CHECK(r.env.notices.empty());
		// The WARN is once per executable per run.
		CHECK(r.c.untrusted_warned_count == 1);
		r.eval(b);
		CHECK(r.c.untrusted_warned_count == 1);
	}
	SECTION("no signer recorded, a stored Allow still allows (as for any app)")
	{
		r.store.delegating.insert(b, U_CAMERA_CONSENT_DELEGATION_USER);
		r.store.apps[b] = U_CAMERA_CONSENT_STORED_ALLOW;
		auto d = r.eval(b);
		CHECK(d.verdict == U_CAMERA_CONSENT_ALLOWED);
		CHECK(d.why == U_CAMERA_CONSENT_WHY_STORED_ALLOW);
		CHECK(!d.delegated);
	}
	SECTION("matching signer: delegated")
	{
		r.store.delegating.insert(b, U_CAMERA_CONSENT_DELEGATION_SYSTEM, "Browser Corp");
		r.env.signers[b] = "browser corp "; // case / surrounding spaces do not matter
		auto d = r.eval(b);
		CHECK(d.verdict == U_CAMERA_CONSENT_ALLOWED);
		CHECK(d.why == U_CAMERA_CONSENT_WHY_DELEGATING);
		CHECK(d.delegated);
		CHECK(r.env.prompts == 0);
		CHECK(r.env.signer_queries == 1);
	}
	SECTION("not validly signed: not delegated")
	{
		r.store.delegating.insert(b, U_CAMERA_CONSENT_DELEGATION_SYSTEM, "Browser Corp");
		auto d = r.eval(b);
		CHECK(d.why == U_CAMERA_CONSENT_WHY_PROMPT_UNAVAILABLE);
		CHECK(!d.delegated);
		CHECK(d.delegation_skip == U_CAMERA_CONSENT_DELEGATION_NOT_SIGNED);
	}
	SECTION("signed by someone else: not delegated")
	{
		r.store.delegating.insert(b, U_CAMERA_CONSENT_DELEGATION_SYSTEM, "Browser Corp");
		r.env.signers[b] = "Browser Corp Evil";
		auto d = r.eval(b);
		CHECK(d.why == U_CAMERA_CONSENT_WHY_PROMPT_UNAVAILABLE);
		CHECK(!d.delegated);
		CHECK(d.delegation_skip == U_CAMERA_CONSENT_DELEGATION_SIGNER_MISMATCH);
	}
	SECTION("a stored Deny still decides before the signer is even looked at")
	{
		r.store.delegating.insert(b, U_CAMERA_CONSENT_DELEGATION_SYSTEM, "Browser Corp");
		r.env.signers[b] = "Browser Corp";
		r.store.apps[b] = U_CAMERA_CONSENT_STORED_DENY;
		auto d = r.eval(b);
		CHECK(d.why == U_CAMERA_CONSENT_WHY_STORED_DENY);
		CHECK(r.env.signer_queries == 0);
	}
}

TEST_CASE("camera consent: an admin-protected delegating entry is trusted by path alone", "[camera_consent][signer]")
{
	const char *b = "C:\\Program Files\\Browser\\browser.exe";
	rig r; // r.env.user_writable is empty: every path is admin-protected
	r.store.delegating.insert(b, U_CAMERA_CONSENT_DELEGATION_SYSTEM, ""); // no signer, unsigned
	auto d = r.eval(b);
	CHECK(d.verdict == U_CAMERA_CONSENT_ALLOWED);
	CHECK(d.why == U_CAMERA_CONSENT_WHY_DELEGATING);
	CHECK(d.delegated);
	CHECK(r.env.signer_queries == 0);
	SECTION("a recorded signer is not consulted there either")
	{
		r.store.delegating.insert(b, U_CAMERA_CONSENT_DELEGATION_SYSTEM, "Somebody Else");
		d = r.eval(b);
		CHECK(d.why == U_CAMERA_CONSENT_WHY_DELEGATING);
		CHECK(r.env.signer_queries == 0);
	}
}

TEST_CASE("camera consent: a host that cannot classify paths fails closed", "[camera_consent][signer]")
{
	// No path_user_writable / exe_signer: every path counts as user-writable
	// and nothing can be verified -> a delegating entry is never applied.
	fake_store store;
	store.delegating.insert("/opt/browser/browser");
	fake_env env;
	const u_camera_consent_env_ops minimal = {fe_os, fe_prompt, nullptr, nullptr, nullptr};
	u_camera_consent c;
	u_camera_consent_init(&c, &fake_store_ops, &store, &minimal, &env);
	u_camera_consent_decision d;
	u_camera_consent_evaluate(&c, "/opt/browser/browser", "B", 1, 0, &d);
	CHECK(d.why == U_CAMERA_CONSENT_WHY_PROMPT_UNAVAILABLE);
	CHECK(d.delegation_skip == U_CAMERA_CONSENT_DELEGATION_NO_SIGNER);
	store.delegating.insert("/opt/browser/browser", U_CAMERA_CONSENT_DELEGATION_SYSTEM, "Browser Corp");
	u_camera_consent_evaluate(&c, "/opt/browser/browser", "B", 1, 0, &d);
	CHECK(d.delegation_skip == U_CAMERA_CONSENT_DELEGATION_NOT_SIGNED);
	CHECK(!d.delegated);
}

/*
 * Spec §7.1.1: a USER-level entry is honoured, but its first use per
 * executable per service run is announced (WARN + the host's notice).
 */
TEST_CASE("camera consent: user-level delegation raises a first-use notice", "[camera_consent][notice]")
{
	const char *b = "/opt/browser/browser";
	const char *b2 = "/opt/other/other";
	rig r;
	r.store.delegating.insert(b, U_CAMERA_CONSENT_DELEGATION_USER);
	auto d = r.eval(b);
	CHECK(d.why == U_CAMERA_CONSENT_WHY_DELEGATING);
	CHECK(d.delegation_scope == U_CAMERA_CONSENT_DELEGATION_USER);
	REQUIRE(r.env.notices.size() == 1);
	CHECK(r.env.notices[0] == b);
	// Once per executable per run: a second start, or another pid, is silent.
	r.eval(b, 100);
	r.eval(b, 200);
	CHECK(r.env.notices.size() == 1);
	// Another user-delegated executable gets its own.
	r.store.delegating.insert(b2, U_CAMERA_CONSENT_DELEGATION_USER);
	r.eval(b2);
	CHECK(r.env.notices.size() == 2);
	SECTION("no notice when the delegation did not apply")
	{
		r.store.delegating.insert("/opt/third", U_CAMERA_CONSENT_DELEGATION_USER);
		r.store.apps["/opt/third"] = U_CAMERA_CONSENT_STORED_DENY;
		r.eval("/opt/third");                                              // stored Deny decides
		r.eval("/opt/third", 1, U_CAMERA_CONSENT_FLAG_DECLINE_DELEGATION); // declined
		CHECK(r.env.notices.size() == 2);
	}
	SECTION("no notice for a system-level entry")
	{
		r.store.delegating.insert("/opt/sys", U_CAMERA_CONSENT_DELEGATION_SYSTEM);
		CHECK(r.eval("/opt/sys").why == U_CAMERA_CONSENT_WHY_DELEGATING);
		CHECK(r.env.notices.size() == 2);
	}
	SECTION("the seen-set is bounded and never overflows")
	{
		for (int i = 0; i < 3 * U_CAMERA_CONSENT_SEEN_MAX; i++) {
			std::string e = "/opt/many/" + std::to_string(i);
			r.store.delegating.insert(e, U_CAMERA_CONSENT_DELEGATION_USER);
			r.eval(e.c_str());
		}
		CHECK(r.env.notices.size() == 2 + 3 * U_CAMERA_CONSENT_SEEN_MAX);
	}
}

TEST_CASE("camera consent: a delegating client is refused by the OS camera switch", "[camera_consent]")
{
	rig r;
	r.store.delegating.insert("/opt/browser/browser");
	r.env.os_allowed = false;
	SECTION("nothing stored")
	{
		auto d = r.eval("/opt/browser/browser");
		CHECK(d.verdict == U_CAMERA_CONSENT_OS_DENIED);
		CHECK(d.why == U_CAMERA_CONSENT_WHY_OS_DENIED);
		CHECK(d.delegating); // still reported: it is a fact about the executable
	}
	SECTION("even with a stored Allow")
	{
		r.store.apps["/opt/browser/browser"] = U_CAMERA_CONSENT_STORED_ALLOW;
		auto d = r.eval("/opt/browser/browser");
		CHECK(d.verdict == U_CAMERA_CONSENT_OS_DENIED);
		CHECK(d.why == U_CAMERA_CONSENT_WHY_OS_DENIED);
	}
	CHECK(r.env.prompts == 0);
	CHECK(r.store.sets == 0);
}

TEST_CASE("camera consent: a stored Deny beats a delegating registration", "[camera_consent]")
{
	rig r;
	r.store.delegating.insert("/opt/browser/browser");
	r.store.apps["/opt/browser/browser"] = U_CAMERA_CONSENT_STORED_DENY;
	r.env.answer = U_CAMERA_CONSENT_PROMPT_ALLOW;
	auto d = r.eval("/opt/browser/browser");
	CHECK(d.verdict == U_CAMERA_CONSENT_REFUSED);
	CHECK(d.why == U_CAMERA_CONSENT_WHY_STORED_DENY);
	CHECK(d.delegating);
	CHECK(r.env.prompts == 0);
	CHECK(r.store.sets == 0);
	// The OS switch, when also off, is the reported reason (it is checked first).
	r.env.os_allowed = false;
	d = r.eval("/opt/browser/browser");
	CHECK(d.verdict == U_CAMERA_CONSENT_OS_DENIED);
}

TEST_CASE("camera consent: sharing off disables a delegating client", "[camera_consent]")
{
	rig r;
	r.store.delegating.insert("/opt/browser/browser");
	SECTION("user toggle")
	{
		r.store.sharing = false;
		auto d = r.eval("/opt/browser/browser");
		CHECK(d.verdict == U_CAMERA_CONSENT_DISABLED);
		CHECK(d.why == U_CAMERA_CONSENT_WHY_SHARING_OFF);
	}
	SECTION("DXR_STEREO_CAMERA=0")
	{
		r.c.kill_switch = true;
		auto d = r.eval("/opt/browser/browser");
		CHECK(d.verdict == U_CAMERA_CONSENT_DISABLED);
		CHECK(d.why == U_CAMERA_CONSENT_WHY_KILL_SWITCH);
	}
	CHECK(r.env.prompts == 0);
}

TEST_CASE("camera consent: a stored Allow on a delegating client reports delegation", "[camera_consent]")
{
	rig r;
	r.store.delegating.insert("/opt/browser/browser");
	r.store.apps["/opt/browser/browser"] = U_CAMERA_CONSENT_STORED_ALLOW;
	auto d = r.eval("/opt/browser/browser");
	CHECK(d.verdict == U_CAMERA_CONSENT_ALLOWED);
	CHECK(d.why == U_CAMERA_CONSENT_WHY_DELEGATING);
	CHECK(d.delegating);
}

TEST_CASE("camera consent: an unregistered executable next to a delegating one takes the ordinary path",
          "[camera_consent]")
{
	rig r;
	r.store.delegating.insert("/opt/browser/browser");
	auto d = r.eval("/apps/caller"); // not registered: the ordinary path
	CHECK(d.verdict == U_CAMERA_CONSENT_REFUSED);
	CHECK(d.why == U_CAMERA_CONSENT_WHY_PROMPT_UNAVAILABLE);
	CHECK(!d.delegating);
	CHECK(r.env.prompts == 1);
}

TEST_CASE("camera consent: the OS camera switch refuses before the store", "[camera_consent]")
{
	rig r;
	r.env.os_allowed = false;
	r.store.apps["/apps/caller"] = U_CAMERA_CONSENT_STORED_ALLOW;
	auto d = r.eval();
	CHECK(d.verdict == U_CAMERA_CONSENT_OS_DENIED);
	CHECK(d.why == U_CAMERA_CONSENT_WHY_OS_DENIED);
	CHECK(r.env.prompts == 0);
}

TEST_CASE("camera consent: stored decisions are final, no prompt", "[camera_consent]")
{
	rig r;
	r.env.answer = U_CAMERA_CONSENT_PROMPT_DENY;
	r.store.apps["/apps/caller"] = U_CAMERA_CONSENT_STORED_ALLOW;
	auto d = r.eval();
	CHECK(d.verdict == U_CAMERA_CONSENT_ALLOWED);
	CHECK(d.why == U_CAMERA_CONSENT_WHY_STORED_ALLOW);

	r.env.answer = U_CAMERA_CONSENT_PROMPT_ALLOW;
	r.store.apps["/apps/caller"] = U_CAMERA_CONSENT_STORED_DENY;
	d = r.eval();
	CHECK(d.verdict == U_CAMERA_CONSENT_REFUSED);
	CHECK(d.why == U_CAMERA_CONSENT_WHY_STORED_DENY);
	CHECK(r.env.prompts == 0);
}

TEST_CASE("camera consent: the prompt's answers", "[camera_consent]")
{
	rig r;
	SECTION("Allow is stored and not asked again")
	{
		r.env.answer = U_CAMERA_CONSENT_PROMPT_ALLOW;
		auto d = r.eval();
		CHECK(d.verdict == U_CAMERA_CONSENT_ALLOWED);
		CHECK(d.why == U_CAMERA_CONSENT_WHY_PROMPT_ALLOW);
		CHECK(r.env.prompts == 1);
		CHECK(r.env.last_exe == "/apps/caller");
		CHECK(r.env.last_pid == 100);
		CHECK(r.store.apps["/apps/caller"] == U_CAMERA_CONSENT_STORED_ALLOW);
		d = r.eval();
		CHECK(d.why == U_CAMERA_CONSENT_WHY_STORED_ALLOW);
		CHECK(r.env.prompts == 1);
	}
	SECTION("Allow once binds to the process, not the executable")
	{
		r.env.answer = U_CAMERA_CONSENT_PROMPT_ALLOW_ONCE;
		auto d = r.eval("/apps/caller", 100);
		CHECK(d.verdict == U_CAMERA_CONSENT_ALLOWED);
		CHECK(d.why == U_CAMERA_CONSENT_WHY_PROMPT_ALLOW_ONCE);
		CHECK(r.store.sets == 0);
		d = r.eval("/apps/caller", 100);
		CHECK(d.why == U_CAMERA_CONSENT_WHY_ALLOW_ONCE);
		CHECK(r.env.prompts == 1);
		d = r.eval("/apps/caller", 101); // a new process of the same app asks again
		CHECK(d.why == U_CAMERA_CONSENT_WHY_PROMPT_ALLOW_ONCE);
		CHECK(r.env.prompts == 2);
		u_camera_consent_forget_once(&r.c); // the user stopped sharing
		d = r.eval("/apps/caller", 100);
		CHECK(r.env.prompts == 3);
	}
	SECTION("Deny is stored")
	{
		r.env.answer = U_CAMERA_CONSENT_PROMPT_DENY;
		auto d = r.eval();
		CHECK(d.verdict == U_CAMERA_CONSENT_REFUSED);
		CHECK(d.why == U_CAMERA_CONSENT_WHY_PROMPT_DENY);
		CHECK(r.store.apps["/apps/caller"] == U_CAMERA_CONSENT_STORED_DENY);
		d = r.eval();
		CHECK(d.why == U_CAMERA_CONSENT_WHY_STORED_DENY);
		CHECK(r.env.prompts == 1);
	}
	SECTION("unanswered / unavailable refuse and are asked again next time")
	{
		r.env.answer = U_CAMERA_CONSENT_PROMPT_TIMEOUT;
		auto d = r.eval();
		CHECK(d.verdict == U_CAMERA_CONSENT_REFUSED);
		CHECK(d.why == U_CAMERA_CONSENT_WHY_PROMPT_TIMEOUT);
		r.env.answer = U_CAMERA_CONSENT_PROMPT_UNAVAILABLE;
		d = r.eval();
		CHECK(d.verdict == U_CAMERA_CONSENT_REFUSED);
		CHECK(d.why == U_CAMERA_CONSENT_WHY_PROMPT_UNAVAILABLE);
		CHECK(r.env.prompts == 2);
		CHECK(r.store.sets == 0);
	}
	SECTION("prompt disabled (headless) refuses without calling it")
	{
		r.c.prompt_enabled = false;
		r.env.answer = U_CAMERA_CONSENT_PROMPT_ALLOW;
		auto d = r.eval();
		CHECK(d.verdict == U_CAMERA_CONSENT_REFUSED);
		CHECK(d.why == U_CAMERA_CONSENT_WHY_PROMPT_UNAVAILABLE);
		CHECK(r.env.prompts == 0);
	}
	SECTION("an Allow the store cannot persist still allows this process")
	{
		r.env.answer = U_CAMERA_CONSENT_PROMPT_ALLOW;
		r.store.set_fails = true;
		auto d = r.eval();
		CHECK(d.verdict == U_CAMERA_CONSENT_ALLOWED);
		d = r.eval();
		CHECK(d.why == U_CAMERA_CONSENT_WHY_ALLOW_ONCE);
		CHECK(r.env.prompts == 1);
	}
}

TEST_CASE("camera consent: no store and no env still decides safely", "[camera_consent]")
{
	u_camera_consent c;
	u_camera_consent_init(&c, nullptr, nullptr, nullptr, nullptr);
	u_camera_consent_decision d;
	u_camera_consent_evaluate(&c, "/apps/caller", "Caller", 1, 0, &d);
	CHECK(d.verdict == U_CAMERA_CONSENT_REFUSED);
	CHECK(d.why == U_CAMERA_CONSENT_WHY_PROMPT_UNAVAILABLE);
	CHECK(u_camera_consent_sharing_enabled(&c));
	char id[64];
	u_camera_consent_persistent_id(&c, "serial-1", "/apps/caller", id); // per-process key
	CHECK(strncmp(id, "dxrcam-", 7) == 0);
	CHECK(strlen(id) == 7 + 32);
}

TEST_CASE("consent store: path comparison rules", "[camera_consent][consent_store]")
{
	// Windows rules: ASCII case-insensitive, either separator.
	CHECK(u_camera_consent_path_equal("C:\\Program Files\\B\\b.exe", "c:/program files/b/B.EXE", true));
	CHECK(!u_camera_consent_path_equal("C:\\B\\b.exe", "C:\\B\\b.exe2", true));
	CHECK(!u_camera_consent_path_equal("C:\\B\\b.exe", "C:\\B\\b.ex", true));
	// UTF-8 paths match byte for byte — the wide-API reader hands UTF-8 over.
	const char *u8 = "C:\\Users\\Jos\xc3\xa9\\\xe6\xb5\x8f\xe8\xa7\x88\xe5\x99\xa8\\b.exe";
	CHECK(u_camera_consent_path_equal(u8, u8, true));
	CHECK(u_camera_consent_path_equal(u8, u8, false));
	// What the ANSI API used to hand back for that path (code-page bytes) does not.
	CHECK(!u_camera_consent_path_equal("C:\\Users\\Jos\xe9\\???\\b.exe", u8, true));
	// Non-ASCII case is not folded (fails closed): E-acute vs e-acute.
	CHECK(!u_camera_consent_path_equal("/x/\xc3\x89", "/x/\xc3\xa9", true));
	// POSIX rules: byte-exact.
	CHECK(u_camera_consent_path_equal("/opt/b/browser", "/opt/b/browser", false));
	CHECK(!u_camera_consent_path_equal("/opt/b/Browser", "/opt/b/browser", false));
	CHECK(!u_camera_consent_path_equal("/opt/b\\browser", "/opt/b/browser", false));
	// Empty / NULL never match.
	CHECK(!u_camera_consent_path_equal("", "", true));
	CHECK(!u_camera_consent_path_equal(nullptr, "/a", false));
}

TEST_CASE("consent store: admin-protected folder test", "[camera_consent][consent_store]")
{
	const char *pf = "C:\\Program Files";
	CHECK(u_camera_consent_path_is_under("C:\\Program Files\\B\\b.exe", pf, true));
	CHECK(u_camera_consent_path_is_under("c:/program files/b/b.exe", "C:\\Program Files\\", true));
	// Not a separator boundary.
	CHECK(!u_camera_consent_path_is_under("C:\\Program FilesX\\b.exe", pf, true));
	CHECK(!u_camera_consent_path_is_under("C:\\Program Files (x86)\\b.exe", pf, true));
	CHECK(u_camera_consent_path_is_under("C:\\Program Files (x86)\\b.exe", "C:\\Program Files (x86)", true));
	// The folder itself is not "inside" it.
	CHECK(!u_camera_consent_path_is_under("C:\\Program Files", pf, true));
	CHECK(!u_camera_consent_path_is_under("C:\\Program Files\\", pf, true));
	// ".." climbs out.
	CHECK(!u_camera_consent_path_is_under("C:\\Program Files\\..\\Users\\u\\b.exe", pf, true));
	CHECK(!u_camera_consent_path_is_under("C:\\Program Files\\B\\..", pf, true));
	CHECK(u_camera_consent_path_is_under("C:\\Program Files\\B\\..b.exe", pf, true)); // a name, not ".."
	// Elsewhere.
	CHECK(!u_camera_consent_path_is_under("C:\\Users\\u\\AppData\\Local\\B\\b.exe", pf, true));
	CHECK(!u_camera_consent_path_is_under("D:\\Program Files\\b.exe", pf, true));
	// POSIX rules are byte-exact.
	CHECK(u_camera_consent_path_is_under("/usr/bin/x", "/usr", false));
	CHECK(!u_camera_consent_path_is_under("/USR/bin/x", "/usr", false));
	CHECK(!u_camera_consent_path_is_under("/usr\\bin", "/usr", false));
	// Empty / NULL.
	CHECK(!u_camera_consent_path_is_under("", pf, true));
	CHECK(!u_camera_consent_path_is_under("C:\\x", "", true));
	CHECK(!u_camera_consent_path_is_under(nullptr, pf, true));
	CHECK(!u_camera_consent_path_is_under("C:\\x", "\\\\", true)); // only separators
}

TEST_CASE("consent store: signer name comparison", "[camera_consent][consent_store]")
{
	CHECK(u_camera_consent_signer_equal("Browser Corp", "Browser Corp"));
	CHECK(u_camera_consent_signer_equal("Browser Corp", "  browser CORP "));
	CHECK(!u_camera_consent_signer_equal("Browser Corp", "Browser Corp Evil"));
	CHECK(!u_camera_consent_signer_equal("Browser Corp", "Browser  Corp"));
	CHECK(!u_camera_consent_signer_equal("\xc3\x89 Corp", "\xc3\xa9 Corp")); // non-ASCII not folded
	CHECK(!u_camera_consent_signer_equal("", ""));
	CHECK(!u_camera_consent_signer_equal("   ", "   "));
	CHECK(!u_camera_consent_signer_equal(nullptr, "x"));
}

namespace {

struct fake_entry
{
	u_camera_consent_enum_result r;
	std::string name, path;
};

struct fake_list
{
	std::vector<fake_entry> entries;
	uint32_t calls = 0;
	bool never_ends = false;
};

u_camera_consent_enum_result
fl_enum(void *ctx, uint32_t index, char *name, size_t name_cap, char *path, size_t path_cap)
{
	auto *l = (fake_list *)ctx;
	l->calls++;
	if (index >= l->entries.size()) {
		return l->never_ends ? U_CAMERA_CONSENT_ENUM_SKIP : U_CAMERA_CONSENT_ENUM_END;
	}
	const fake_entry &e = l->entries[index];
	if (e.r == U_CAMERA_CONSENT_ENUM_ENTRY) {
		snprintf(name, name_cap, "%s", e.name.c_str());
		snprintf(path, path_cap, "%s", e.path.c_str());
	}
	return e.r;
}

} // namespace

TEST_CASE("consent store: an oversized or malformed entry is skipped, not the end of the list",
          "[camera_consent][consent_store]")
{
	fake_list l;
	l.entries = {
	    {U_CAMERA_CONSENT_ENUM_ENTRY, "other", "C:\\Other\\other.exe"},
	    {U_CAMERA_CONSENT_ENUM_SKIP, "", ""}, // an installer's 5 KB value
	    {U_CAMERA_CONSENT_ENUM_SKIP, "", ""}, // a REG_DWORD where a path belongs
	    {U_CAMERA_CONSENT_ENUM_ENTRY, "browser.exe", "C:\\Program Files\\B\\browser.exe"},
	};
	char name[64] = "unchanged";
	CHECK(u_camera_consent_list_find(fl_enum, &l, "c:/program files/b/browser.exe", true, name, sizeof(name)));
	CHECK(std::string(name) == "browser.exe");

	SECTION("no match walks everything once and stops at END")
	{
		l.calls = 0;
		CHECK(!u_camera_consent_list_find(fl_enum, &l, "C:\\Nope\\x.exe", true, nullptr, 0));
		CHECK(l.calls == 5); // 4 entries + the END
	}
	SECTION("END ends the scan even if a match would follow")
	{
		l.entries[1].r = U_CAMERA_CONSENT_ENUM_END;
		CHECK(!u_camera_consent_list_find(fl_enum, &l, "C:\\Program Files\\B\\browser.exe", true, nullptr, 0));
	}
	SECTION("an enumerator that never ends is bounded")
	{
		l.never_ends = true;
		l.calls = 0;
		CHECK(!u_camera_consent_list_find(fl_enum, &l, "C:\\Nope\\x.exe", true, nullptr, 0));
		CHECK(l.calls == U_CAMERA_CONSENT_LIST_MAX);
	}
	SECTION("no identity never matches")
	{
		CHECK(!u_camera_consent_list_find(fl_enum, &l, "", true, nullptr, 0));
	}
}

TEST_CASE("sha256 / hmac vectors", "[camera_consent]")
{
	static const char *hex = "0123456789abcdef";
	auto to_hex = [&](const uint8_t *d, size_t n) {
		std::string s;
		for (size_t i = 0; i < n; i++) {
			s += hex[d[i] >> 4];
			s += hex[d[i] & 15];
		}
		return s;
	};
	uint8_t out[32];
	u_sha256("", 0, out);
	CHECK(to_hex(out, 32) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
	u_sha256("abc", 3, out);
	CHECK(to_hex(out, 32) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
	const char *long_msg = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
	u_sha256(long_msg, strlen(long_msg), out);
	CHECK(to_hex(out, 32) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
	// RFC 4231 test case 2.
	u_hmac_sha256("Jefe", 4, "what do ya want for nothing?", 28, out);
	CHECK(to_hex(out, 32) == "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
	// RFC 4231 test case 6: a key longer than one block.
	uint8_t key131[131];
	memset(key131, 0xaa, sizeof(key131));
	const char *m6 = "Test Using Larger Than Block-Size Key - Hash Key First";
	u_hmac_sha256(key131, sizeof(key131), m6, strlen(m6), out);
	CHECK(to_hex(out, 32) == "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
}

TEST_CASE("keyed persistentId: stable per key, unlinkable across keys and consumers", "[camera_consent]")
{
	uint8_t k1[32], k2[32];
	memset(k1, 0x11, 32);
	memset(k2, 0x22, 32);
	char a[64], b[64], c[64], d[64], e[64];
	u_camera_consent_persistent_id_keyed(k1, 32, "serial-0001", "/opt/browser", a);
	u_camera_consent_persistent_id_keyed(k1, 32, "serial-0001", "/opt/browser", b);
	u_camera_consent_persistent_id_keyed(k2, 32, "serial-0001", "/opt/browser", c);
	u_camera_consent_persistent_id_keyed(k1, 32, "serial-0001", "/opt/other", d);
	u_camera_consent_persistent_id_keyed(k1, 32, "serial-0002", "/opt/browser", e);
	CHECK(std::string(a) == b);
	CHECK(std::string(a) != c);
	CHECK(std::string(a) != d);
	CHECK(std::string(a) != e);
	CHECK(strncmp(a, "dxrcam-", 7) == 0);
	CHECK(strlen(a) == 39);
	// The serial never appears in the id.
	CHECK(strstr(a, "0001") == nullptr);
	// Length-prefixed fields: ("ab","c") != ("a","bc").
	char f[64], g[64];
	u_camera_consent_persistent_id_keyed(k1, 32, "ab", "c", f);
	u_camera_consent_persistent_id_keyed(k1, 32, "a", "bc", g);
	CHECK(std::string(f) != g);
	// The store's secret is what keys the service's id.
	rig r;
	memcpy(r.store.secret, k1, 32);
	char h[64];
	u_camera_consent_persistent_id(&r.c, "serial-0001", "/opt/browser", h);
	CHECK(std::string(h) == a);
}

TEST_CASE("client class: CAMERA_CONSUMER is outside the PRESENT_OWNER quota", "[camera_consent][client_class]")
{
	CHECK(u_client_class_counts_toward_present_owner(XRT_CLIENT_CLASS_PRESENT_OWNER));
	CHECK(!u_client_class_counts_toward_present_owner(XRT_CLIENT_CLASS_CAMERA_CONSUMER));
	CHECK(!u_client_class_counts_toward_present_owner(XRT_CLIENT_CLASS_APP));
	CHECK(!u_client_class_may_create_session(XRT_CLIENT_CLASS_CAMERA_CONSUMER));
	CHECK(u_client_class_may_create_session(XRT_CLIENT_CLASS_PRESENT_OWNER));
	CHECK(u_client_class_may_create_session(XRT_CLIENT_CLASS_APP));

	const char *chrome = "C:\\Program Files\\Browser\\browser.exe";
	const char *other = "C:\\Other\\other.exe";
	std::vector<u_client_class_peer> peers = {
	    {XRT_CLIENT_CLASS_PRESENT_OWNER, chrome},   // the GPU process
	    {XRT_CLIENT_CLASS_PRESENT_OWNER, chrome},   // the browser process
	    {XRT_CLIENT_CLASS_CAMERA_CONSUMER, chrome}, // the video-capture utility: not an owner
	    {XRT_CLIENT_CLASS_APP, other},
	};
	SECTION("one owner, siblings take no slot")
	{
		CHECK(u_client_class_present_owner_count(peers.data(), (uint32_t)peers.size(), other) == 1);
		CHECK(u_client_class_present_owner_count(peers.data(), (uint32_t)peers.size(), chrome) == 0);
	}
	SECTION("the capture utility alone never makes the browser an owner")
	{
		std::vector<u_client_class_peer> only_cam = {{XRT_CLIENT_CLASS_CAMERA_CONSUMER, chrome}};
		CHECK(u_client_class_present_owner_count(only_cam.data(), 1, other) == 0);
		CHECK(u_client_class_present_owner_count(only_cam.data(), 1, chrome) == 0);
	}
	SECTION("two distinct owners; an unknown path is its own owner and nobody's sibling")
	{
		peers.push_back({XRT_CLIENT_CLASS_PRESENT_OWNER, other});
		CHECK(u_client_class_present_owner_count(peers.data(), (uint32_t)peers.size(), "/x") == 2);
		peers.push_back({XRT_CLIENT_CLASS_PRESENT_OWNER, ""});
		CHECK(u_client_class_present_owner_count(peers.data(), (uint32_t)peers.size(), "/x") == 3);
		CHECK(u_client_class_present_owner_count(peers.data(), (uint32_t)peers.size(), "") == 3);
	}
}
