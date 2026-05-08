/**
 * ssh2-compatible JS layer over the libssh native binding.
 *
 * The native module exposes `SshSession` with promise-returning methods.
 * This file wraps that into the EventEmitter / callback-based API surface
 * that ssh2 consumers expect, so the package can be a drop-in replacement
 * for ssh2 in the subset of features Dario actually uses.
 */
import { EventEmitter } from 'node:events'
import { Readable } from 'node:stream'
import { createRequire } from 'node:module'
import { existsSync } from 'node:fs'

const require = createRequire(import.meta.url)
import { fileURLToPath } from 'node:url'
import { dirname, resolve } from 'node:path'
const __dirname = dirname(fileURLToPath(import.meta.url))

/**
 * Locate `ssh2ng.node`. Search order:
 *   1) Next to this JS file (normal `node_modules/ssh2ng/build/Release/ssh2ng.node`)
 *   2) Next to the running executable (Bun --compile output ships the
 *      .node file alongside the binary).
 *   3) `SSH2NG_NATIVE_ADDON` env var override (for tests / packagers).
 *
 * Bun's `--compile` does not bundle `.node` files — they have to travel
 * with the binary. Path #2 covers that case so consumers who ship a
 * compiled CLI can drop the .node next to it.
 */
function findNativeAddon(): string {
  const override = process.env.SSH2NG_NATIVE_ADDON
  if (override && existsSync(override)) return override

  const candidates = [
    resolve(__dirname, '..', 'build', 'Release', 'ssh2ng.node'),
    resolve(dirname(process.execPath), 'ssh2ng.node'),
    resolve(dirname(process.execPath), 'lib', 'ssh2ng.node'),
  ]
  for (const c of candidates) if (existsSync(c)) return c
  throw new Error(
    `ssh2ng: could not find native addon. Looked in:\n` +
    candidates.map(c => `  - ${c}`).join('\n') +
    `\nSet SSH2NG_NATIVE_ADDON to override.`,
  )
}

const native = require(findNativeAddon()) as {
  SshSession: new () => NativeSession
}

interface NativeSession {
  connect(opts: { host: string; username?: string; port?: number; timeoutSec?: number; acceptAnyHostKey?: boolean }): Promise<void>
  exec(cmd: string, stdin?: Buffer): Promise<{ stdout: Buffer; stderr: Buffer; code: number }>
  sftpInit(): Promise<void>
  sftpReadFile(path: string): Promise<Buffer>
  sftpWriteFile(path: string, data: Buffer, mode?: number): Promise<void>
  sftpStat(path: string): Promise<{ size: number; mtime: number; isDirectory: boolean; isFile: boolean }>
  sftpRename(oldPath: string, newPath: string): Promise<void>
  sftpUnlink(path: string): Promise<void>
  sftpMkdir(path: string, mode?: number): Promise<void>
  end(): void
}

export interface ConnectConfig {
  host: string
  port?: number
  username?: string
  /** Ignored — auth is delegated to libssh (agent + ssh-config). Kept for
   *  ssh2 API compatibility. */
  agent?: string
  /** Ignored — see `agent`. */
  privateKey?: Buffer | string
  /** Ignored — see `agent`. */
  passphrase?: string
  readyTimeout?: number
  keepaliveInterval?: number
  /** ssh2-incompatible extension: when false, refuse hosts not in
   *  known_hosts. Default true (accept any) for parity with our previous
   *  ssh2 wiring which also did not verify. */
  acceptAnyHostKey?: boolean
}

/**
 * ssh2-compatible Stats object returned by sftp.stat().
 */
export interface SftpStats {
  size: number
  mtime: number
  isDirectory(): boolean
  isFile(): boolean
}

/**
 * ssh2-compatible Client. Same event names, same method signatures (within
 * the subset implemented here).
 */
export class Client extends EventEmitter {
  private session?: NativeSession
  private connected = false
  private ended = false

  connect(config: ConnectConfig): this {
    if (this.session) throw new Error('Client already has a session')
    this.session = new native.SshSession()
    const timeoutSec = config.readyTimeout ? Math.ceil(config.readyTimeout / 1000) : 15

    // Build the native opts object with only defined fields — passing
    // `username: undefined` trips the C++ binding's String coercion.
    const nativeOpts: Parameters<NativeSession['connect']>[0] = {
      host: config.host,
      timeoutSec,
      acceptAnyHostKey: config.acceptAnyHostKey ?? true,
    }
    if (config.username !== undefined) nativeOpts.username = config.username
    if (config.port !== undefined) nativeOpts.port = config.port

    this.session.connect(nativeOpts).then(() => {
      this.connected = true
      this.emit('ready')
    }).catch(err => {
      // Drop the native session — it's already torn down on connect failure.
      this.session = undefined
      this.emit('error', err instanceof Error ? err : new Error(String(err)))
    })

    return this
  }

  exec(command: string, callback: (err: Error | null, stream?: ExecStream) => void): boolean {
    if (!this.connected || !this.session) {
      queueMicrotask(() => callback(new Error('Not connected')))
      return false
    }
    const sess = this.session
    const stream = new ExecStream()
    sess.exec(command, stream._stdinPending).then(result => {
      // Push collected output then close. ssh2 streams emit 'data' and
      // 'close' separately for stdout/stderr — we mirror that.
      if (result.stdout.length > 0) stream.push(result.stdout)
      stream.push(null)
      if (result.stderr.length > 0) stream.stderr.push(result.stderr)
      stream.stderr.push(null)
      stream.emit('exit', result.code, null)
      stream.emit('close', result.code, null)
    }).catch(err => {
      stream.destroy(err instanceof Error ? err : new Error(String(err)))
      stream.stderr.destroy(err instanceof Error ? err : new Error(String(err)))
    })
    queueMicrotask(() => callback(null, stream))
    return true
  }

