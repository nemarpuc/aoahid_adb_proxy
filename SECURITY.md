# Security Policy

## Supported Versions

Only the [latest release](https://github.com/nemarpuc/aoahid_adb_proxy/releases/latest)
receives security fixes. Before 1.0.0 a fix may come with a change of the minor
number.

## Scope and Threat Model

- **A local TCP port gives access to the phone's ADB.** The listener binds
  `127.0.0.1` only, so nothing outside the machine can reach it, but any process
  on the machine can connect, and ADB can run a shell on the phone. Run the proxy
  only on a machine whose local users you trust, and stop it when you do not need it.
- **No authentication.** The proxy forwards bytes between the TCP client and the
  phone; it does not check who the client is. The phone's own "Allow USB debugging"
  prompt is the only gate.
- **Ports.** Choose a port outside 5555-5585 (adb treats that range as emulators).

## Reporting a Vulnerability

Report vulnerabilities privately through the repository's
[GitHub Security Advisories form](https://github.com/nemarpuc/aoahid_adb_proxy/security/advisories/new).
Do not put device serial numbers, ADB keys, or captured ADB traffic in a public issue.
