#!/bin/bash
# macos_sign_notarize.sh — Developer ID signing + notarization for DisplayXR
# macOS packages (#280). One script, reused by every repo that ships a .pkg
# (runtime, MCP, shell, demos, bundle): copy it verbatim, or fetch it from
# displayxr-runtime at a pinned tag.
#
# Modes
#   sign-tree <dir> [--entitlements <glob>=<plist>]... [--entitlements-map <file>]
#       Sign every Mach-O under <dir>, innermost first: dylibs and bundles,
#       then executables, then .app bundles (deepest first). Each file is
#       signed individually; `--deep` is only ever used to VERIFY. Run it
#       LAST, after every install_name_tool / lipo / strip: each of those
#       invalidates the signature, and an invalid signature on arm64 is an
#       immediate SIGKILL at load.
#       <glob> is a shell pattern matched against the path relative to <dir>
#       (e.g. '*/bin/displayxr-service'). A map file holds one
#       '<glob> <plist>' pair per line ('#' comments; a relative plist path
#       resolves against the map file's directory).
#   sign-pkg <in.pkg> [out.pkg]
#       productsign with the Installer identity (in place when out is omitted),
#       then `pkgutil --check-signature`.
#   notarize <file.pkg|.zip|.dmg>
#       notarytool submit --wait; fails (and prints `notarytool log`) unless
#       the status is Accepted.
#   staple <file.pkg|.dmg|.app>
#       stapler staple + validate, then the Gatekeeper assessment
#       (`spctl --assess`) must report "Notarized Developer ID".
#   finalize-pkg <file.pkg>
#       sign-pkg + notarize + staple, each step only when its credentials
#       are present. What a package build script calls as its last step.
#   verify-pkg <file.pkg>
#       Assert a finished .pkg is Developer-ID signed, notarized and stapled:
#       pkgutil --check-signature, stapler validate, and spctl --assess
#       --type install reporting "source=Notarized Developer ID".
#   status
#       Print which mode each step will run in. Prints no secret.
#
# CI modes (GitHub Actions; the org-level secrets below arrive as env vars)
#   ci-secrets-check
#       Exit 0 when every CI signing secret is non-empty, else print the
#       NAMES (never values) of the missing ones and exit 1. The workflow's
#       gate: absent secrets = an unsigned build, not a failed one.
#   ci-keychain-setup [--env-out <file>]
#       Create a throwaway keychain (random password) under $RUNNER_TEMP,
#       import both .p12 identities + Apple's Developer ID intermediates,
#       allow codesign/productsign to use the keys without a prompt, put the
#       keychain on the user search list, write the .p8 API key to a 0600
#       file, and emit the DXR_* configuration above as KEY=VALUE lines to
#       <file> (pass "$GITHUB_ENV") or stdout. Fails (non-zero) if either
#       Team-ID-matching identity is not found after import.
#   ci-keychain-teardown
#       Restore the search list, delete the keychain, delete the key files.
#       Idempotent; safe to run when setup never ran (`if: always()`).
#
#   Secrets (names fixed at the DisplayXR org level, #280):
#     MACOS_APP_CERT_P12            base64 of the "Developer ID Application" .p12
#     MACOS_APP_CERT_PASSWORD       its export password
#     MACOS_INSTALLER_CERT_P12      base64 of the "Developer ID Installer" .p12
#     MACOS_INSTALLER_CERT_PASSWORD its export password
#     APPLE_TEAM_ID                 10-char Team ID; both identities must match
#     APPLE_NOTARY_KEY_P8           raw text of the App Store Connect API key
#                                   (AuthKey_XXXX.p8, BEGIN/END lines included)
#     APPLE_NOTARY_KEY_ID           that key's Key ID
#     APPLE_NOTARY_ISSUER_ID        the App Store Connect Issuer ID (UUID)
#   (`base64 -i cert.p12 | pbcopy` produces the P12 form.)
#
# Configuration (environment only; nothing here is hard-coded)
#   DXR_MACOS_APP_IDENTITY        "Developer ID Application: <Org> (<TEAM>)" or
#                                 its SHA-1. Unset = ad-hoc signing ('-'), which
#                                 is exactly what contributor and fork-PR builds
#                                 have always done: unsigned, and green.
#   DXR_MACOS_INSTALLER_IDENTITY  "Developer ID Installer: <Org> (<TEAM>)".
#                                 Unset = the .pkg stays unsigned.
#   DXR_MACOS_KEYCHAIN            optional keychain path to search (CI
#                                 temporary keychain); default = search list.
#   DXR_NOTARY_PROFILE            notarytool keychain profile
#                                 (`xcrun notarytool store-credentials`), or
#   DXR_NOTARY_KEY_PATH / DXR_NOTARY_KEY_ID / DXR_NOTARY_ISSUER
#                                 an App Store Connect API key (takes
#                                 precedence over the profile).
#   DXR_MACOS_NOTARIZE=0          sign, but skip notarization (local dry runs).
#   DXR_CI_SIGN_DIR               CI modes' work dir (keychain, key files);
#                                 default ${RUNNER_TEMP:-$TMPDIR}/dxr-macos-sign.
#
# The hardened runtime (--options runtime) and a secure timestamp are applied
# whenever a real identity is used; notarization rejects anything without them.
set -euo pipefail
# Never `set -x` in this file: the CI modes handle .p12 passwords and the key.

