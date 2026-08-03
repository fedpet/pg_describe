import type { Described, DescribeRow } from './describe.js'
import type { SqlQuery } from './discover.js'
import { tsTypeFor } from './typemap.js'

export interface EmitOptions {
  /** Extra or overriding PostgreSQL type name -> TypeScript type mappings. */
  types?: Record<string, string>
}

export interface DescribedQuery {
  query: SqlQuery
  described: Described
}

export interface EmitResult {
  code: string
  /** PostgreSQL type names that had no mapping and fell back to `unknown`. */
  unmapped: Set<string>
}

/** ListVipOrders -> listVipOrders */
function lowerFirst(s: string): string {
  return s.length > 0 ? s[0]!.toLowerCase() + s.slice(1) : s
}

/**
 * A column name is emitted as a bare identifier when it can be, and quoted when
 * it cannot. `SELECT count(*)` yields the column name `count`, which is fine;
 * `SELECT 1 AS "odd name"` needs quoting.
 */
function propertyKey(name: string): string {
  return /^[A-Za-z_$][A-Za-z0-9_$]*$/.test(name) ? name : JSON.stringify(name)
}

/** Escape SQL for embedding in a template literal. */
function templateLiteral(sql: string): string {
  return sql.replace(/\\/g, '\\\\').replace(/`/g, '\\`').replace(/\$\{/g, '\\${')
}

/**
 * Nullability of a result column.
 *
 * `result_not_null` is pg_describe's outer-join-aware answer. It is:
 *   true   guaranteed non-null
 *   false  can be null
 *   null   unknown — an expression, with no source column to reason from
 *
 * Unknown is treated as nullable. Over-declaring a null costs the consumer one
 * impossible check; under-declaring it costs them a crash in production.
 */
function isNullable(col: DescribeRow): boolean {
  return col.resultNotNull !== true
}

function emitRowInterface(
  name: string,
  columns: DescribeRow[],
  opts: EmitOptions,
  unmapped: Set<string>,
): string {
  const fields = columns.map((col, i) => {
    const mapped = tsTypeFor(col.typeName, opts.types)
    if (mapped.unmapped) unmapped.add(col.typeName)

    const key = propertyKey(col.name ?? `column${i + 1}`)
    const type = isNullable(col) ? `${mapped.ts} | null` : mapped.ts

    // Provenance, where there is any, makes the generated file reviewable:
    // you can see at a glance which table a field came from.
    const from =
      col.sourceTable && col.sourceColumn
        ? `  // ${col.sourceTable}.${col.sourceColumn}`
        : ''

    return `  ${key}: ${type}${from ? `${from}` : ''}`
  })

  return `export interface ${name} {\n${fields.join('\n')}\n}`
}

function emitParamsInterface(
  name: string,
  params: DescribeRow[],
  opts: EmitOptions,
  unmapped: Set<string>,
): string {
  const fields = params.map((p) => {
    const mapped = tsTypeFor(p.typeName, opts.types)
    if (mapped.unmapped) unmapped.add(p.typeName)
    // Parameters are declared non-null. Passing null is legal SQL, but it is
    // almost always a bug rather than an intent, and `p1: T | null` on every
    // parameter makes the generated types tiresome to use.
    return `  p${p.ord}: ${mapped.ts}`
  })

  return `export interface ${name} {\n${fields.join('\n')}\n}`
}

function emitQuery(
  dq: DescribedQuery,
  opts: EmitOptions,
  unmapped: Set<string>,
): string {
  const { query, described } = dq
  const { params, columns } = described

  const fn = lowerFirst(query.name)
  const paramsType = `${query.name}Params`
  const rowType = `${query.name}Row`

  const parts: string[] = []

  parts.push(`// ${'-'.repeat(72)}\n// ${query.name}  (${query.file}:${query.line})\n// ${'-'.repeat(72)}`)

  if (params.length > 0) {
    parts.push(emitParamsInterface(paramsType, params, opts, unmapped))
  }

  if (columns.length > 0) {
    parts.push(emitRowInterface(rowType, columns, opts, unmapped))
  }

  parts.push(`export const ${fn}Sql = \`${templateLiteral(query.sql)}\`;`)

  const args = params.length > 0 ? `client: ClientBase, params: ${paramsType}` : 'client: ClientBase'
  const values =
    params.length > 0 ? `[${params.map((p) => `params.p${p.ord}`).join(', ')}]` : '[]'

  // The query's own comment block becomes the function's JSDoc. Editors show it
  // on hover at the call site, so a query documents itself all the way to
  // whoever uses it, not just to whoever opens the .sql file.
  const docLines = [
    ...query.doc,
    ...(query.doc.length > 0 ? [''] : []),
    `@see {@link ${fn}Sql} — defined in ${query.file}`,
  ]
  const jsdoc = ['/**', ...docLines.map((l) => (l === '' ? ' *' : ` * ${l}`)), ' */'].join('\n')

  if (columns.length > 0) {
    parts.push(
      `${jsdoc}\n` +
        `export async function ${fn}(${args}): Promise<${rowType}[]> {\n` +
        `  const result = await client.query<${rowType}>(${fn}Sql, ${values});\n` +
        `  return result.rows;\n` +
        `}`,
    )
  } else {
    // No result columns: an INSERT/UPDATE/DELETE without RETURNING. The useful
    // return value is the number of rows affected.
    parts.push(
      `${jsdoc}\n` +
        `export async function ${fn}(${args}): Promise<number> {\n` +
        `  const result = await client.query(${fn}Sql, ${values});\n` +
        `  return result.rowCount ?? 0;\n` +
        `}`,
    )
  }

  return parts.join('\n\n')
}

export function emit(queries: DescribedQuery[], opts: EmitOptions = {}): EmitResult {
  const unmapped = new Set<string>()

  const header = [
    '// Code generated by pg-describe-gen. DO NOT EDIT.',
    '//',
    '// Types come from the database itself: each statement below was parsed and',
    '// analysed by PostgreSQL via the pg_describe extension, and none of them was',
    '// executed. Regenerate with `npx pg-describe-gen`.',
    '',
    "import type { ClientBase } from 'pg';",
  ].join('\n')

  const body = queries.map((q) => emitQuery(q, opts, unmapped)).join('\n\n')

  return { code: `${header}\n\n${body}\n`, unmapped }
}
