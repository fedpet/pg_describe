/**
 * Uses the generated types against a real database.
 *
 *   PGHOST=localhost PGPORT=5432 PGUSER=postgres PGPASSWORD=postgres \
 *   PGDATABASE=pg_describe_demo node src/demo.ts
 *
 * The interesting line is marked below: `email` is `string | null`, so
 * TypeScript will not let this file compile without handling the guest-checkout
 * case. That is the whole point of the exercise — the type system knows about a
 * NULL the schema alone does not predict.
 */
import pg from 'pg'

import {
  dailyOrderTotals,
  findCustomerByEmail,
  listRecentOrders,
} from './generated/queries.ts'

const client = new pg.Client()
await client.connect()

try {
  const orders = await listRecentOrders(client, { p1: new Date('2000-01-01') })

  console.log('recent orders')
  for (const order of orders) {
    // `order.email` is `string | null`, because customers is LEFT JOINed and
    // pg_describe reports that the join can null-extend it.
    //
    // The annotation on `who` is what makes this a real check rather than a
    // decorative one: drop the `?? '(guest)'` and `npm run typecheck` fails
    // with "Type 'string | null' is not assignable to type 'string'". A
    // generator that read attnotnull alone would have typed this `string` and
    // let the guest order through to a null-dereference at run time.
    //
    // (Note that a bare template literal would NOT catch it — `${x}` happily
    // stringifies null. Nullability only bites where a string is required.)
    const who: string = order.email ?? '(guest)'
    console.log(`  #${order.id}  ${order.total}  ${who}`)
  }

  const [customer] = await findCustomerByEmail(client, { p1: 'ada@example.com' })
  // No LEFT JOIN here, so `email` is plain `string` — but the row itself may be
  // absent, which is why this is destructured and checked.
  console.log('\nlookup')
  console.log(customer ? `  ${customer.email} vip=${customer.vip}` : '  not found')

  console.log('\ndaily totals')
  for (const day of await dailyOrderTotals(client)) {
    // Aggregates over an empty group are NULL, so these are nullable too.
    console.log(`  ${day.day?.toISOString().slice(0, 10) ?? '?'}  n=${day.order_count}  ${day.revenue}`)
  }
} finally {
  await client.end()
}
