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
#   status
#       Print which mode each step will run in. Prints no secret.
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
#
# The hardened runtime (--options runtime) and a secure timestamp are applied
# whenever a real identity is used; notarization rejects anything without them.
set -euo pipefail

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
cmd_staple() {
    local file="${1:?staple <file>}"
    xcrun stapler staple "$file"
    xcrun stapler validate "$file"
    local type=install
    case "$file" in *.app) type=execute ;; *.dmg) type=open ;; esac
    local assess
    if [ "$type" = open ]; then
        assess="$(spctl --assess --type open --context context:primary-signature -vv "$file" 2>&1)" || { echo "$assess" >&2; die "Gatekeeper rejected $file"; }
    else
        assess="$(spctl --assess --type "$type" -vv "$file" 2>&1)" || { echo "$assess" >&2; die "Gatekeeper rejected $file"; }
    fi
    echo "$assess"
    grep -q 'source=Notarized Developer ID' <<<"$assess" || die "Gatekeeper accepted $file but not as 'Notarized Developer ID'"
    case "$file" in *.pkg) pkgutil --check-signature "$file" ;; esac
    log "staple OK: $(basename "$file") is notarized, stapled and Gatekeeper-accepted"
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
    status)       cmd_status ;;
    *) sed -n '2,50p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