APP_ID="${DXR_MACOS_APP_IDENTITY:-}"
INST_ID="${DXR_MACOS_INSTALLER_IDENTITY:-}"
KEYCHAIN="${DXR_MACOS_KEYCHAIN:-}"

log()  { echo "[macos-sign] $*"; }
die()  { echo "[macos-sign] ERROR: $*" >&2; exit 1; }

keychain_args() { [ -n "$KEYCHAIN" ] && printf '%s\n' --keychain "$KEYCHAIN"; return 0; }

notary_creds() {
    # Print notarytool credential args, one per line; return 1 when none.
    if [ -n "${DXR_NOTARY_KEY_PATH:-}" ]; then
        [ -n "${DXR_NOTARY_KEY_ID:-}" ] && [ -n "${DXR_NOTARY_ISSUER:-}" ] \
            || die "DXR_NOTARY_KEY_PATH is set but DXR_NOTARY_KEY_ID / DXR_NOTARY_ISSUER are not"
        printf '%s\n' --key "$DXR_NOTARY_KEY_PATH" --key-id "$DXR_NOTARY_KEY_ID" --issuer "$DXR_NOTARY_ISSUER"
    elif [ -n "${DXR_NOTARY_PROFILE:-}" ]; then
        printf '%s\n' --keychain-profile "$DXR_NOTARY_PROFILE"
    else
        return 1
    fi
}

notary_mode() {
    if [ "${DXR_MACOS_NOTARIZE:-1}" = "0" ]; then echo "skipped (DXR_MACOS_NOTARIZE=0)"
    elif [ -n "${DXR_NOTARY_KEY_PATH:-}" ]; then echo "API key (env)"
    elif [ -n "${DXR_NOTARY_PROFILE:-}" ]; then echo "keychain profile '$DXR_NOTARY_PROFILE'"
    else echo "none (not notarized)"; fi
}

is_macho() { file -b "$1" 2>/dev/null | grep -q 'Mach-O'; }
is_macho_exec() { file -b "$1" 2>/dev/null | grep -q 'Mach-O.*executable'; }

# ---------------------------------------------------------------- sign-tree --
ENT_GLOBS=()
ENT_PLISTS=()

add_entitlement() {
    local spec="$1" base="${2:-}"
    local glob="${spec%%=*}" plist="${spec#*=}"
    [ "$glob" != "$spec" ] || die "entitlements spec must be <glob>=<plist>: $spec"
    if [ -n "$base" ] && [ "${plist#/}" = "$plist" ]; then plist="$base/$plist"; fi
    [ -f "$plist" ] || die "entitlements plist not found: $plist"
    plutil -lint -s "$plist" || die "invalid entitlements plist: $plist"
    ENT_GLOBS+=("$glob")
    ENT_PLISTS+=("$plist")
}

