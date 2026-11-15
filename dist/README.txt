PostRCS 1.2.1 - Windows x64 package for PostgreSQL 18

This package is built for PostgreSQL 18 and tested with PostgreSQL 18.7 on
Windows x64. Other PostgreSQL major versions, operating systems, and CPU
architectures need their own build.

Install the files from an elevated PowerShell or Command Prompt. Set PGROOT
to your PostgreSQL installation directory, then copy:

  lib\postrcs.dll
      to %PGROOT%\lib\

  share\extension\postrcs.control
  share\extension\postrcs--1.0.sql
  share\extension\postrcs--1.1.sql
  share\extension\postrcs--1.2.sql
  share\extension\postrcs--1.2.1.sql
  share\extension\postrcs--1.0--1.1.sql
  share\extension\postrcs--1.1--1.2.sql
  share\extension\postrcs--1.2--1.2.1.sql
      to %PGROOT%\share\extension\

For example, with PGROOT=C:\Users\Matthias\PostgreSQL\18:

  Copy-Item .\lib\postrcs.dll "$PGROOT\lib\"
  Copy-Item .\share\extension\postrcs.control "$PGROOT\share\extension\"
  Copy-Item .\share\extension\postrcs--1.0.sql "$PGROOT\share\extension\"
  Copy-Item .\share\extension\postrcs--1.1.sql "$PGROOT\share\extension\"
  Copy-Item .\share\extension\postrcs--1.2.sql "$PGROOT\share\extension\"
  Copy-Item .\share\extension\postrcs--1.2.1.sql "$PGROOT\share\extension\"
  Copy-Item .\share\extension\postrcs--1.0--1.1.sql "$PGROOT\share\extension\"
  Copy-Item .\share\extension\postrcs--1.1--1.2.sql "$PGROOT\share\extension\"
  Copy-Item .\share\extension\postrcs--1.2--1.2.1.sql "$PGROOT\share\extension\"

Then connect to the target database as a database administrator and run:

  CREATE EXTENSION postrcs;

The extension must be created separately in each database where it is used.
The PostgreSQL server does not need to be restarted after copying the files.
See USAGE.md in this package for SQL examples and usage details.

New installations use PostRCS 1.2.1. Existing PostRCS 1.0, 1.1, and 1.2
installations can be upgraded with:

  ALTER EXTENSION postrcs UPDATE;

The extension supports text content, trunk revisions, branches created from
trunk revisions, symbolic tags, multi-hunk diffs, and three-way merge. Nested
branches and binary content are not supported. Non-empty file contents must end
in a newline. Archives are limited to 16 MiB, check-in content to 8 MiB, log
messages to 1 MiB, and diff/merge comparisons to 4 million line pairs.
