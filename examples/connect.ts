/**
 * Quick smoke test: connect to a host using ~/.ssh/config (ProxyJump,
 * IdentityFile, agent) and run a few commands.
 *
 *   bun run examples/connect.ts <host>
 */
import { Client } from '../lib/index.js'

const host = process.argv[2] ?? 't580ng'

async function main() {
  const client = new Client()

  await new Promise<void>((resolve, reject) => {
    client.once('ready', resolve)
    client.once('error', reject)
    client.connect({ host })
  })
  console.log(`✓ connected to ${host}`)

  const out = await new Promise<{ stdout: Buffer; stderr: Buffer; code: number }>((resolve, reject) => {
    client.exec('ls -la /dev/ttyUSB* 2>&1 || echo NO_USB; uname -a', (err, stream) => {
      if (err) return reject(err)
      let so = Buffer.alloc(0), se = Buffer.alloc(0), code = -1
      stream!.on('data', (c: Buffer) => { so = Buffer.concat([so, c]) })
      stream!.stderr.on('data', (c: Buffer) => { se = Buffer.concat([se, c]) })
      stream!.on('close', (c: number) => { code = c; resolve({ stdout: so, stderr: se, code }) })
    })
  })
  console.log(`✓ exec exit=${out.code}`)
  console.log('--- stdout ---')
  console.log(out.stdout.toString('utf-8'))
  if (out.stderr.length) {
    console.log('--- stderr ---')
    console.log(out.stderr.toString('utf-8'))
  }

  // Also test SFTP read of /etc/hostname
  await new Promise<void>((resolve, reject) => {
    client.sftp((err, sftp) => {
      if (err) return reject(err)
      sftp!.readFile('/etc/hostname', (err2, data) => {
        if (err2) return reject(err2)
        console.log(`✓ sftp readFile /etc/hostname → ${JSON.stringify(data!.toString('utf-8').trim())}`)
        resolve()
      })
    })
  })

  client.end()
}

main().catch(err => { console.error('✗', err); process.exit(1) })
