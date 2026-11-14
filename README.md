# postrcs

`postrcs` provides an `rcs_archive` domain and SQL functions for working with
text RCS archives.

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
- `rcs_checkout(archive, revision)` returns the content of a trunk revision.
- `rcs_diff(archive, revision_a, revision_b)` returns a unified diff.
- `rcs_revisions(archive)` returns revision number, author, date, state, and
  log for each revision.

RCS content must be newline-terminated when non-empty. Checkout currently
supports trunk revisions; branch checkout and branch check-in are not
implemented. `rcs_diff` emits a valid whole-file unified diff rather than
minimizing changed lines.
