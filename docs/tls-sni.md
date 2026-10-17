# TLS Server Name Indication (SNI) on the CC3200

Status: investigation only (2026-10-13). Not planned, not decided; kept so
the facts are at hand if the owner ever takes it up.

## Observation

Neither the original firmware nor this firmware sends an SNI extension in
the TLS client hello to teddyCloud. For the original firmware this is an
observed behaviour only (no knowledge of its code).

## Why: a socket option, not a hardware limit

On the CC3200 the TLS handshake runs entirely in the network processor
(NWP). The host firmware never builds the client hello itself; it can only
configure the secure socket through `sl_SetSockOpt`. `connect_once` in
`src/cloud.c` sets the three file options (private key, client certificate,
CA file) and connects to the resolved IP. Without a domain-name option the
NWP sends no server name.

The SimpleLink host driver has the option for it:
`SL_SO_SECURE_DOMAIN_NAME_VERIFICATION` (value 35,
`third_party/simplelink/include/socket.h`). It takes the host name as a
string. The SSL example of the TI SDK 1.5.0
(`cc3200-sdk/example/ssl/main.c`) sets it right after the CA file:

```c
sl_SetSockOpt(sock, SL_SOL_SOCKET, SL_SO_SECURE_DOMAIN_NAME_VERIFICATION,
              host, strlen(host));
```

With the option the NWP verifies the server certificate against the name
(CN or SAN) and, per TI documentation, sends the name as SNI. In this
firmware it would be one more entry in the option loop of `connect_once`,
using `cfg.host`.

Requirement: an NWP service pack that knows the option. On an older
service pack `sl_SetSockOpt` returns an error, visible in the UART log. The
NWP version is printed at start in the line `net: up, NWP ...`.

## Side effects to settle first

1. **Name verification becomes strict.** The teddyCloud server certificate
   must carry the configured host (`host` in `/cfw/config.txt`) in CN or
   SAN. A self-generated teddyCloud certificate does not guarantee this;
   `sl_Connect` would then fail even though the CA matches.
2. **teddyCloud selects the certificate by SNI presence.** In the fork
   `teddycloud_cfw`, `httpServerSelectBoxCertificate` in `src/server.c`
   serves the TB1 certificate when no SNI is present and the TB2
   certificate (ESP32 box) when one is. A CC3200 box sending SNI would get
   the TB2 chain, which the NWP cannot verify with its CA. The server would
   have to select by the name or by the client certificate instead.
3. **Decision.** Sending SNI is new behaviour of the custom firmware and
   needs a decision of its own; the server verification by CA stays
   untouched.

## If it is ever tried

1. Check the server certificate of the test server for the host name.
2. Change the certificate selection in the teddyCloud fork.
3. Set the option in `connect_once`, build, run on the box.
4. Verify with teddyCloud's trace `Box TLS SNI '...'` and the box's
   `cloud: connect ...` line.
