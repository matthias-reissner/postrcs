# postrcs

`postrcs` provides an `rcs_archive` domain and SQL functions for working with
text RCS archives, including trunk revisions, branches, and symbolic tags.

Try the [interactive GitHub Pages demo](https://matthias-reissner.github.io/postrcs/).

## Build and install

The extension uses PostgreSQL's PGXS build system and requires the server
development files for the PostgreSQL installation where it will be loaded.
Use a recent GNU Make (4.x recommended) and ensure `pg_config` from that
installation is on `PATH`:

```sh
make
make install
```

On Windows, use an x64 Visual Studio developer prompt and put MSYS2's
`usr\bin` and PostgreSQL's `bin` directories on `PATH`. For example:

```bat
set "PATH=%VCToolsInstallDir%bin\Hostx64\x64;C:\Users\Matthias\Desktop\msys2\msys64\usr\bin;%USERPROFILE%\PostgreSQL\18\bin;%PATH%"
make
```

`make install` needs write access to PostgreSQL's library and extension
directories. Run it from an elevated prompt if the installation directory is
protected.

In a database where the shared library is available:

```sql
CREATE EXTENSION postrcs;
```

## SQL API

- `rcs_archive` stores a textual RCS archive and checks its structure. The
  empty string is accepted as an uninitialized archive and can be used for the
  first check-in.
- `rcs_checkin(archive, content, log)` returns an archive with a new trunk
  revision. The author is the current database user and the date is the
  database server's current time.
- `rcs_checkout(archive, revision_or_symbol)` returns a trunk or branch
  revision; symbolic revision names are accepted.
- `rcs_diff(archive, revision_a, revision_b)` returns a multi-hunk unified
  diff.
- `rcs_merge(archive, base, ours, theirs)` returns `rcs_merge_result` with
  merged `content` and a `has_conflicts` flag. Conflicts are marked in the
  returned text with `<<<<<<< ours`, `=======`, and `>>>>>>> theirs`.
- `rcs_import(text)` validates and imports an existing RCS archive;
  `rcs_export(archive)` returns its archive text.
- `rcs_revisions(archive)` returns revision number, author, date, state, and
  log for each revision.
- `rcs_head(archive)` returns the current trunk head.
- `rcs_resolve_revision(archive, name)` resolves a revision or symbol.
- `rcs_create_branch(archive, name, from_revision)` creates a named branch
  from a trunk revision.
- `rcs_branch_checkin(archive, name, content, log)` adds a branch revision.
- `rcs_tag(archive, name, revision)` adds or updates a symbolic revision tag.
- `rcs_symbols(archive)` lists symbolic tags and branches.

Non-empty text must end with a newline. Branches can currently be created from
trunk revisions; nested branches are not supported. Contents are text, not
binary data. Archives are limited to 16 MiB, individual check-in contents to
8 MiB, and log messages to 1 MiB. Diff and merge use an LCS comparison capped
at 4 million line pairs; large comparisons beyond that bound fail explicitly.
The benchmark script in [benchmarks/postrcs_bench.sql](./benchmarks/postrcs_bench.sql)
provides repeatable 1,000-line check-in and near-limit 1,900-line diff/merge
workloads without enforcing machine-dependent timing thresholds.

The extension's default version is 1.2.1. Existing 1.0, 1.1, and 1.2
installations can be upgraded with `ALTER EXTENSION postrcs UPDATE;`.

Run the repeatable timing workload in a database with PostRCS installed:

```sh
psql -d your_database -f benchmarks/postrcs_bench.sql
```

For SQL examples and a usage walkthrough, see [USAGE.md](./USAGE.md).
