#!/usr/bin/env node
import { mkdir, readFile, writeFile } from 'node:fs/promises'
import { dirname, relative } from 'node:path'
import pg from 'pg'

import { loadConfig } from './config.js'
import { assertExtensionInstalled, describe } from './describe.js'
import { discover } from './discover.js'
import { emit, type DescribedQuery } from './emit.js'

const USAGE = `
pg-describe-gen — generate TypeScript types from plain .sql files

  pg-describe-gen [options]

Options:
  -c, --config <path>   Config file (default: pg-describe.json)
      --check           Do not write. Exit 1 if the output is out of date.
  -h, --help            Show this message

Connection settings come from the environment: DATABASE_URL, or the standard
PGHOST / PGPORT / PGUSER / PGPASSWORD / PGDATABASE variables.

--check is the point of this in CI: a schema change that breaks a query fails
the build instead of the deploy.
`.trim()

interface Args {
  config?: string
  check: boolean
  help: boolean
}

function parseArgs(argv: string[]): Args {
  const args: Args = { check: false, help: false }

  for (let i = 0; i < argv.length; i++) {
    const a = argv[i]
    if (a === '--check') args.check = true
    else if (a === '-h' || a === '--help') args.help = true
    else if (a === '-c' || a === '--config') args.config = argv[++i]
    else if (a !== undefined && a.startsWith('--config=')) args.config = a.slice('--config='.length)
    else throw new Error(`Unknown argument: ${a}\n\n${USAGE}`)
  }

  return args
}

async function main(): Promise<number> {
  const args = parseArgs(process.argv.slice(2))

  if (args.help) {
    process.stdout.write(`${USAGE}\n`)
    return 0
  }

  const config = await loadConfig(args.config)
  const queries = await discover(config.queriesDir, config.root)

  if (queries.length === 0) {
    process.stderr.write(
      `No annotated queries found under ${config.queriesDir}.\n` +
        `Each query needs a "-- @name SomeName" comment above it.\n`,
    )
    return 1
  }

  const client = new pg.Client()
  await client.connect()

  const described: DescribedQuery[] = []
  let failures = 0

  try {
    await assertExtensionInstalled(client)

    for (const query of queries) {
      try {
        described.push({ query, described: await describe(client, query.sql) })
      } catch (err) {
        // Report every broken query, not just the first: on a schema change
        // that breaks five queries you want all five in one run.
        failures++
        const message = err instanceof Error ? err.message : String(err)
        process.stderr.write(`✗ ${query.name} (${query.file}:${query.line})\n  ${message}\n`)
      }
    }
  } finally {
    await client.end()
  }

  if (failures > 0) {
    process.stderr.write(`\n${failures} of ${queries.length} queries failed to describe.\n`)
    return 1
  }

  const { code, unmapped } = emit(described, { types: config.types })

  for (const type of [...unmapped].sort()) {
    process.stderr.write(
      `warning: no TypeScript mapping for PostgreSQL type "${type}"; used \`unknown\`.\n` +
        `         Add it to "types" in your config to fix this.\n`,
    )
  }

  const rel = relative(process.cwd(), config.outputFile) || config.outputFile

  if (args.check) {
    let existing: string | null = null
    try {
      existing = await readFile(config.outputFile, 'utf8')
    } catch {
      existing = null
    }

    if (existing === code) {
      process.stdout.write(`${rel} is up to date (${described.length} queries).\n`)
      return 0
    }

    process.stderr.write(
      existing === null
        ? `${rel} does not exist. Run pg-describe-gen to create it.\n`
        : `${rel} is out of date. Run pg-describe-gen to regenerate it.\n`,
    )
    return 1
  }

  await mkdir(dirname(config.outputFile), { recursive: true })
  await writeFile(config.outputFile, code, 'utf8')

  process.stdout.write(`Wrote ${rel} (${described.length} queries).\n`)
  return 0
}

main()
  .then((code) => process.exit(code))
  .catch((err: unknown) => {
    process.stderr.write(`${err instanceof Error ? err.message : String(err)}\n`)
    process.exit(1)
  })
