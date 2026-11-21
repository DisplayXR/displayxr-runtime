// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The `displayxr://activate` handler core (see activation_handler.h).
 * @ingroup targets
 */

#include "activation_handler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *
activation_outcome_str(enum activation_outcome_kind k)
{
	switch (k) {
	case ACTIVATION_OUTCOME_ACTIVATED: return "activated";
	case ACTIVATION_OUTCOME_REFUSED: return "refused";
	case ACTIVATION_OUTCOME_UNSUPPORTED: return "unsupported";
	case ACTIVATION_OUTCOME_NOT_LOADED: return "not-loaded";
	case ACTIVATION_OUTCOME_UNKNOWN: return "unknown";
	case ACTIVATION_OUTCOME_BUSY: return "busy";
	case ACTIVATION_OUTCOME_INVALID_LINK: return "invalid-link";
	case ACTIVATION_OUTCOME_NOT_PENDING: return "not-pending";
	case ACTIVATION_OUTCOME_ITEM_MISMATCH: return "item-mismatch";
	case ACTIVATION_OUTCOME_RATE_LIMITED: return "rate-limited";
	default: return "store-error";
	}
}

int
activation_outcome_exit_code(enum activation_outcome_kind k)
{
	return k == ACTIVATION_OUTCOME_ACTIVATED ? 0 : 1;
}

const char *
activation_class_noun(const char *cls)
{
	if (cls == NULL) {
		return "component";
	}
	if (strcmp(cls, "dp") == 0) {
		return "display processor";
	}
	if (strcmp(cls, "input") == 0) {
		return "input provider";
	}
	if (strcmp(cls, "conv") == 0) {
		return "conversion provider";
	}
	if (strcmp(cls, "tracking") == 0) {
		return "tracking provider";
	}
	return "component";
}

static enum activation_outcome_kind
outcome_of_delivery(int d)
{
	switch (d) {
	case ACTIVATION_DELIVER_OK: return ACTIVATION_OUTCOME_ACTIVATED;
	case ACTIVATION_DELIVER_REFUSED: return ACTIVATION_OUTCOME_REFUSED;
	case ACTIVATION_DELIVER_UNSUPPORTED: return ACTIVATION_OUTCOME_UNSUPPORTED;
	case ACTIVATION_DELIVER_NOT_LOADED: return ACTIVATION_OUTCOME_NOT_LOADED;
	case ACTIVATION_DELIVER_BUSY: return ACTIVATION_OUTCOME_BUSY;
	default: return ACTIVATION_OUTCOME_UNKNOWN;
	}
}

void
activation_handle_url(const char *url, const struct activation_ops *ops, struct activation_outcome *out)
{
	memset(out, 0, sizeof(*out));
	xrt_component_license_state_init(&out->licence);
	out->kind = ACTIVATION_OUTCOME_STORE_ERROR;
	if (ops == NULL || ops->deliver == NULL) {
		return;
	}

	char default_dir[1024];
	const char *dir = ops->store_dir;
	if (dir == NULL) {
		if (!u_activation_store_default_dir(default_dir, sizeof(default_dir))) {
			return; // no profile: nothing can be pending, and nothing is delivered
		}
		dir = default_dir;
	}
	const int64_t now = ops->now_unix_s != 0 ? ops->now_unix_s : u_activation_now_unix_s();

	// 1. A flood of bad links is ignored without looking at them, and without a dialog each.
	if (u_activation_store_rate_limited(dir, now)) {
		out->kind = ACTIVATION_OUTCOME_RATE_LIMITED;
		out->quiet = true;
		u_activation_store_log(dir, now, "", "", "", activation_outcome_str(out->kind));
		return;
	}

	// 2. Strict parse. The parsed link lives on the heap (it holds the token) and is wiped on every path.
	struct u_activation_url *link = (struct u_activation_url *)calloc(1, sizeof(*link));
	if (link == NULL) {
		return;
	}
	const enum u_activation_url_error perr = u_activation_url_parse(url, link);
	if (perr != U_ACTIVATION_URL_OK) {
		out->kind = ACTIVATION_OUTCOME_INVALID_LINK;
		(void)snprintf(out->detail, sizeof(out->detail), "%s", u_activation_url_error_str(perr));
		u_activation_store_note_failure(dir, now);
		u_activation_store_log(dir, now, "", "", "", out->detail);
		goto done;
	}
	u_activation_nonce_id(link->nonce, out->nonce_id);

	// 3. Consume the nonce: single use, whatever happens next.
	struct u_activation_record rec;
	const enum u_activation_store_result sr = u_activation_store_consume(dir, link->nonce, now, &rec);
	if (sr != U_ACTIVATION_STORE_OK) {
		out->kind = ACTIVATION_OUTCOME_NOT_PENDING;
		(void)snprintf(out->detail, sizeof(out->detail), "%s", u_activation_store_result_str(sr));
		u_activation_store_note_failure(dir, now);
		u_activation_store_log(dir, now, out->nonce_id, "", "", out->detail);
		goto done;
	}
	(void)snprintf(out->cls, sizeof(out->cls), "%s", rec.cls);
	(void)snprintf(out->id, sizeof(out->id), "%s", rec.id);

	// 4. The optional echo must name the component the nonce was begun for.
	//    The component is ALWAYS the record's; the link never chooses it.
	if (link->has_item && (strcmp(link->item_class, rec.cls) != 0 || strcmp(link->item_id, rec.id) != 0)) {
		out->kind = ACTIVATION_OUTCOME_ITEM_MISMATCH;
		u_activation_store_note_failure(dir, now);
		u_activation_store_log(dir, now, out->nonce_id, rec.cls, rec.id, activation_outcome_str(out->kind));
		goto done;
	}

	// 5. Deliver to that one component.
	const int d = ops->deliver(ops->userdata, rec.cls, rec.id, link->token, link->token_size, &out->licence,
	                           out->error, sizeof(out->error), out->via, sizeof(out->via));
	xrt_component_license_state_sanitize(&out->licence);
	out->error[sizeof(out->error) - 1] = '\0';
	out->via[sizeof(out->via) - 1] = '\0';
	out->kind = outcome_of_delivery(d);

	// 6. One line: nonce id, component, outcome. A component that refused a
	//    token it was legitimately handed is not a "failed link": it is not counted.
	u_activation_store_log(dir, now, out->nonce_id, rec.cls, rec.id, activation_outcome_str(out->kind));

done:
	u_activation_url_wipe(link);
	free(link);
}

