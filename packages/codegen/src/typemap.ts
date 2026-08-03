/**
 * PostgreSQL type name -> TypeScript type.
 *
 * Names are what `format_type_be` prints: the SQL spelling ("integer",
 * "character varying(10)"), not the catalog spelling ("int4", "varchar").
 *
 * bigint and numeric map to `string` because that is what node-postgres
 * returns — neither survives a JavaScript number. Override via the `types`
 * block in the config if you have installed custom type parsers.
 */

/** Exact matches, checked first. */
const EXACT: Record<string, string> = {
  smallint: 'number',
  integer: 'number',
  real: 'number',
  'double precision': 'number',

  bigint: 'string', // not number: see the header note

  boolean: 'boolean',

  text: 'string',
  uuid: 'string',
  name: 'string',
  citext: 'string',
  inet: 'string',
  cidr: 'string',
  macaddr: 'string',
  interval: 'string',
  money: 'string',
  xml: 'string',

  date: 'Date',

  bytea: 'Buffer',

  json: 'unknown',
  jsonb: 'unknown',

  // Present in a target list only for something like `SELECT NULL`.
  void: 'void',
}

/**
 * Prefix matches, checked after the exact table. Order matters: the first
 * match wins, so `timestamp with time zone` must not be shadowed by a shorter
 * `time` prefix — which is why `timestamp` is listed before `time`.
 */
const PREFIX: Array<[string, string]> = [
  ['character varying', 'string'],
  ['character', 'string'],
  ['numeric', 'string'],
  ['decimal', 'string'],
  ['timestamp', 'Date'],
  ['time', 'string'], // time / time with time zone have no Date equivalent
  ['bit', 'string'],
]

export interface TypeMapResult {
  ts: string
  /** True when nothing matched and `unknown` was used as a fallback. */
  unmapped: boolean
}

/**
 * Map a PostgreSQL type name to a TypeScript type.
 *
 * Arrays are handled by recursion: `integer[]` becomes `number[]`, and
 * `text[][]` becomes `string[][]`, because format_type_be prints one `[]` per
 * dimension.
 */
export function tsTypeFor(
  pgType: string,
  overrides: Record<string, string> = {},
): TypeMapResult {
  const name = pgType.trim()

  // User overrides win over everything, including arrays, so that someone can
  // map `int8[]` wholesale if they want to.
  const override = overrides[name]
  if (override !== undefined) return { ts: override, unmapped: false }

  if (name.endsWith('[]')) {
    const inner = tsTypeFor(name.slice(0, -2), overrides)
    // Parenthesise unions so `a | null` does not become `a | null[]`.
    const base = inner.ts.includes('|') ? `(${inner.ts})` : inner.ts
    return { ts: `${base}[]`, unmapped: inner.unmapped }
  }

  const exact = EXACT[name]
  if (exact !== undefined) return { ts: exact, unmapped: false }

  for (const [prefix, ts] of PREFIX) {
    if (name.startsWith(prefix)) return { ts, unmapped: false }
  }

  // Enums, domains and user-defined types. `unknown` forces the consumer to
  // narrow rather than trust a wrong guess.
  return { ts: 'unknown', unmapped: true }
}
