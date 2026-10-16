# DisplayXR-RemoteConversion

The generic **remote conversion provider** (ADR-057 S2;
`docs/specs/runtime/conversion-provider-discovery.md` §6.3). Id `remote-conversion`. It serves
`SPLATS_MONO` (cap 1) and `SPLATS_STEREO` (cap 2) on the job lane by forwarding each job to an
HTTPS endpoint that implements **endpoint contract v1** (`GET /v1/describe`, `POST /v1/jobs`). One
provider for every remote splat endpoint: an endpoint is configuration, not code.

It is off by default (`ENABLE`), consent-gated (the user's per-provider off-box consent), and no
installer registers it: add a row yourself.

## Register it

Windows (elevated), next to the other providers:

```bat
reg add "HKLM\Software\DisplayXR\ConversionProviders\remote-conversion" /v Binary /t REG_SZ /d "C:\Program Files\DisplayXR\Runtime\plugins\DisplayXR-RemoteConversion.dll" /f /reg:64
reg add "HKLM\Software\DisplayXR\ConversionProviders\remote-conversion" /v DisplayName /t REG_SZ /d "DisplayXR Remote Conversion" /f /reg:64
reg add "HKLM\Software\DisplayXR\ConversionProviders\remote-conversion" /v Version /t REG_SZ /d "1" /f /reg:64
```

POSIX: a manifest `100-remote-conversion-conversion-provider.json` in a plug-in root
(`XRT_PLUGIN_SEARCH_PATH` for a dev tree):

```json
{
    "file_format_version": "1.0",
    "plugin": {
        "id": "remote-conversion", "display_name": "DisplayXR Remote Conversion",
        "binary_path": "/path/to/DisplayXR-RemoteConversion.so", "probe_order": 100,
        "settings": { "ENABLE": "1", "ENDPOINTS": "https://splats.example/api", "CREDENTIAL": "my-endpoint-token" }
    }
}
```

## Configure it

```sh
displayxr-cli conv set remote-conversion ENABLE 1
displayxr-cli conv set remote-conversion ENDPOINTS "https://a.example/api,https://b.example/api"
displayxr-cli conv set remote-conversion CREDENTIAL my-endpoint-token
printf '%s\n' "$TOKEN" | displayxr-cli conv credential set my-endpoint-token   # the OS credential store
displayxr-cli conv consent remote-conversion grant                           # photos may leave this machine
displayxr-cli conv run splats-mono photo.jpg photo.sog --camera-block auto
```

| Key | Default | Meaning |
|---|---|---|
| `ENABLE` | 0 | `probe` declines unless 1 |
| `ENDPOINTS` | — | base URLs in fallback order; `https://`, or `http://` on loopback only |
| `CREDENTIAL` | — | the **name** of the bearer token in the OS credential store; never the token |
| `TIMEOUT_S` | 120 | per request; a timeout falls through to the next host |
| `FORMAT` | `sog` | `sog` \| `ply` when the job names none |
| `OPTIONS` | — | a JSON object passed through as `params.options` |

Settings are read only through `host->get_setting` (never `getenv`); `DXR_CONV_REMOTE_CONVERSION_<KEY>`
in the hosting process's environment overrides them, like every provider.

## Behaviour

- **Consent**: without `conv consent remote-conversion grant` both capabilities are `DISABLED`
  ("consent required"); re-read for every job, so a revoke stops the next upload.
- **Licence**: the class `/v1/describe` states, as-is. `research_only` stays research only and is
  selectable only with `AllowResearchOnly`.
- **Fallback**: 5xx, 429, 408, 401, 403, 404, timeouts and connect errors try the next host; a 422 (bad
  input) is never retried. Every host down: `REMOTE_ERROR` and `FAILED(NETWORK, retryable)`.
- **Facts**: the blob is written untouched; `X-DXR-Result` fills `xrt_conv_splat_info` and
  `xrt_conv_splat_result` (build stamp of the host that answered, refusal, metric fit, layers, focus
  suggestion). The runtime never writes a camera block; `conv run --camera-block auto` is the
  separate convenience writer.
- **HTTP**: WinHTTP on Windows; libcurl loaded at run time on macOS and Linux (no build or package
  dependency). Linux without `libcurl.so.4`: `ABSENT(PLATFORM)` with that reason.

Tests: `tests/tests_remote_conversion.cpp` against `tests/conv_remote_test_server.py` (a stdlib-only
contract-v1 endpoint on 127.0.0.1).