entitlements_for() {
    local rel="$1" i
    [ ${#ENT_GLOBS[@]} -gt 0 ] || return 0
    for i in "${!ENT_GLOBS[@]}"; do
        # shellcheck disable=SC2053  # glob match is the point
        if [[ "$rel" == ${ENT_GLOBS[$i]} ]]; then echo "${ENT_PLISTS[$i]}"; return 0; fi
    done
    return 0
}

sign_one() {
    local path="$1" rel="$2" ent
    local args=(--force)
    if [ -z "$APP_ID" ]; then
        args+=(--sign -)
    else
        args+=(--options runtime --timestamp --sign "$APP_ID")
        while IFS= read -r a; do args+=("$a"); done < <(keychain_args)
        ent="$(entitlements_for "$rel")"
        if [ -n "$ent" ]; then
            args+=(--entitlements "$ent")
            log "  entitlements $(basename "$ent") -> $rel"
        fi
    fi
    chmod u+w "$path" 2>/dev/null || true
    codesign "${args[@]}" "$path"
}

cmd_sign_tree() {
    local root="${1:?sign-tree <dir> [--entitlements glob=plist]...}"; shift
    [ -d "$root" ] || die "not a directory: $root"
    root="$(cd "$root" && pwd)"
    while [ $# -gt 0 ]; do
        case "$1" in
            --entitlements) add_entitlement "${2:?}"; shift 2 ;;
            --entitlements-map)
                local map="${2:?}" mapdir glob plist
                mapdir="$(cd "$(dirname "$map")" && pwd)"
                while read -r glob plist _; do
                    case "$glob" in ''|\#*) continue ;; esac
                    add_entitlement "$glob=$plist" "$mapdir"
                done < "$map"
                shift 2 ;;
            *) die "unknown sign-tree option: $1" ;;
        esac
    done

    local libs=() exes=() apps=() f
    while IFS= read -r -d '' f; do
        is_macho "$f" || continue
        if is_macho_exec "$f"; then exes+=("$f"); else libs+=("$f"); fi
    done < <(find "$root" -type f -print0)
    # .app bundles, deepest first, so an outer bundle seals already-signed inner ones.
    while IFS= read -r f; do
        [ -n "$f" ] && apps+=("$f")
    done < <(find "$root" -type d -name '*.app' | awk '{print gsub("/","/") "\t" $0}' | sort -rn | cut -f2-)

    log "sign-tree $root: ${#libs[@]} libraries, ${#exes[@]} executables, ${#apps[@]} app bundles"
    log "  identity: ${APP_ID:-ad-hoc (-)}"
    for f in ${libs[@]+"${libs[@]}"} ${exes[@]+"${exes[@]}"}; do sign_one "$f" "${f#"$root"/}"; done
    for f in ${apps[@]+"${apps[@]}"}; do sign_one "$f" "${f#"$root"/}"; done

    for f in ${libs[@]+"${libs[@]}"} ${exes[@]+"${exes[@]}"}; do
        codesign --verify --strict "$f" || die "signature does not verify: $f"
    done
    for f in ${apps[@]+"${apps[@]}"}; do
        codesign --verify --deep --strict "$f" || die "bundle signature does not verify: $f"
    done
    if [ -n "$APP_ID" ]; then
        # Notarization needs the hardened runtime + a secure timestamp on every
        # Mach-O; catch a miss here rather than in an Apple log 5 minutes later.
        for f in ${libs[@]+"${libs[@]}"} ${exes[@]+"${exes[@]}"} ${apps[@]+"${apps[@]}"}; do
            local info
            info="$(codesign -dvv "$f" 2>&1)"
            grep -q 'flags=.*runtime' <<<"$info" || die "no hardened runtime: $f"
            grep -q '^Timestamp=' <<<"$info" || die "no secure timestamp: $f"
        done
    fi
    log "sign-tree OK"
}

# ----------------------------------------------------------------- sign-pkg --
cmd_sign_pkg() {
    local in="${1:?sign-pkg <in.pkg> [out.pkg]}" out="${2:-}"
    [ -f "$in" ] || die "no such package: $in"
    if [ -z "$INST_ID" ]; then
        log "sign-pkg: DXR_MACOS_INSTALLER_IDENTITY unset — $(basename "$in") stays unsigned"
        [ -n "$out" ] && [ "$out" != "$in" ] && cp "$in" "$out"
        return 0
    fi
    local tmp="${out:-$in}.signing.$$"
    local args=(--sign "$INST_ID" --timestamp)
    while IFS= read -r a; do args+=("$a"); done < <(keychain_args)
    productsign "${args[@]}" "$in" "$tmp"
    mv -f "$tmp" "${out:-$in}"
    pkgutil --check-signature "${out:-$in}"
}

