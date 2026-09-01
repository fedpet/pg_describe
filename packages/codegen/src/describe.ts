import type { ClientBase } from 'pg'

/** One row of `pg_describe(...)` output. */
export interface DescribeRow {
  kind: 'param' | 'column'
  ord: number
  name: string | null
  typeOid: number
  typeName: string
  sourceTable: string | null
  sourceColumn: string | null
  baseNotNull: boolean | null
  resultNotNull: boolean | null
  resultShape: unknown | null
}

export interface Described {
  params: DescribeRow[]
  columns: DescribeRow[]
}

const SQL = `
  SELECT kind, ord, name, type_oid, type_name,
         source_table::text AS source_table,
         source_column, base_not_null, result_not_null, result_shape
  FROM pg_describe($1)
  ORDER BY ord
`

/**
 * Ask the server what a statement would return, without running it.
 *
 * This is one round trip. The extension does the parse and the analysis in the
 * backend and stops before the executor, so nothing in `sql` is executed --
 * describing `DELETE FROM orders` is safe.
 */
export async function describe(client: ClientBase, sql: string): Promise<Described> {
  const res = await client.query(SQL, [sql])

  const rows: DescribeRow[] = res.rows.map((r: Record<string, unknown>) => ({
    kind: r.kind as 'param' | 'column',
    ord: Number(r.ord),
    name: (r.name as string | null) ?? null,
    typeOid: Number(r.type_oid),
    typeName: r.type_name as string,
    sourceTable: (r.source_table as string | null) ?? null,
    sourceColumn: (r.source_column as string | null) ?? null,
    baseNotNull: (r.base_not_null as boolean | null) ?? null,
    resultNotNull: (r.result_not_null as boolean | null) ?? null,
    resultShape: r.result_shape ?? null,
  }))

  return {
    params: rows.filter((r) => r.kind === 'param'),
    columns: rows.filter((r) => r.kind === 'column'),
  }
}

/**
 * Fail early and legibly when the extension is missing, rather than letting
 * every query fail with an opaque "function pg_describe(unknown) does not
 * exist".
 */
export async function assertExtensionInstalled(client: ClientBase): Promise<void> {
  const res = await client.query(
    `SELECT extversion FROM pg_extension WHERE extname = 'pg_describe'`,
  )
  if (res.rowCount === 0) {
    throw new Error(
      'The pg_describe extension is not installed in this database.\n' +
        "  Run:  CREATE EXTENSION pg_describe;\n" +
        '  See:  https://github.com/sajonaro/pg_describe#installation',
    )
  }
}
