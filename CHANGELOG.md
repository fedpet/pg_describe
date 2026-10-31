# Changelog

## 2.2.0

- Add statement/COPY transport metadata and language-neutral finite value facts.
- Infer direct validated text/enum membership, domain intersections and array-element containment without executing expressions.
- Refine positive WHERE non-null facts conservatively and retain unknown facts for opaque or volatile expressions.
- Describe CALL OUT/INOUT result columns without invoking procedures.

## 2.1.0

- Add bulk routine introspection with exact signatures, defaults and symbolic non-null conditions.
- Share function proofs with query description, including clocks and analyzable SQL bodies.
- Keep unknown, recursive, opaque and potentially empty SQL bodies conservative.

## 2.0.0

First independently maintained release, based on sajonaro/pg_describe.

### Breaking changes

- Remove the bundled TypeScript generator, its examples, and npm publication.
- Remove the documentation website, its build tooling, and Pages deployment;
  keep documentation as Markdown in the repository.
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