  sftp(callback: (err: Error | null, sftp?: SFTPWrapper) => void): boolean {
    if (!this.connected || !this.session) {
      queueMicrotask(() => callback(new Error('Not connected')))
      return false
    }
    const sess = this.session
    sess.sftpInit().then(() => {
      callback(null, new SFTPWrapper(sess))
    }).catch(err => {
      callback(err instanceof Error ? err : new Error(String(err)))
    })
    return true
  }

  end(): this {
    if (this.ended) return this
    this.ended = true
    if (this.session) {
      try { this.session.end() } catch { /* ignore */ }
      this.session = undefined
    }
    queueMicrotask(() => this.emit('end'))
    queueMicrotask(() => this.emit('close'))
    return this
  }
}

/**
 * Exec channel — ssh2-shaped. The native binding currently delivers the
 * full stdout/stderr in one shot; we surface that through Readable's data
 * events so consumers that listen for 'data' / 'close' work unchanged.
 *
 * Future: switch the native binding to streaming reads and forward chunks
 * here as they arrive. The JS API is already streaming-shaped.
 */
export class ExecStream extends Readable {
  stderr: Readable
  /** Bytes queued by callers before exec() actually runs (rare). */
  _stdinPending?: Buffer

  /** Stub stdin. Writes are buffered and delivered as part of the exec
   *  request. Only useful when called synchronously after .exec(). */
  stdin = {
    write: (chunk: string | Buffer): boolean => {
      const buf = typeof chunk === 'string' ? Buffer.from(chunk, 'utf-8') : chunk
      this._stdinPending = this._stdinPending ? Buffer.concat([this._stdinPending, buf]) : buf
      return true
    },
    end: (chunk?: string | Buffer): void => {
      if (chunk) this.stdin.write(chunk)
    },
    on: (_e: string, _cb: () => void) => this.stdin,
  }

  constructor() {
    super()
    this.stderr = new Readable({ read() {} })
  }
  _read(): void { /* no-op: data is push()'d when exec resolves */ }

  /** ssh2 exposes this on the channel. We send a remote signal via... well,
   *  not yet. Punt: just close the channel. */
  signal(_sig: string): void { /* TODO: implement via libssh ssh_channel_request_send_signal */ }
  close(): void { this.destroy() }
}

/**
 * SFTPWrapper — subset of ssh2's SFTPWrapper API used by Dario.
 * All methods accept the ssh2 callback shape: `(err, result?) => void`.
 */
export class SFTPWrapper extends EventEmitter {
  constructor(private session: NativeSession) { super() }

  readFile(path: string, callback: (err: Error | null, data?: Buffer) => void): void {
    this.session.sftpReadFile(path)
      .then(buf => callback(null, buf))
      .catch(err => callback(err instanceof Error ? err : new Error(String(err))))
  }

  writeFile(path: string, data: Buffer | string, callback: (err: Error | null) => void): void {
    const buf = typeof data === 'string' ? Buffer.from(data, 'utf-8') : data
    this.session.sftpWriteFile(path, buf)
      .then(() => callback(null))
      .catch(err => callback(err instanceof Error ? err : new Error(String(err))))
  }

  stat(path: string, callback: (err: Error | null, stats?: SftpStats) => void): void {
    this.session.sftpStat(path)
      .then(s => callback(null, {
        size: s.size,
        mtime: s.mtime,
        isDirectory: () => s.isDirectory,
        isFile: () => s.isFile,
      }))
      .catch(err => callback(err instanceof Error ? err : new Error(String(err))))
  }

  rename(oldPath: string, newPath: string, callback: (err: Error | null) => void): void {
    this.session.sftpRename(oldPath, newPath)
      .then(() => callback(null))
      .catch(err => callback(err instanceof Error ? err : new Error(String(err))))
  }

  unlink(path: string, callback: (err: Error | null) => void): void {
    this.session.sftpUnlink(path)
      .then(() => callback(null))
      .catch(err => callback(err instanceof Error ? err : new Error(String(err))))
  }

  mkdir(path: string, callback: (err: Error | null) => void): void {
    this.session.sftpMkdir(path)
      .then(() => callback(null))
      .catch(err => callback(err instanceof Error ? err : new Error(String(err))))
  }

  /**
   * createReadStream — ssh2-compatible streaming Read. Currently delegates
   * to readFile under the hood; range support and true streaming will be
   * added when the binding gets sftp_open / sftp_read with offsets.
   */
  createReadStream(path: string, opts?: { start?: number; end?: number; encoding?: BufferEncoding }): Readable {
    const r = new Readable({ read() {} })
    this.session.sftpReadFile(path).then(buf => {
      let view: Buffer = buf
      if (opts?.start !== undefined || opts?.end !== undefined) {
        const start = opts?.start ?? 0
        const end = opts?.end !== undefined ? Math.min(opts.end + 1, buf.length) : buf.length
        view = buf.subarray(start, end)
      }
      if (opts?.encoding) r.push(view.toString(opts.encoding))
      else r.push(view)
      r.push(null)
    }).catch(err => r.destroy(err instanceof Error ? err : new Error(String(err))))
    return r
  }

  end(): void { /* no-op — the parent Client.end() tears down sftp */ }
}
