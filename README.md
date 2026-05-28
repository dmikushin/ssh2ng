# ssh2ng

Drop-in replacement for [`ssh2`](https://www.npmjs.com/package/ssh2) using
`libautossh` as the supervised transport and [libssh](https://www.libssh.org)
for exec/SFTP on top of that transport.

## Why

The `ssh2` npm package implements only the SSH protocol. It does not behave
like the system OpenSSH client for real-world configs, and it cannot supervise
the underlying TCP session the way autossh can.

`ssh2ng` keeps the same `Client` / `SFTPWrapper` / `exec` API surface, but the
default connection path is now:

1. `libautossh` starts and supervises an OpenSSH local forward to the target.
2. `libssh` connects to the returned local endpoint.
3. exec and SFTP run through libssh over that supervised transport.

This keeps OpenSSH responsible for `ProxyJump`, `ProxyCommand`, `Match`,
hardware tokens, certificates, and ssh-config behavior, while autossh owns
monitoring and restart of the fragile transport layer.

## Status

Early. Connect, exec, and a useful subset of SFTP (read/write/stat/rename/
unlink/mkdir) are implemented and ssh2-API-compatible. Streaming exec,
`AbortSignal`, range SFTP reads, and prebuilt binaries for non-Linux
platforms are still pending.

## Install (current state)

`ssh2ng` ships libautossh and libssh statically inside the native addon. The
resulting `ssh2ng.node` only depends on libraries every distro has
(libcrypto, libz, libc, libstdc++). No `libssh-dev` package is needed at
install or runtime.

```sh
# Build prerequisites: cmake, cargo/rustc, a C/C++ toolchain, OpenSSL dev
# headers. On Debian/Ubuntu: build-essential cmake cargo libssl-dev.
# On macOS: brew install cmake rust openssl.
git clone --recurse-submodules <ssh2ng-repo>
bun install        # builds libautossh.a, libssh.a, then the native addon
```

Once the package is published to npm with prebuilds, consumers will just do:

```sh
bun add ssh2ng
```

with no compilation step.

## Usage

```ts
import { Client } from 'ssh2ng'

const client = new Client()
client.once('ready', () => {
  client.exec('ls -la /etc', (err, stream) => {
    if (err) throw err
    let out = ''
    stream.on('data', (c: Buffer) => { out += c.toString('utf-8') })
    stream.on('close', (code: number) => {
      console.log('exit', code, '\n', out)
      client.end()
    })
  })
})
client.once('error', err => console.error(err))
client.connect({ host: 'pi' })   // OpenSSH/autossh transport by default
```

By default, `client.connect()` starts a supervised autossh transport. Useful
transport options:

- `useAutossh: false` disables the supervised transport and uses direct libssh.
- `autosshDestination` overrides the OpenSSH destination. Defaults to `host`.
- `autosshTargetHost` / `autosshTargetPort` set the remote endpoint forwarded
  back to libssh. Defaults to `127.0.0.1:<port || 22>`.
- `autosshBindHost` controls the local bind address consumed by libssh.
- `autosshMonitorHost` / `autosshMonitorPort` control autossh monitoring.
- `autosshDisableMonitor: true` is equivalent to `autossh -M 0`.
- `autosshExtraArgs` passes extra arguments to the OpenSSH process.

## What's *not* drop-in vs `ssh2`

- `Client.connect({ privateKey, agent, passphrase })` — accepted for API
  parity but ignored. libssh handles auth (agent + IdentityFile chain).
  Set `SSH_AUTH_SOCK` to use the agent; otherwise libssh will use the
  IdentityFile entries from your config / known defaults.
- Streaming exec is currently one-shot (data arrives in a single batch
  on `'data'`, then `'close'` fires). Consumers using the streaming
  shape work; consumers depending on chunked delivery for memory don't,
  yet.
- `sftp.createReadStream` reads the whole file then slices. Real
  streaming/range reads are coming.

## License

MIT for the binding code. libssh itself is LGPL-2.1+; `ssh2ng` links
against it, see `LICENSE` for details.
