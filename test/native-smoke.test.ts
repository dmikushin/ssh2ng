import { expect, test } from 'bun:test'
import { createRequire } from 'node:module'
import { Client } from '../lib/index.ts'

const require = createRequire(import.meta.url)

test('native addon exposes ssh2ng session classes', () => {
  const native = require('../build/Release/ssh2ng.node')

  expect(typeof native.SshSession).toBe('function')
  expect(typeof native.SshExecChannel).toBe('function')
  expect(typeof native.SshSftpReadStream).toBe('function')
})

test('client and native session can be constructed and closed', () => {
  const native = require('../build/Release/ssh2ng.node')
  const session = new native.SshSession()
  session.end()

  const client = new Client()
  client.end()
})
