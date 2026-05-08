# ssh2ng

Drop-in replacement for [`ssh2`](https://www.npmjs.com/package/ssh2) on top
of [libssh](https://www.libssh.org), so SSH client behavior in Node.js
matches what you get from the system `ssh` binary — including the parts of
`~/.ssh/config` that the JavaScript-only `ssh2` library never implemented.

## Why

The `ssh2` npm package implements only the SSH protocol. It does not parse
`~/.ssh/config`, doesn't honor `ProxyJump`, `ProxyCommand`, certificates,
`Match` blocks, or hardware tokens. For real-world setups (corporate jump
hosts, multi-hop bastions, pinned identities per host) this means every
non-trivial connection silently fails.

`ssh2ng` is the same `Client` / `SFTPWrapper` / `exec` API surface, but the
underlying transport is libssh — the same library used by Cyberduck,
Remmina, and Proxmox. libssh natively reads `~/.ssh/config` via
`ssh_options_parse_config()`, so every host alias, ProxyJump, IdentityFile,
and Match stanza in your config "just works".

## Status

Early. Connect, exec, and a useful subset of SFTP (read/write/stat/rename/
unlink/mkdir) are implemented and ssh2-API-compatible. Streaming exec,
`AbortSignal`, range SFTP reads, and prebuilt binaries for non-Linux
platforms are still pending.

## Install (current state)

```sh
# Build dependency: libssh ≥ 0.10 with headers (libssh-dev / libssh-devel /
# brew install libssh / pacman -S libssh).
bun add file:/path/to/ssh2ng
```

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
client.connect({ host: 'pi' })   // resolves through ~/.ssh/config
```

`client.connect()` reads:

- `Host`, `HostName`, `User`, `Port` — for the alias
- `IdentityFile` chain (or the agent if `SSH_AUTH_SOCK` is set)
- `ProxyJump`, `ProxyCommand`
- `CertificateFile`
- `Match` blocks

…by delegating to libssh, which is the canonical implementation.

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
