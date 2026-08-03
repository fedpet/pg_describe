import { readFile } from 'node:fs/promises'
import { dirname, isAbsolute, resolve } from 'node:path'

export interface Config {
  /** Directory scanned recursively for .sql files. Relative to the config file. */
  queries: string
  /** File the generated TypeScript is written to. Relative to the config file. */
  output: string
  /** Extra or overriding PostgreSQL type name -> TypeScript type mappings. */
  types?: Record<string, string>
}

export interface ResolvedConfig extends Config {
  /** Directory the config file lives in; all paths resolve against it. */
  root: string
  queriesDir: string
  outputFile: string
}

const DEFAULT_FILE = 'pg-describe.json'

/**
 * Connection settings are not part of this file: node-postgres reads
 * DATABASE_URL and the standard PG* variables, keeping credentials out of a
 * committed file.
 */
export async function loadConfig(explicitPath?: string): Promise<ResolvedConfig> {
  const path = resolve(explicitPath ?? DEFAULT_FILE)

  let raw: string
  try {
    raw = await readFile(path, 'utf8')
  } catch {
    throw new Error(
      `No config file at ${path}.\n` +
        `Create one:\n\n` +
        `  {\n` +
        `    "queries": "queries",\n` +
        `    "output": "src/generated/queries.ts"\n` +
        `  }\n`,
    )
  }

  let parsed: Partial<Config>
  try {
    parsed = JSON.parse(raw) as Partial<Config>
  } catch (err) {
    throw new Error(`${path} is not valid JSON: ${(err as Error).message}`)
  }

  if (typeof parsed.queries !== 'string' || parsed.queries.length === 0) {
    throw new Error(`${path}: "queries" must be a directory path.`)
  }
  if (typeof parsed.output !== 'string' || parsed.output.length === 0) {
    throw new Error(`${path}: "output" must be a file path.`)
  }

  const root = dirname(path)
  const against = (p: string) => (isAbsolute(p) ? p : resolve(root, p))

  return {
    queries: parsed.queries,
    output: parsed.output,
    types: parsed.types ?? {},
    root,
    queriesDir: against(parsed.queries),
    outputFile: against(parsed.output),
  }
}
