# Security Policy

## Supported versions

Claudeputer is a hobby project; only the latest commit on `main` (and the build
published to the [web flasher](https://poppnn.github.io/claudeputer/)) is
supported. Please update before reporting an issue.

## Reporting a vulnerability

Please **do not** open a public issue for security problems. Instead:

- Open a private [security advisory](https://github.com/poppnn/claudeputer/security/advisories/new), or
- Contact the maintainer [@poppnn](https://github.com/poppnn) directly.

I'll do my best to respond promptly. As a personal project there's no formal SLA,
but security reports are taken seriously.

## Good to know (by design)

These are intentional trade-offs, not vulnerabilities — but worth understanding:

- **Secrets stay on the device.** Your WiFi password and Anthropic API key are
  stored in the ESP32's NVS (or compile-time `src/config.h`, which is
  git-ignored). They are **not** stored encrypted at rest. The API key is sent
  only to `api.anthropic.com` over validated TLS.
- **TLS is verified.** Requests to `api.anthropic.com` validate the server
  certificate against embedded root CAs (`src/anthropic_ca.h`); the clock is
  synced over NTP at boot. `TLS_INSECURE` (off by default) disables this for
  debugging only.
- **The API-key import form runs over plain HTTP on your LAN**, and only during
  setup. It's meant for a trusted home network. Anyone on the same network
  during that short window could reach the form. Prefer typing the key or the
  SD-card import on untrusted networks.
- **No authentication** on the setup web server (it stops once setup completes).

If you find a way these assumptions can be abused beyond the documented scope,
please report it.
