# Activation deep link (`displayxr://activate`)

**Status:** implemented (ADR-064 step 7). Windows registration and the browser
hop on every platform are validated by the checklist in §11, not by CI.

How a vendor's checkout page hands an activation token back to a licensed
component on the user's machine. This spec is the contract for three readers:
a **vendor** building a checkout page (§4), a **front end** starting an
activation (§3, the Dashboard), and whoever reviews the **handler**, which is
reachable from any web page or document on the machine (§5, §9).

Background: [ADR-064 §5](../../adr/ADR-064-component-catalog-and-third-party-trust.md)
(the licence model), [plugin-discovery.md §2.4](plugin-discovery.md) (the
licence-state and `activate` slots every component class shares).

## 1. Scope, and what needs no link at all

Install is free. A vendor gates only the licence, inside its own binary, and
the runtime carries a licence *state* and an opaque *token*; it decides
neither.

Most licensed components never use this flow:

| The component's licence `source` | What the user does | This spec |
|---|---|---|
| `hardware` (the connected display or device is the entitlement), `oem` | nothing. The component reports `licensed` by itself, and the Dashboard shows "Included with your display" | not involved: no nonce, no link, no handler |
| `key` (a purchased licence) | buys on the vendor's page, comes back through the link; or pastes a key | this flow, or the pasted key (§1.1) |
| `trial` | as `key`, when the trial ends or the user buys early | this flow, or the pasted key |

**Hardware entitlement is the primary case and is zero-click.** The link, the
handler and the Buy / Activate action exist for `key` and `trial` sources.

### 1.1 The other activation path: a pasted key

A component may also take a key the user types: a setting of type `secret`
(masked input in the Dashboard, stored through the settings contract, never
returned by any surface), or `displayxr-cli <class> activate <id> <key>`.
**That is where a reusable licence key belongs.** A reusable key never belongs
in a URL (§4.3).

## 2. The flow

```
 Dashboard                 vendor's page                browser + OS            handler               component
 ---------                 -------------                ------------            -------               ---------
 activate-begin <id>
   nonce N (15 min, this user, this component)
   opens purchase_url?displayxr_nonce=N&...  ───────►
                           user pays / signs in
                           navigates to
                           displayxr://activate?nonce=N&token=T  ──►  starts the registered handler
                                                                       with the link as ONE argument
                                                                                 strict parse
                                                                                 consume N (single use)
                                                                                 N was begun for <id>  ──► activate(T)
                                                                                 show the outcome   ◄──── new licence state
```

The nonce is what makes the link safe to expose: a link whose nonce is not
pending **on this machine, for this user, right now** does nothing. The
component that receives the token is the one the nonce was begun for; nothing
in the link can choose another.

## 3. Starting an activation: `activate-begin`

```
displayxr-cli <dp|input|conv|tracking> activate-begin <id> [--json]
```

Creates one pending activation and prints the URL to open. It opens nothing
and fetches nothing: the caller opens the URL in the user's browser. Refused
unless the component is registered, advertises deep-link activation
(`activation.deep_link` in its licence state) and reports a usable purchase
URL.

