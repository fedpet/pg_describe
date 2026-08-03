/**
 * PostgreSQL type name -> TypeScript type.
 *
 * The type names here are what `format_type_be` prints, which is the SQL
 * spelling ("integer", "character varying(10)", "timestamp with time zone"),
 * not the catalog spelling ("int4", "varchar", "timestamptz").
 *
 * Two mappings routinely surprise people, and both are correct:
 *
 *   bigint  -> string    An int8 does not fit in a JavaScript number. Rather
 *                        than silently lose precision above 2^53, node-postgres
 *                        returns it as a string, so that is what we declare.
 *
 *   numeric -> string    Same reasoning, more urgently: numeric is arbitrary
 *                        precision and exists precisely because floats are
 *                        wrong for money. node-postgres returns it as a string.
 *
 * If you have configured node-postgres type parsers to override either of
 * those, use the `types` block in your config to say so.
 */

/** Exact matches, checked first. */
const EXACT: Record<string, string> = {
  smallint: 'number',
  integer: 'number',
  real: 'number',
  'double precision': 'number',

  // See the note above: not `number`, on purpose.
  bigint: 'string',

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
  ['numeric', 'string'], // see the note above
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

  // Enums, domains and user-defined types land here. `unknown` is deliberate:
  // it compiles, and it forces the consumer to narrow rather than quietly
  // trusting a wrong guess.
  return { ts: 'unknown', unmapped: true }
}
