CREATE EXTENSION postrcs;

DO $$
DECLARE
    arch rcs_archive;
    revision_count integer;
    revision_log text;
BEGIN
    arch := rcs_checkin(''::rcs_archive, E'first @ line\n', 'initial');
    IF rcs_checkout(arch, '1.1') <> E'first @ line\n' THEN
        RAISE EXCEPTION 'initial checkout did not round-trip';
    END IF;

    arch := rcs_checkin(arch, E'first @ line\nsecond line\n', 'second');
    IF NOT rcs_is_valid(arch::text) THEN
        RAISE EXCEPTION 'check-in produced an invalid archive';
    END IF;
    IF rcs_checkout(arch, '1.1') <> E'first @ line\n' OR
       rcs_checkout(arch, '1.2') <> E'first @ line\nsecond line\n' THEN
        RAISE EXCEPTION 'trunk checkout did not round-trip';
    END IF;

    SELECT count(*), max(log)
      INTO revision_count, revision_log
      FROM rcs_revisions(arch)
     WHERE revision = '1.2';
    IF revision_count <> 1 OR revision_log <> 'second' THEN
        RAISE EXCEPTION 'revision metadata did not round-trip';
    END IF;

    IF rcs_diff(arch, '1.1', '1.2') <>
       E'--- 1.1\n+++ 1.2\n@@ -1,1 +1,2 @@\n-first @ line\n+first @ line\n+second line\n' THEN
        RAISE EXCEPTION 'unexpected unified diff';
    END IF;

    IF rcs_is_valid('not an RCS archive') THEN
        RAISE EXCEPTION 'malformed archive was accepted';
    END IF;
END
$$;
