# Changelog

## 2.0.0

First independently maintained release, based on sajonaro/pg_describe.

### Breaking changes

- Remove the bundled TypeScript generator, its examples, and npm publication.
- Extend the function result with recursive JSON shapes, SQL array dimensions,
  and array element nullability.
- Support and test PostgreSQL 18.

### Query analysis

- Resolve PostgreSQL 18 grouping references to their original expressions.
- Infer nullability for recognized expressions conservatively.
- Describe nested JSON objects, arrays, scalar subqueries and scalar provenance.
- Infer array rank and element nullability from proven expressions and enforced
  schema constraints.

### Installation and releases

- Provide fresh installation and upgrade paths from 1.0.0 and 1.1.0.
- Validate release metadata and compare upgrades with fresh installation in CI.
- Publish versioned source archives and checksums through GitHub releases.
