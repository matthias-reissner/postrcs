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
- `rcs_diff(archive, revision_a, revision_b)` returns a unified diff.
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
trunk revisions; nested branches are not supported. Deltas and unified diffs
preserve common prefixes and suffixes, but multiple separate edits may still
be represented as one larger changed region. The extension stores archives as
text and does not version binary data.

The extension's default version is 1.1. Existing 1.0 installations can be
upgraded with `ALTER EXTENSION postrcs UPDATE;`.

For SQL examples and a usage walkthrough, see [USAGE.md](./USAGE.md).
