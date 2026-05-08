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
  SshExecChannel: new (session: NativeSession, command: string, stdin?: Buffer) => NativeExecChannel
  SshSftpReadStream: new (session: NativeSession, path: string, offset?: number, length?: number) => NativeSftpReadStream
}

interface NativeSession {
  connect(opts: { host: string; username?: string; port?: number; timeoutSec?: number; acceptAnyHostKey?: boolean }): Promise<void>
  exec(cmd: string, stdin?: Buffer): Promise<{ stdout: Buffer; stderr: Buffer; code: number }>
  execStream(cmd: string, stdin?: Buffer): NativeExecChannel
  sftpInit(): Promise<void>
  sftpReadFile(path: string): Promise<Buffer>
  sftpReadStream(path: string, offset?: number, length?: number): NativeSftpReadStream
  sftpWriteFile(path: string, data: Buffer, mode?: number): Promise<void>
  sftpStat(path: string): Promise<{ size: number; mtime: number; isDirectory: boolean; isFile: boolean }>
  sftpRename(oldPath: string, newPath: string): Promise<void>
  sftpUnlink(path: string): Promise<void>
  sftpMkdir(path: string, mode?: number): Promise<void>
  end(): void
}

interface NativeExecChannel {
  start(
    onData: (chunk: Buffer) => void,
    onStderr: (chunk: Buffer) => void,
    onClose: (info: { code: number; aborted: boolean; error?: string }) => void,
  ): void
  abort(): void
  writeStdin(buf: Buffer): void
  endStdin(): void
}

interface NativeSftpReadStream {
  start(
    onData: (chunk: Buffer) => void,
    onClose: (info: { bytes: number; aborted: boolean; error?: string }) => void,
  ): void
  abort(): void
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
    const native_channel = this.session.execStream(command)
    const stream = new ExecStream(native_channel)
    stream._wireUp()
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
 * Exec channel — ssh2-shaped. Backed by the native `SshExecChannel`,
 * which runs the libssh read loop on its own thread and pushes chunks
 * into JS via TSFN callbacks. Stdout chunks land on the stream's
 * built-in Readable; stderr chunks land on `stream.stderr`. The final
 * `'close'` event fires with the remote exit code (and `'exit'` for
 * ssh2 compatibility).
 *
 * `signal('TERM')` and `close()` both call `abort()` on the native
 * channel — the worker thread sends SIGTERM to the remote process at
 * its next 100ms poll boundary and closes the channel.
 */
export class ExecStream extends Readable {
  stderr: Readable
  private native_channel: NativeExecChannel
  private aborted = false

  /** Pre-start stdin pending. Used only if writes happen before the
   *  channel has been wired up (matches ssh2's order-tolerant shape). */
  _stdinPending?: Buffer
  stdin = {
    write: (chunk: string | Buffer): boolean => {
      const buf = typeof chunk === 'string' ? Buffer.from(chunk, 'utf-8') : chunk
      if (this._wired) this.native_channel.writeStdin(buf)
      else this._stdinPending = this._stdinPending ? Buffer.concat([this._stdinPending, buf]) : buf
      return true
    },
    end: (chunk?: string | Buffer): void => {
      if (chunk !== undefined) this.stdin.write(chunk)
      if (this._wired) this.native_channel.endStdin()
    },
    on: (_e: string, _cb: () => void) => this.stdin,
  }

  private _wired = false

  constructor(native_channel: NativeExecChannel) {
    // emitClose:false — we emit our own 'close' with (code, signal) so it
    // doesn't fight with Readable's automatic argless close on push(null).
    super({ emitClose: false, autoDestroy: false })
    this.stderr = new Readable({ read() {}, emitClose: false, autoDestroy: false })
    this.native_channel = native_channel
  }

  /** Called by Client.exec right after construction — registers TSFN
   *  callbacks and starts the worker. */
  _wireUp(): void {
    if (this._wired) return
    this._wired = true
    this.native_channel.start(
      (chunk: Buffer) => { this.push(chunk) },
      (chunk: Buffer) => { this.stderr.push(chunk) },
      (info: { code: number; aborted: boolean; error?: string }) => {
        this.push(null)
        this.stderr.push(null)
        if (info.error) this.emit('error', new Error(info.error))
        this.emit('exit', info.code, info.aborted ? 'TERM' : null)
        this.emit('close', info.code, info.aborted ? 'TERM' : null)
      },
    )
    if (this._stdinPending) {
      this.native_channel.writeStdin(this._stdinPending)
      this._stdinPending = undefined
    }
  }

  _read(): void { /* no-op: data is push()'d by TSFN callbacks */ }

  /** ssh2's exec channel exposes `.signal(name)` for SIGTERM/SIGKILL/etc.
   *  We translate any signal to channel-abort, which sends 'TERM' to the
   *  remote process and closes the channel. */
  signal(_sig: string): void {
    if (this.aborted) return
    this.aborted = true
    try { this.native_channel.abort() } catch { /* ignore */ }
  }

  close(): void {
    if (this.aborted) return
    this.aborted = true
    try { this.native_channel.abort() } catch { /* ignore */ }
  }
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
   * createReadStream — ssh2-compatible streaming read. Backed by the native
   * `SshSftpReadStream`, which streams 32 KB chunks via TSFN as they arrive
   * from the remote — no full-file buffering. Supports `start` / `end`
   * (inclusive byte offsets) and an `encoding` for string decoding.
   */
  createReadStream(path: string, opts?: { start?: number; end?: number; encoding?: BufferEncoding }): Readable {
    const r = new Readable({ read() {}, emitClose: false, autoDestroy: false })
    const start = opts?.start ?? 0
    const length = opts?.end !== undefined ? Math.max(0, opts.end - start + 1) : 0  // 0 → until EOF
    const native_stream = this.session.sftpReadStream(path, start, length)
    let aborted = false

    const r_with_destroy = r as Readable & { destroy(err?: Error): void }
    const orig_destroy = r_with_destroy.destroy.bind(r_with_destroy)
    r_with_destroy.destroy = (err?: Error) => {
      if (!aborted) { aborted = true; try { native_stream.abort() } catch { /* ignore */ } }
      return orig_destroy(err)
    }

    native_stream.start(
      (chunk: Buffer) => {
        if (opts?.encoding) r.push(chunk.toString(opts.encoding))
        else r.push(chunk)
      },
      (info: { bytes: number; aborted: boolean; error?: string }) => {
        r.push(null)
        if (info.error) r.emit('error', new Error(info.error))
        r.emit('close')
      },
    )
    return r
  }

  end(): void { /* no-op — the parent Client.end() tears down sftp */ }
}