//! @p s when it is displayable text (well-formed UTF-8, no control characters), else "".
static const char *
shown(const char *s)
{
	return (s != NULL && s[0] != '\0' && u_activation_text_valid(s, strlen(s))) ? s : "";
}

void
activation_outcome_message(
    const struct activation_outcome *o, char *title, size_t title_cap, char *body, size_t body_cap)
{
	static const char again[] =
	    "To try again, start the activation from the DisplayXR Dashboard. A link works once.";
	const char *noun = activation_class_noun(o->cls);
	const char *err = shown(o->error);
	const char *msg = shown(o->licence.message);
	const char *state = xrt_component_license_state_str(o->licence.state);
	(void)snprintf(title, title_cap, "%s",
	               o->kind == ACTIVATION_OUTCOME_ACTIVATED ? "DisplayXR: activation complete"
	                                                       : "DisplayXR: activation not completed");
	switch (o->kind) {
	case ACTIVATION_OUTCOME_ACTIVATED:
		(void)snprintf(body, body_cap, "The %s \"%s\" accepted the activation.\nLicence: %s%s%s", noun, o->id,
		               state, msg[0] != '\0' ? "\n" : "", msg);
		break;
	case ACTIVATION_OUTCOME_REFUSED:
		(void)snprintf(body, body_cap, "The %s \"%s\" did not accept the activation.%s%s\nLicence: %s\n\n%s",
		               noun, o->id, err[0] != '\0' ? "\n" : "", err, state, again);
		break;
	case ACTIVATION_OUTCOME_UNSUPPORTED:
		(void)snprintf(body, body_cap,
		               "The %s \"%s\" cannot be activated by a link. Enter its licence key in the DisplayXR "
		               "Dashboard instead.",
		               noun, o->id);
		break;
	case ACTIVATION_OUTCOME_NOT_LOADED:
		(void)snprintf(body, body_cap,
		               "The %s \"%s\" is not installed any more, or could not be loaded. Nothing was "
		               "activated.\n\n%s",
		               noun, o->id, again);
		break;
	case ACTIVATION_OUTCOME_UNKNOWN:
		(void)snprintf(body, body_cap,
		               "The activation was handed to the %s \"%s\", but it did not answer in time. Check its "
		               "licence state in the DisplayXR Dashboard before trying again.",
		               noun, o->id);
		break;
	case ACTIVATION_OUTCOME_BUSY:
		(void)snprintf(body, body_cap,
		               "DisplayXR was busy with another activation, so the %s \"%s\" was not activated.\n\n%s",
		               noun, o->id, again);
		break;
	case ACTIVATION_OUTCOME_NOT_PENDING:
		(void)snprintf(body, body_cap,
		               "No activation is waiting for this link on this computer. It may have expired (a link "
		               "is valid for %d minutes), been used already, or not been started from the DisplayXR "
		               "Dashboard. Nothing was activated.",
		               U_ACTIVATION_TTL_S / 60);
		break;
	case ACTIVATION_OUTCOME_ITEM_MISMATCH:
		(void)snprintf(body, body_cap,
		               "This link is for a different component than the activation that was started (%s "
		               "\"%s\"). Nothing was activated.\n\n%s",
		               noun, o->id, again);
		break;
	case ACTIVATION_OUTCOME_STORE_ERROR:
		(void)snprintf(body, body_cap,
		               "DisplayXR could not read this user's pending activations. Nothing was "
		               "activated.");
		break;
	case ACTIVATION_OUTCOME_RATE_LIMITED:
		(void)snprintf(body, body_cap,
		               "Too many activation links failed in the last few minutes, so this one was ignored. "
		               "Wait %d minutes, then start the activation again from the DisplayXR Dashboard.",
		               U_ACTIVATION_FAIL_WINDOW_S / 60);
		break;
	case ACTIVATION_OUTCOME_INVALID_LINK:
	default:
		(void)snprintf(body, body_cap, "This is not a valid DisplayXR activation link. Nothing was activated.");
		break;
	}
}
