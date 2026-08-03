import { readdir, readFile } from 'node:fs/promises'
import { join, relative, sep } from 'node:path'

export interface SqlQuery {
  /** The name from the `-- @name Foo` annotation. */
  name: string
  /** The statement text, with the annotation, doc comment and trailing semicolon removed. */
  sql: string
  /**
   * The comment block between the `@name` annotation and the statement, with
   * the `--` markers stripped. Becomes the generated function's JSDoc, so a
   * query documents itself all the way through to the caller's editor.
   */
  doc: string[]
  /** Path relative to the project root, for error messages and comments. */
  file: string
  /** 1-based line the annotation sits on, for error messages. */
  line: number
}

const NAME_RE = /^\s*--\s*@name\s+([A-Za-z_][A-Za-z0-9_]*)\s*$/

/** Every .sql file under `dir`, recursively, in a stable order. */
async function sqlFiles(dir: string): Promise<string[]> {
  const entries = await readdir(dir, { withFileTypes: true })
  const found: string[] = []

  for (const entry of entries.sort((a, b) => a.name.localeCompare(b.name))) {
    const full = join(dir, entry.name)
    if (entry.isDirectory()) found.push(...(await sqlFiles(full)))
    else if (entry.isFile() && entry.name.endsWith('.sql')) found.push(full)
  }

  return found
}

/**
 * Split one file into its annotated queries.
 *
 * The format is deliberately minimal, and deliberately still valid SQL:
 *
 *     -- @name ListVipOrders
 *     SELECT o.id, c.email
 *     FROM orders o LEFT JOIN customers c ON c.id = o.customer_id
 *     WHERE o.placed_at >= $1;
 *
 * Placeholders are native `$1`, not a bespoke `:name` syntax, so the file you
 * generate types from is a file you can paste straight into psql. That is the
 * main ergonomic difference from pgTyped, and it falls out of pg_describe
 * taking real SQL rather than a rewritten dialect.
 */
export function parseQueries(text: string, file: string): SqlQuery[] {
  const lines = text.split('\n')
  const queries: SqlQuery[] = []

  let current: { name: string; line: number; body: string[] } | null = null

  const flush = () => {
    if (!current) return

    // Split the leading comment block off the statement. Everything from the
    // annotation down to the first line of real SQL is documentation; keeping
    // it out of the statement means the emitted `...Sql` constant is the query
    // and nothing else, while the prose still reaches the caller as JSDoc.
    const body = [...current.body]
    const doc: string[] = []

    while (body.length > 0) {
      const line = body[0]!.trim()
      if (line === '') {
        body.shift()
        // A blank line inside the doc block is kept; a blank line before it
        // is not, which is what `doc.length > 0` distinguishes.
        if (doc.length > 0) doc.push('')
        continue
      }
      if (!line.startsWith('--')) break
      doc.push(line.replace(/^--\s?/, ''))
      body.shift()
    }

    while (doc.length > 0 && doc[doc.length - 1] === '') doc.pop()

    const sql = body.join('\n').trim().replace(/;\s*$/, '').trim()
    if (sql.length > 0) {
      queries.push({ name: current.name, sql, doc, file, line: current.line })
    }
    current = null
  }

  lines.forEach((line, i) => {
    const m = NAME_RE.exec(line)
    if (m && m[1]) {
      flush()
      current = { name: m[1], line: i + 1, body: [] }
    } else if (current) {
      current.body.push(line)
    }
    // Text before the first @name is ignored: it is a file header comment.
  })
  flush()

  return queries
}

/** Read and parse every annotated query under `dir`. */
export async function discover(dir: string, root: string): Promise<SqlQuery[]> {
  const files = await sqlFiles(dir)
  const all: SqlQuery[] = []

  for (const file of files) {
    const text = await readFile(file, 'utf8')
    const rel = relative(root, file).split(sep).join('/')
    all.push(...parseQueries(text, rel))
  }

  const seen = new Map<string, string>()
  for (const q of all) {
    const previous = seen.get(q.name)
    if (previous !== undefined) {
      throw new Error(
        `Duplicate query name "${q.name}" in ${q.file} — already defined in ${previous}. ` +
          'Names become exported identifiers, so they must be unique across all files.',
      )
    }
    seen.set(q.name, q.file)
  }

  return all
}
