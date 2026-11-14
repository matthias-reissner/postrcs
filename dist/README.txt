postrcs 1.0 - Windows x64 package for PostgreSQL 18

This package was built and tested with PostgreSQL 18.7 on Windows x64.
Use it with PostgreSQL 18 on Windows x64. Other PostgreSQL major versions,
operating systems, and CPU architectures need their own build.

Install the files from an elevated PowerShell or Command Prompt. Set PGROOT
to your PostgreSQL installation directory, then copy:

  lib\postrcs.dll
      to %PGROOT%\lib\

  share\extension\postrcs.control
  share\extension\postrcs--1.0.sql
      to %PGROOT%\share\extension\

For example, with PGROOT=C:\Users\Matthias\PostgreSQL\18:

  Copy-Item .\lib\postrcs.dll "$PGROOT\lib\"
  Copy-Item .\share\extension\postrcs.control "$PGROOT\share\extension\"
  Copy-Item .\share\extension\postrcs--1.0.sql "$PGROOT\share\extension\"

Then connect to the target database as a database administrator and run:

  CREATE EXTENSION postrcs;

The extension must be created separately in each database where it is used.
The PostgreSQL server does not need to be restarted after copying the files.

Current limitations: check-in and checkout support trunk revisions and text
content only. Non-empty file contents must end in a newline. Diffs include
the whole file rather than only changed lines.