`--json` (stable; this is what the Dashboard's Buy / Activate action reads):

```json
{
  "class": "conv",
  "id": "vendor-depth",
  "result": "ok",
  "url": "https://vendor.example/buy?displayxr_nonce=…&displayxr_item=conv%2Fvendor-depth&displayxr_return=displayxr%3A%2F%2Factivate",
  "return_url": "displayxr://activate",
  "nonce_id": "790fa3af",
  "ttl_s": 900,
  "expires_unix": 1795263506,
  "expires": "2026-11-21T12:18:26Z",
  "licence": { "state": "unlicensed", "…": "the component's licence object, as in `list --json`" }
}
```

| `result` | Meaning | Exit |
|---|---|---|
| `ok` | a nonce is pending; open `url` | 0 |
| `not-registered` | no such component of that class | 1 |
| `no-deep-link` | the component does not advertise deep-link activation (offer the pasted key instead) | 1 |
| `no-purchase-url` | it reports no purchase URL, or one that is not acceptable (below) | 1 |
| `too-many` | 16 activations are already pending for this user | 1 |
| `store-error` | the user's pending store is not usable | 1 |

Unless `ok`, `url` and `nonce_id` are `""` and the two times are `null`.
`nonce_id` is a loggable id (it is what the handler's log shows); it is not the
nonce, which appears only inside `url`.

**The purchase URL is checked, not trusted.** It comes from a third-party
binary and is about to be opened in a browser, so `activate-begin` accepts
only `https://<host>…` made of printable ASCII with no space, quote,
backslash, angle bracket, backtick, brace, caret or pipe. A front end must
open `url` only as an `https` URL in the default browser, never through a
shell.

### 3.1 The pending record

| Property | Value |
|---|---|
| Nonce | 32 bytes from the OS random source, base64url without padding: 43 characters of `A-Z a-z 0-9 - _` |
| Bound to | one component (class + id) and the user who began it |
| Lifetime | 15 minutes (`U_ACTIVATION_TTL_S`) |
| Uses | one. The record is deleted by the first presentation of its nonce, whatever the outcome |
| Limit | 16 pending per user (`U_ACTIVATION_MAX_PENDING`); expired ones are pruned at every begin |
| Windows | `%LOCALAPPDATA%\DisplayXR\Activation\` (the profile's ACL) |
| macOS | `~/Library/Application Support/DisplayXR/activation/` (directory `0700`, files `0600`) |
| Linux | `$XDG_STATE_HOME/displayxr/activation/`, else `~/.local/state/displayxr/activation/` (`0700` / `0600`) |

On disk a record is a small file named after the **SHA-256 of the nonce** (the
nonce itself is never written), holding the class, the id, the creating user
(the SID on Windows, the uid on POSIX) and the two times. No token is ever
written: the store is consulted before the token is looked at and never sees
it. On POSIX the directory and the record must be owned by the caller and
closed to group and others, or they are refused.

The store is per user and needs no service. (ADR-064's step table sketched
nonce start / consume as RPCs on the service's DIAG channel; a per-user store
has the same properties, keeps working when no service runs, and does not add
a token-bearing state machine to the service for the part that needs none.)

## 4. What a vendor implements

### 4.1 What your page receives

`activate-begin` appends three parameters to your `purchase_url` (to its
existing query if it has one, before any fragment), each percent-encoded:

| Parameter | Value | What you do with it |
|---|---|---|
| `displayxr_nonce` | 43 characters, `A-Z a-z 0-9 - _` | echo it back, unchanged, as `nonce` |
| `displayxr_item` | `<class>/<id>` with the `/` encoded (`conv%2Fvendor-depth`); class is `dp`, `input`, `conv` or `tracking` | tells you which of your components is being activated; you may echo it as `item` |
| `displayxr_return` | `displayxr://activate` (encoded) | the link to return through; treat it as a constant |

Carry the nonce through your checkout (it is not a secret from you, and it is
useless to anyone else: it only works on the machine that created it, once,
for fifteen minutes). You do not need to store it after returning.

### 4.2 What your page sends back

Navigate the user's browser to:

```
displayxr://activate?nonce=<displayxr_nonce>&token=<token>[&item=<class>%2F<id>]
```

Grammar (RFC 3986 character classes):

```
link       = "displayxr://activate" [ "/" ] "?" param *( "&" param )
param      = "nonce=" nonce / "token=" token / "item=" item
nonce      = 43( ALPHA / DIGIT / "-" / "_" )
token      = 1*( unreserved / pct-encoded )
item       = class ( "/" / "%2F" ) id
class      = "dp" / "input" / "conv" / "tracking"
id         = 1*63( ALPHA / DIGIT / "." / "_" / "-" )
unreserved = ALPHA / DIGIT / "-" / "." / "_" / "~"
```

Rules, all enforced by the handler; a link that breaks one is refused whole:

- `activate` is the only action. The scheme name is matched without regard to
  case; `activate` is exact. One trailing `/` after it is tolerated.
- `nonce` and `token` are required, `item` is optional; each appears at most
  once, in any order. **Any other parameter makes the link invalid**; do not
  add tracking parameters.
- The whole link is at most 6400 bytes.
- **Percent-encode every byte of the token that is not `A-Z a-z 0-9 - . _ ~`.**
  This is stricter than `encodeURIComponent`, which leaves `! * ' ( )` bare,
  and `+` is not a space. In JavaScript:

  ```js
  const enc = s => Array.from(new TextEncoder().encode(s),
      b => /[A-Za-z0-9\-._~]/.test(String.fromCharCode(b))
          ? String.fromCharCode(b) : '%' + b.toString(16).toUpperCase().padStart(2, '0')).join('');
  location.href = `displayxr://activate?nonce=${nonce}&token=${enc(token)}`;
  ```

- The decoded token is 1 to 2047 bytes of well-formed UTF-8 with no control
  character (U+0000–U+001F, U+007F–U+009F). A base64url string, a JWT or an
  alphanumeric code all qualify as they are.
- No user-info, port, fragment, space, quote or raw non-ASCII byte anywhere.
- If you send `item`, it must be exactly the `displayxr_item` you received.
  It is a cross-check, not a selector: a mismatch refuses the link.

Trigger the navigation from a user gesture (a "Return to DisplayXR" button),
because browsers ask the user before starting an external program and some
block a script-initiated attempt. Show the user a fallback on the same page
(§4.4): the link works once and only on the machine that started it.

### 4.3 The token: a one-time redemption code, not a licence key

**The `token` SHOULD be a one-time redemption code that your plug-in exchanges
with your own service for the real licence. It SHOULD NOT be a reusable
licence key.**

A URL is not a private channel. It can end up in the browser's history, in a
`Referer` header, in the operating system's logs of launched programs, in
crash reports and in the process list while the handler runs. DisplayXR's half
is covered: the nonce is single use, the handler writes neither the token nor
the link anywhere, and it hands the token to exactly one component, once (§5,
§6). What the token is *worth* afterwards is yours to decide, so:

- make it redeemable once, soon (minutes), and bind it to the purchase; a
  replayed or leaked link then yields nothing;
- have your plug-in's `activate` redeem it against your service and keep the
  resulting licence in **your own storage** (the slot may block for a few
  seconds; it is never called on a render or IPC thread);
- put a **reusable** key only on the pasted-key path (§1.1): a `secret`
  setting, masked on entry and never printed, or the `activate` verb. Never in
  a URL.

Nothing in the runtime makes a URL token reusable, and nothing in it can make
a reusable one safe.

### 4.4 What your plug-in implements

In its `get_license_state` answer: `purchase_url` (an `https` URL, under 256
bytes) and the `XRT_COMPONENT_LICENSE_ACTIVATION_DEEP_LINK` flag. In its
`activate` slot: accept the token, return the new state, or refuse with one
line the user will be shown. The token arrives exactly as your page sent it
(after percent-decoding), at most once per link.

Keep the activation yourself: a link may be delivered to your plug-in in a
short-lived process (§6), and every later process must find it licensed.

Always offer a way that does not need the link: the user may be on another
machine, the browser may refuse to start the handler, or the fifteen minutes
may have passed. Show the code to paste, or let the user start again from the
Dashboard.

Reference implementation: the scripted licence of the in-tree sim providers
(`src/xrt/drivers/sim_common/sim_license.h`; `LICENSE_SIM`, `LICENSE_STORE`),
which a round trip can be run against without hardware (§10).

## 5. The handler

One implementation, `activation_handle_url()`
(`src/xrt/targets/activate/activation_handler.c`), behind every platform's
registered entry (§8). In this order and no other:

1. **Flood check.** If five links failed in the last ten minutes for this
   user, stop: the link is not looked at, nothing is spent, nothing is shown.
2. **Strict parse** (§4.2). An allow-list parser over a closed character set
   with hard length caps; a refused link yields no field at all.
3. **Consume the nonce.** The record is claimed with an atomic rename and
   deleted, *then* judged: not found, expired, begun by another user, or not a
   record this code wrote are all refusals, and the record is gone in every
   case. Of two handlers racing on one nonce, exactly one claim succeeds.
4. **Cross-check `item`**, if present, against the record.
5. **Deliver** the token to the record's component (§6), then wipe it.
6. **Record** one line: time, nonce id, class, id, outcome. Never the token,
   never the link (`activation.log` in the store directory, capped at 64 KB).
7. **Show the outcome** (below).

What it never does: open, execute or forward any part of the link; pass any of
it to a shell; start the service; ask for elevation; choose a component from
the link.

Failures that count toward the flood limit: an invalid link, a nonce that is
not pending, an `item` mismatch. A component refusing a token it was
legitimately handed does not count.

### 5.1 The user always sees the outcome

There is no silent activation. After a link is handled the user is shown
which component it concerned and what happened, in a minimal native message:

| Platform | Message |
|---|---|
| Windows | one message box from `displayxr-activate.exe` |
| macOS | one alert from `DisplayXRActivate.app` |
| Linux | a desktop notification (`notify-send`; `zenity` or `kdialog` when absent). When none of the three exists the handler stops **before** looking at the link, so nothing is activated unseen |

The text is fixed sentences plus the component's id **from the pending
record**, its licence state, and its own one-line message when that is
well-formed text. Nothing from the link is ever shown. The one exception to
"always" is the flood state of step 1: a hostile page must not be able to
raise a dialog per attempt, so links ignored for that reason are ignored
quietly (and logged).

### 5.2 `activate-url`

```
displayxr-cli activate-url [--json] [--notify] <link>
```

The same handler from a terminal, and the registered entry on Linux. The link
is the **last** argument and is never read as an option; the only options are
`--json` and `--notify`, and only before it; any other argument is refused
before the link is looked at.

`--json`:

```json
{ "result": "activated", "accepted": true, "quiet": false,
  "title": "DisplayXR: activation complete",
  "message": "The conversion provider \"vendor-depth\" accepted the activation.\nLicence: licensed",
  "detail": "", "class": "conv", "id": "vendor-depth", "nonce_id": "790fa3af",
  "via": "service", "error": "", "licence": { "state": "licensed", "source": "key" } }
```

| `result` | Meaning |
|---|---|
| `activated` | the component accepted the token (exit 0; every other result exits 1) |
| `refused` | it refused; `error` is its line |
| `unsupported` | it has no activation slot |
| `not-loaded` | it is not registered any more, or did not load |
| `unknown` | the service took the token and did not report back in time; check the licence state before retrying |
| `busy` | the service was busy with another activation for 10 s; nothing was delivered |
| `invalid-link` | not a well-formed link; `detail` says which rule (`bad-scheme`, `bad-action`, `bad-query`, `bad-nonce`, `bad-token`, `bad-item`, `bad-character`, `too-long`, `empty`) |
| `not-pending` | no live pending activation; `detail` is `not-found`, `expired`, `wrong-user` or `corrupt` |
| `item-mismatch` | the link's `item` is not what the nonce was begun for |
| `rate-limited` | the flood limit is in force; `quiet` is true |
| `store-error` | the user's pending store could not be located |

`class` and `id` name the component the nonce was begun for, and are `""` when
the link never resolved to one. `via` is `service` or `process` (§6).

## 6. Delivery: exactly one `activate` call

A token may be single use at the vendor's service, so a link results in **at
most one** call of the component's `activate`.

**1. A live instance, through the running service.** When a service is running
and hosts a live instance of the component, the service calls that instance's
slot, so the running instance learns its new state:

| Class | Live instance | What follows an accepted token |
|---|---|---|
| conversion | the provider in the service, or in its satellite host (the host channel's `ACTIVATE` message) | the core re-describes the provider at once: capabilities that read `UNLICENSED` become selectable without a restart |
| display | the active display processor | the same re-probe a preference change asks for |
| input | a resident provider | a policy re-read and re-rank |
| tracking | none (§6.4) | — |

**2. Otherwise, in the handler's process.** No service, no live instance
there, or the tracking class: the component's binary is loaded (negotiate
only, through the class loader and its provenance gate), its slot is called,
and it is unloaded. A component keeps its activation across processes itself;
a running service is then asked to re-select the class
(`system_request_component_reselect`).

Step 2 runs only when step 1 reported that **nothing was called**. Once the
service has taken the token its answer is final, and a missing answer is
reported as `unknown`, never retried.

### 6.1 The DIAG calls

A token (up to 2047 bytes) does not fit one IPC message, and the component's
`activate` may block, so it never runs on an IPC thread:

| Call | Does |
|---|---|
| `system_component_activate_put(chunk)` | stage up to 768 bytes at an offset; offset 0 starts over |
| `system_component_activate_commit(class, id, size)` | check the staging is whole and well-formed, then start the delivery on the service's activation worker thread |
| `system_component_activate_poll()` | `PENDING`, then once `DONE` (result, licence state, error line) or `NOT_HOSTED` |

DIAG only (the caller's executable lives in the service's own directory,
[ipc-client-classes.md](ipc-client-classes.md)). None of the three blocks. The
slot is one, service-wide: a second caller is told `BUSY` and nothing of its is
taken. The staging buffer is zeroed when the delivery ends, when a staging is
discarded and when the staging connection closes; the token is never logged
(the service logs class, id, token size and outcome).

These calls do not validate a nonce: that happened in the handler, which is
the only shipped caller. They add no capability a local process running as the
user does not already have (it can load the same component and call its slot);
the nonce protects against a *web page*, which cannot reach the IPC channel.

### 6.2 Bounds

| Wait | Bound |
|---|---|
| a busy activation slot | retried for 10 s, then `busy` |
| the service's answer | 45 s, then `unknown` |
| the conversion host's answer to `ACTIVATE` | 30 s (the proxy), reported as refused |
| service shutdown with a delivery still inside a component | 2 s grace, then abandoned |
| macOS: the CLI run by the handler bundle | 75 s, then ended |

### 6.3 What the runtime persists

Nothing about a licence. The pending record (§3.1) is deleted on first use,
and the log holds no token. The component owns its licence storage: its own
files, its vendor's service, or its `secret` setting.

### 6.4 Known gap: the tracking host

A viewer-tracking provider runs in `displayxr-tracking-host`, chosen once per
service run, and that host has no control channel (it publishes a shared
page). Delivery is therefore always in the handler's process, and the provider
serves licensed from the **next service start**. The same holds for a display
processor that was skipped as unlicensed (it is not resident; it is adopted at
the next re-probe / session by the existing rule, plugin-discovery.md §4.2).

## 7. The reference provider

`sim-conversion` scripts the whole contract (`src/xrt/drivers/sim_conversion/README.md`):

| Setting | Effect |
|---|---|
| `LICENSE_SIM=unlicensed` | reports `unlicensed`, a purchase URL, and both activation methods |
| `LICENSE_STORE=<file>` | its own licence storage: an accepted activation is recorded there (a marker, never the key) and read back at load |

The fixed test token is `DXR-SIM-TEST-KEY`. With both settings, a round trip
needs no service and no hardware (§10).

## 8. Platform registration

| Platform | Registered as | Handler | By |
|---|---|---|---|
| Windows | `HKLM\Software\Classes\displayxr` (`URL Protocol`), `shell\open\command` = `"<dir>\displayxr-activate.exe" "%1"` | `displayxr-activate.exe` | the runtime installer; removed by its uninstaller when it still points at that install |
| macOS | `CFBundleURLTypes` → `displayxr` in the bundle's `Info.plist`, registered with Launch Services by the `.pkg` postinstall (`lsregister -f`) for the console user | `DisplayXRActivate.app` in the runtime's `bin/` | the runtime `.pkg`; `uninstall.sh` unregisters it |
| Linux | `displayxr-url-handler.desktop` with `MimeType=x-scheme-handler/displayxr;` and `Exec=<bin>/displayxr-cli activate-url --notify %u` | `displayxr-cli` | the `.deb` (`/usr/share/applications`, refreshed by the `desktop-file-utils` trigger and postinst) and the tarball's `install.sh` (`$XDG_DATA_HOME/applications`, or `/usr/local/share/applications` with `--system`) |

**Windows: a dedicated program.** `displayxr-activate.exe` is a GUI-subsystem
program with no options and one action. It takes its command line from
`GetCommandLineW` / `CommandLineToArgvW` and accepts **exactly one** argument:
the classic scheme-handler injection (a link containing `" --flag`) produces
more than one and is refused before anything is parsed. It has the default
`asInvoker` manifest (it runs as the user who clicked and never asks for
elevation) and removes the current directory from the DLL search path before
anything loads. The registered command is `"<exe>" "%1"`: both quoted, nothing
after `%1`. `displayxr-cli activate-url` was not registered instead because
the CLI is a console program (a console window would flash over the browser on
every link) with many verbs and flags.

**macOS: a bundle, because the platform requires one.** A URL scheme belongs
to an app bundle and the link arrives as an Apple Event, never as an argument,
so a bare executable cannot be the handler. `DisplayXRActivate.app` is a
shim: `LSUIElement`, system frameworks only, no entitlements. It runs the
`displayxr-cli` beside it (`activate-url --json <link>`, an argument vector,
no shell, found only beside the bundle) and shows the `title` / `message` it
reports. The CLI is the process that loads plug-ins and already carries the
signing entitlements for that. Launch Services does not register a bundle
that lives under a temporary directory.

**Linux: the CLI.** There is no console to flash and a `.desktop` entry is the
registration, so the CLI is the handler. `%u` is one argument, last.

## 9. Threat model

The handler is started by the browser with attacker-chosen input whenever any
page navigates to `displayxr://…` (browsers ask the user first; assume they
say yes).

| Threat | Mitigation |
|---|---|
| A page pushes a token at a plug-in | no pending nonce → nothing reaches a plug-in. A nonce is 256 bits of OS randomness, lives 15 minutes, works once, on one machine, for one user |
| A page picks which component gets a token | the component comes from the pending record; `item` is only a cross-check |
| A page triggers another action (`displayxr://install`, `…/open`) | the action allow-list is `activate`; there is no other |
| Argument injection through the registered command (`" --flag`, spaces, `%1` tricks) | Windows: `"%1"` quoted and a handler that accepts exactly one argument and has no options. Linux: the link is the last argument and never an option. macOS: an Apple Event, then an argument vector. The link's character set excludes quote, space and backslash in any case |
| Shell or command injection | no part of the link reaches a shell, a command line of ours beyond the one argument, a file name (records are named by a hash) or a format string |
| Malformed input: oversized, `%00`, control characters, invalid or overlong UTF-8, unknown or repeated keys | the strict parser (§4.2); table-tested |
| Replay of a captured link | the nonce is spent by first use, valid or not |
| A link leaks (history, `Referer`, logs) | our half is single use and writes nothing; the vendor's half is §4.3 |
| Another local user's pending activation | the store is per user; a record names its user and is refused for any other; POSIX also checks owner and mode |
| A hostile page floods the handler (dialog spam, nonce guessing) | five failures in ten minutes stop the handler answering, quietly; the counter is bounded |
| A hostile page burns a legitimate pending activation | it would need the nonce. Failed links spend only the nonce they name. A flood can make the handler ignore links for up to ten minutes (the pasted key is unaffected) |
| The token appears in a log, a crash line or a UI | never logged or shown; wiped from the handler's and the service's buffers after delivery |
| A hostile purchase URL from a plug-in | `https` only, closed character set (§3); the front end opens it in the browser, never a shell |
| Elevation | the handler never asks for it; a browser running elevated runs the handler elevated, where the component loader's provenance gate still applies |
| DLL planting next to the browser's working directory (Windows) | the handler drops the current directory from the DLL search path first |
| A second program takes the scheme | out of scope for the handler; the installer writes the registration, and uninstall removes it only when it still points at that install |

Outside the model: code already running as the user (it can call a
component's slot directly) and an administrator.

**Not DRM.** Nothing here protects a licence. The nonce protects the *user*
from pages that would push data at a plug-in; entitlement is the vendor's
check, in the vendor's binary.

## 10. Trying it without hardware

```
displayxr-cli conv set sim-conversion ENABLE 1
displayxr-cli conv set sim-conversion LICENSE_SIM unlicensed
displayxr-cli conv set sim-conversion LICENSE_STORE <a file path>
displayxr-cli conv list                              # UNLICENSED, selected for nothing
displayxr-cli conv activate-begin sim-conversion     # prints the URL; note displayxr_nonce
displayxr-cli activate-url "displayxr://activate?nonce=<nonce>&token=DXR-SIM-TEST-KEY"
displayxr-cli conv list                              # licensed (key), selected again
```

Tests: `tests/tests_activation.cpp` (the parser's table of hostile links, the
store, the handler end to end against a scripted component, the service's
activation slot), `tests_conv_lanes` *"a live activation licenses a provider
and the task re-selects it"* (the delivery to a live conversion provider), and
the conformance cell in `build-windows.yml` (the round trip on the real
loaders and the real per-user store, the flood limit, the handler program's
subsystem, manifest and exit code).

## 11. Validation checklist (not covered by CI)

CI builds the handler program and runs the round trip through the CLI. It does
not exercise the registration or a browser. On a Windows box with the
installer from the branch:

1. **Registration.** `reg query HKLM\Software\Classes\displayxr /s` shows
   `URL Protocol` and `shell\open\command` = `"<InstallPath>\displayxr-activate.exe" "%1"`.
   Present in the 32-bit view too (`/reg:32`).
2. **Round trip from a browser.** Set up §10 through `LICENSE_STORE`, run
   `activate-begin`, then open a local HTML page with a button that navigates
   to the link (and once more by typing the link in the address bar). Expect
   the browser's "open DisplayXR Activation?" prompt, no console window, one
   message box naming `sim-conversion` with `Licence: licensed`, and
   `conv list` reading licensed. Repeat in Edge, Chrome and Firefox.
3. **With the service running** (`sim-conversion` hosted by it): the same,
   and `activate-url --json` reports `"via": "service"`; `conv list` (served by
   the service) turns licensed without a restart.
4. **Replay.** Click the same link again: "No activation is waiting…".
5. **Hostile links**, each from a page and from `Win+R`; none may reach a
   component, and none may show anything from the link:
   - `displayxr://open?nonce=…&token=x` and `displayxr://install/x` (action)
   - `displayxr://activate?nonce=<valid>&token=a" --json "b` (quote injection)
   - `displayxr://activate?nonce=<valid>&token=a%00b`, `…token=%C0%AF`, `…token=%0D%0A`
   - `displayxr://activate?nonce=<valid>&token=x&next=calc.exe` (unknown key)
   - `displayxr://activate?nonce=<valid>&token=` followed by 10 000 characters
   - `displayxr://activate?nonce=<guessed 43 characters>&token=x`
   - `displayxr://activate?nonce=<valid>&token=x&item=dp%2Fsim-display` (mismatch; this one spends the nonce)
   - `displayxr-activate.exe` started by hand with zero and with three arguments
   After the fifth failure further links produce **no** dialog for ten minutes
   and `%LOCALAPPDATA%\DisplayXR\Activation\activation.log` gains
   `outcome=rate-limited` lines; delete `failures` there to reset.
6. **Another user.** Begin as user A; as user B (same machine) open the link:
   "No activation is waiting…", and A's record is untouched.
7. **Expiry.** Begin, wait 16 minutes, open the link: refused.
8. **No elevation, no leftovers.** Task Manager shows `displayxr-activate.exe`
   at the browser's integrity level, gone after the message box. The log holds
   no token and no nonce; no `.pending` file remains.
9. **Uninstall** removes `HKLM\Software\Classes\displayxr` and the program.

macOS: after installing the `.pkg`, `open 'displayxr://activate?nonce=x&token=y'`
shows the alert ("not a valid… link"); then steps 2, 4 and 5. Linux:
`xdg-mime query default x-scheme-handler/displayxr` names
`displayxr-url-handler.desktop`; `xdg-open '<link>'` raises a notification;
then steps 2, 4 and 5.
