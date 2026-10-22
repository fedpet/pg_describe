# Contributing

## Repository layout

```
src/pg_describe.c        extension implementation
sql/                     installation and upgrade scripts
test/                    pg_regress suite and upgrade checks
scripts/                 release metadata checks
docs/                    documentation source
```

## Testing

```bash
python3 scripts/check-release.py
docker compose up -d --build --wait db
docker compose exec -T db bash -lc \
  'cd /src/pg_describe && PGUSER=postgres PGHOST=/var/run/postgresql \
   PGDATABASE=contrib_regression make installcheck'
docker compose exec -T db psql -X -U postgres -d postgres \
  -v ON_ERROR_STOP=1 -f /src/pg_describe/test/upgrades.sql
```

The regression suite covers parameter inference, provenance, expression
nullability, joins, grouping, JSON shapes, arrays, errors and permissions.
The upgrade checks compare the function definition and described results after
each supported upgrade path with a fresh installation. They run in a transaction
that is rolled back.

Analysis changes belong with regression SQL and expected output. CI tests
PostgreSQL 18. Existing compatibility code can remain without making additional
server versions supported.

## Documentation

Documentation lives in `docs/` as Markdown and is read directly on GitHub.
Keep links relative so they work in the repository and source archives.

## Versions

The extension uses semantic versions independently of PostgreSQL server versions:

- Patch: correctness fixes preserving the documented contract.
- Minor: backward-compatible capabilities.
- Major: incompatible function signatures or metadata behavior.

Git tags use `vMAJOR.MINOR.PATCH`; PostgreSQL extension versions omit the `v`.
Never edit or move a released tag. Historical installation and update scripts
remain unchanged; new releases add scripts instead.

## Preparing a release

1. Update `pg_describe.control` and both extension versions in `META.json`.
2. Add the new installation script and update scripts from supported versions.
   The Makefile installs all versioned SQL scripts. A C-only fix can use an empty
   update script to advance the installed version.
3. Add release notes under `## VERSION` in `CHANGELOG.md`.
4. Update upgrade checks to cover the supported paths and target version.
5. Run metadata, regression and upgrade checks, then merge the
   preparation PR into `main`.

The first independently maintained release is 2.0.0. It removes the bundled
language-specific generator and establishes the JSON and array metadata API.

## Publishing a release

After the preparation PR is merged and its checks pass:

```bash
python3 scripts/check-release.py --tag v2.0.0
git tag -a v2.0.0 -m "pg_describe 2.0.0"
git push origin v2.0.0
```

The tag workflow reruns CI, verifies that the commit belongs to `main` and that
the tag matches the extension metadata, then publishes a GitHub release with a
source archive and SHA-256 checksum. Notes come from the matching changelog
entry. There is no registry publication or binary distribution.

The workflow assembles a draft with all assets before publishing it, so it also
works when release immutability is enabled in repository settings. A published
version is final; corrections need a new patch release.