# ----------------------------------------------------------------- notarize --
cmd_notarize() {
    local file="${1:?notarize <file>}"
    [ -f "$file" ] || die "no such file: $file"
    local creds=() a
    while IFS= read -r a; do creds+=("$a"); done < <(notary_creds) \
        || die "no notarization credentials (DXR_NOTARY_PROFILE or DXR_NOTARY_KEY_PATH/_KEY_ID/_ISSUER)"
    [ ${#creds[@]} -gt 0 ] || die "no notarization credentials (DXR_NOTARY_PROFILE or DXR_NOTARY_KEY_PATH/_KEY_ID/_ISSUER)"
    log "notarize $(basename "$file") via $(notary_mode) — waiting (timeout 30m)"
    local out id status errf
    errf="$(mktemp)"
    out="$(xcrun notarytool submit "$file" --wait --timeout 30m --output-format json "${creds[@]}" 2>"$errf")" || true
    id="$(/usr/bin/plutil -extract id raw -o - - <<<"$out" 2>/dev/null || true)"
    status="$(/usr/bin/plutil -extract status raw -o - - <<<"$out" 2>/dev/null || true)"
    log "notarization id=${id:-?} status=${status:-?}"
    [ "$status" = "Accepted" ] && rm -f "$errf"
    if [ "$status" != "Accepted" ]; then
        echo "$out" >&2; cat "$errf" >&2
        if [ -n "$id" ]; then
            log "notarytool log $id:"
            xcrun notarytool log "$id" "${creds[@]}" >&2 || true
        fi
        die "notarization not accepted"
    fi
}

# ------------------------------------------------------------------- staple --
gatekeeper_check() {
    local file="$1" type=install assess
    case "$file" in *.app) type=execute ;; *.dmg) type=open ;; esac
    if [ "$type" = open ]; then
        assess="$(spctl --assess --type open --context context:primary-signature -vv "$file" 2>&1)" || { echo "$assess" >&2; die "Gatekeeper rejected $file"; }
    else
        assess="$(spctl --assess --type "$type" -vv "$file" 2>&1)" || { echo "$assess" >&2; die "Gatekeeper rejected $file"; }
    fi
    echo "$assess"
    grep -q 'source=Notarized Developer ID' <<<"$assess" || die "Gatekeeper accepted $file but not as 'Notarized Developer ID'"
}

cmd_staple() {
    local file="${1:?staple <file>}"
    xcrun stapler staple "$file"
    xcrun stapler validate "$file"
    gatekeeper_check "$file"
    case "$file" in *.pkg) pkgutil --check-signature "$file" ;; esac
    log "staple OK: $(basename "$file") is notarized, stapled and Gatekeeper-accepted"
}

# --------------------------------------------------------------- verify-pkg --
cmd_verify_pkg() {
    local pkg="${1:?verify-pkg <file.pkg>}" sig
    [ -f "$pkg" ] || die "no such package: $pkg"
    sig="$(pkgutil --check-signature "$pkg" 2>&1)" || { echo "$sig" >&2; die "pkgutil: $pkg is not signed"; }
    echo "$sig"
    grep -q 'Developer ID Installer' <<<"$sig" || die "$pkg is not signed with a Developer ID Installer identity"
    xcrun stapler validate "$pkg" || die "no valid notarization ticket stapled to $pkg"
    gatekeeper_check "$pkg"
    log "verify-pkg OK: $(basename "$pkg") — Developer ID signed, notarized, stapled"
}

# ------------------------------------------------------------- finalize-pkg --
cmd_finalize_pkg() {
    local pkg="${1:?finalize-pkg <file.pkg>}"
    cmd_sign_pkg "$pkg"
    if [ -z "$INST_ID" ]; then
        log "finalize-pkg: unsigned package (no Installer identity) — not notarized"
        return 0
    fi
    if [ "${DXR_MACOS_NOTARIZE:-1}" = "0" ] || ! notary_creds >/dev/null; then
        log "finalize-pkg: signed, NOT notarized ($(notary_mode))"
        return 0
    fi
    cmd_notarize "$pkg"
    cmd_staple "$pkg"
}

