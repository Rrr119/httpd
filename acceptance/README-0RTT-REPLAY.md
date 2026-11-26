# HTTP/3 0-RTT replay acceptance

Run:

```sh
./acceptance/h3_0rtt_replay.sh
```

The suite requires the normal mod_http3 test dependencies, including aioquic.
It configures `H3EarlyData On`, starts the pyhttpd server, and verifies:

- Three independent resumed aioquic clients each complete a normal first 0-RTT
  request with `200`, then have the same session token replayed and rejected
  with `425 Too Early` and `Retry-After: 0`.
- Fresh tokens and ordinary first packets remain unaffected.
- A duplicate inside the configured time window is rejected; the same token is
  accepted again after the window expires.
- With capacity one and the default `evict-oldest` policy, the bounded table
  never grows and the oldest token becomes a cache miss after a new token is
  inserted.
- With `H3EarlyDataReplayCacheFull reject-early`, a new token beyond capacity
  receives `503`; unexpired tokens are not evicted.

A curl adapter is also present in
`test/modules/http3/test_210_early_replay.py`. It runs only when curl provides
both `HTTP3` and `SSLS-EXPORT` and can save an HTTP/3 TLS ticket; otherwise it
skips rather than treating a client limitation as a server failure.

## Capacity semantics

`H3EarlyDataReplayCacheSize N` allocates a fixed `N`-entry table at child
startup. There are no per-request allocations after creation. Expired entries
are reclaimed first.

The default `evict-oldest` policy removes the oldest entry when the table is
full. This preserves service availability but means a token evicted before its
time window ends is no longer recognized as a replay. The `evicted` counter in
the `http3-status` replay cache object exposes this case.

The `reject-early` policy is fail-closed: it does not forget unexpired tokens
and returns `503` for additional early requests at capacity. Clients can retry
those requests after the handshake.

The cache is per child process. In the supported `ListenCoresBucketsRatio`
deployment, connection-ID routing keeps a replayed QUIC connection on the same
bucket/child.