# ----------------------------------------------------------------- CI modes --
CI_SECRETS=(MACOS_APP_CERT_P12 MACOS_APP_CERT_PASSWORD
            MACOS_INSTALLER_CERT_P12 MACOS_INSTALLER_CERT_PASSWORD
            APPLE_TEAM_ID APPLE_NOTARY_KEY_P8 APPLE_NOTARY_KEY_ID APPLE_NOTARY_ISSUER_ID)
CI_DIR="${DXR_CI_SIGN_DIR:-${RUNNER_TEMP:-${TMPDIR:-/tmp}}/dxr-macos-sign}"
CI_KC="$CI_DIR/dxr-signing.keychain-db"

cmd_ci_secrets_check() {
    local n missing=()
    for n in "${CI_SECRETS[@]}"; do
        [ -n "${!n:-}" ] || missing+=("$n")
    done
    if [ ${#missing[@]} -gt 0 ]; then
        echo "${missing[*]}"
        return 1
    fi
    return 0
}

# find_identity <policy-args> <prefix> → the quoted identity name containing
# "(<TEAM>)", from the CI keychain only.
find_identity() {
    local prefix="$1"; shift
    security find-identity "$@" "$CI_KC" 2>/dev/null \
        | sed -n 's/^ *[0-9]*) [0-9A-F]\{40\} "\(.*\)".*$/\1/p' \
        | grep -F "$prefix" | grep -F "($APPLE_TEAM_ID)" | head -1 || true
}

cmd_ci_keychain_setup() {
    local env_out=""
    while [ $# -gt 0 ]; do
        case "$1" in
            --env-out) env_out="${2:?}"; shift 2 ;;
            *) die "unknown ci-keychain-setup option: $1" ;;
        esac
    done
    local missing
    missing="$(cmd_ci_secrets_check)" || die "missing CI signing secrets: $missing"
    [ ! -e "$CI_KC" ] || die "keychain already exists: $CI_KC (run ci-keychain-teardown first)"

    ( umask 077; mkdir -p "$CI_DIR" )
    chmod 700 "$CI_DIR"
    local kc_pw
    kc_pw="$(openssl rand -hex 24)"

    # Secrets -> files. tr strips the line wrapping a pasted base64 may carry.
    ( umask 077
      printf '%s' "$MACOS_APP_CERT_P12"       | tr -d ' \r\n' | base64 --decode > "$CI_DIR/app.p12"
      printf '%s' "$MACOS_INSTALLER_CERT_P12" | tr -d ' \r\n' | base64 --decode > "$CI_DIR/installer.p12"
      printf '%s\n' "$APPLE_NOTARY_KEY_P8" > "$CI_DIR/AuthKey_${APPLE_NOTARY_KEY_ID}.p8" )
    [ -s "$CI_DIR/app.p12" ]       || die "MACOS_APP_CERT_P12 did not base64-decode to a file"
    [ -s "$CI_DIR/installer.p12" ] || die "MACOS_INSTALLER_CERT_P12 did not base64-decode to a file"
    grep -q 'BEGIN PRIVATE KEY' "$CI_DIR/AuthKey_${APPLE_NOTARY_KEY_ID}.p8" \
        || die "APPLE_NOTARY_KEY_P8 is not a PEM .p8 (expected the raw AuthKey_*.p8 text)"

    # Remember the search list so teardown can put it back exactly.
    security list-keychains -d user | sed -e 's/^ *"//' -e 's/"$//' > "$CI_DIR/searchlist"

    security create-keychain -p "$kc_pw" "$CI_KC"
    security set-keychain-settings -lut 21600 "$CI_KC"   # no auto-lock mid-notarization
    security unlock-keychain -p "$kc_pw" "$CI_KC"
    security import "$CI_DIR/app.p12" -k "$CI_KC" -f pkcs12 -P "$MACOS_APP_CERT_PASSWORD" \
        -T /usr/bin/codesign -T /usr/bin/productsign -T /usr/bin/security >/dev/null \
        || die "importing MACOS_APP_CERT_P12 failed (wrong MACOS_APP_CERT_PASSWORD?)"
    security import "$CI_DIR/installer.p12" -k "$CI_KC" -f pkcs12 -P "$MACOS_INSTALLER_CERT_PASSWORD" \
        -T /usr/bin/codesign -T /usr/bin/productsign -T /usr/bin/security >/dev/null \
        || die "importing MACOS_INSTALLER_CERT_P12 failed (wrong MACOS_INSTALLER_CERT_PASSWORD?)"
    rm -f "$CI_DIR/app.p12" "$CI_DIR/installer.p12"
    # Let Apple tools use the private keys without a UI prompt (there is no UI).
    security set-key-partition-list -S apple-tool:,apple:,codesign: -s -k "$kc_pw" "$CI_KC" >/dev/null
    kc_pw=""

    # Apple's Developer ID intermediates: a .p12 usually carries only the leaf,
    # and without the chain the identity is not "valid" and codesign cannot
    # embed it. Best effort — runner images often have them already.
    local ca
    for ca in DeveloperIDG2CA DeveloperIDCA; do
        if curl -fsSL --retry 3 -o "$CI_DIR/$ca.cer" "https://www.apple.com/certificateauthority/$ca.cer"; then
            security import "$CI_DIR/$ca.cer" -k "$CI_KC" >/dev/null 2>&1 || true
        else
            log "note: could not download $ca.cer; relying on the runner's System keychain"
        fi
    done

    # Prepend ours to the user search list (codesign/productsign resolve the
    # chain through it), keeping everything that was there.
    local list=()
    while IFS= read -r k; do [ -n "$k" ] && list+=("$k"); done < "$CI_DIR/searchlist"
    security list-keychains -d user -s "$CI_KC" ${list[@]+"${list[@]}"}

    local app inst
    app="$(find_identity "Developer ID Application:" -v -p codesigning)"
    inst="$(find_identity "Developer ID Installer:" -v)"
    [ -n "$inst" ] || inst="$(find_identity "Developer ID Installer:")"
    [ -n "$app" ]  || die "no valid 'Developer ID Application: … ($APPLE_TEAM_ID)' identity after import"
    [ -n "$inst" ] || die "no 'Developer ID Installer: … ($APPLE_TEAM_ID)' identity after import"
    log "CI keychain ready: $CI_KC"
    log "  app identity:       $app"
    log "  installer identity: $inst"

    local out="${env_out:-/dev/stdout}"
    {
        echo "DXR_MACOS_KEYCHAIN=$CI_KC"
        echo "DXR_MACOS_APP_IDENTITY=$app"
        echo "DXR_MACOS_INSTALLER_IDENTITY=$inst"
        echo "DXR_NOTARY_KEY_PATH=$CI_DIR/AuthKey_${APPLE_NOTARY_KEY_ID}.p8"
        echo "DXR_NOTARY_KEY_ID=$APPLE_NOTARY_KEY_ID"
        echo "DXR_NOTARY_ISSUER=$APPLE_NOTARY_ISSUER_ID"
    } >> "$out"
}

cmd_ci_keychain_teardown() {
    if [ -f "$CI_DIR/searchlist" ]; then
        local list=()
        while IFS= read -r k; do [ -n "$k" ] && [ "$k" != "$CI_KC" ] && list+=("$k"); done < "$CI_DIR/searchlist"
        security list-keychains -d user -s ${list[@]+"${list[@]}"} || true
    fi
    if [ -e "$CI_KC" ]; then
        security delete-keychain "$CI_KC" || rm -f "$CI_KC"
        log "deleted CI keychain $CI_KC"
    fi
    rm -rf "$CI_DIR"
}

cmd_status() {
    echo "app identity:       ${APP_ID:-ad-hoc (-)}"
    echo "installer identity: ${INST_ID:-none (pkg unsigned)}"
    echo "keychain:           ${KEYCHAIN:-default search list}"
    echo "notarization:       $(notary_mode)"
}

mode="${1:-}"; [ $# -gt 0 ] && shift
case "$mode" in
    sign-tree)    cmd_sign_tree "$@" ;;
    sign-pkg)     cmd_sign_pkg "$@" ;;
    notarize)     cmd_notarize "$@" ;;
    staple)       cmd_staple "$@" ;;
    finalize-pkg) cmd_finalize_pkg "$@" ;;
    verify-pkg)   cmd_verify_pkg "$@" ;;
    status)       cmd_status ;;
    ci-secrets-check)     cmd_ci_secrets_check ;;
    ci-keychain-setup)    cmd_ci_keychain_setup "$@" ;;
    ci-keychain-teardown) cmd_ci_keychain_teardown ;;
    *) awk 'NR>1 && !/^#/{exit} NR>1' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
