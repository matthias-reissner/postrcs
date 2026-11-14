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
       E'--- 1.1\n+++ 1.2\n@@ -1,0 +2,1 @@\n+second line\n' THEN
        RAISE EXCEPTION 'unexpected unified diff';
    END IF;

    IF rcs_head(arch) <> '1.2' OR
       rcs_resolve_revision(arch, '1.1') <> '1.1' THEN
        RAISE EXCEPTION 'head or revision resolution failed';
    END IF;

    arch := rcs_tag(arch, 'stable', '1.2');
    arch := rcs_create_branch(arch, 'feature', 'stable');
    IF rcs_resolve_revision(arch, 'feature') <> '1.2.1' THEN
        RAISE EXCEPTION 'branch symbol did not resolve';
    END IF;

    arch := rcs_branch_checkin(
        arch, 'feature', E'first @ line\nbranch line\n', 'branch first');
    arch := rcs_branch_checkin(
        arch, 'feature', E'first @ line\nbranch line\ntail\n', 'branch second');
    IF rcs_checkout(arch, '1.2.1.1') <>
       E'first @ line\nbranch line\n' OR
       rcs_checkout(arch, 'feature') <>
       E'first @ line\nbranch line\ntail\n' OR
       rcs_checkout(arch, 'stable') <>
       E'first @ line\nsecond line\n' THEN
        RAISE EXCEPTION 'branch or tag checkout failed';
    END IF;
    arch := rcs_tag(arch, 'branch_release', 'feature');
    IF rcs_checkout(arch, 'branch_release') <>
       E'first @ line\nbranch line\ntail\n' THEN
        RAISE EXCEPTION 'branch revision tag did not check out';
    END IF;
    IF (SELECT count(*) FROM rcs_symbols(arch)
         WHERE (name, is_branch) IN (('feature', true), ('stable', false))) <> 2 THEN
        RAISE EXCEPTION 'RCS symbols were not listed correctly';
    END IF;
    IF rcs_head(arch) <> '1.2' THEN
        RAISE EXCEPTION 'branch check-in changed the trunk head';
    END IF;

    arch := rcs_checkin(arch, '', 'empty file');
    IF rcs_checkout(arch, '1.3') <> '' THEN
        RAISE EXCEPTION 'empty file did not round-trip';
    END IF;
    arch := rcs_checkin(arch, E'Grüße\n', 'unicode');
    IF rcs_checkout(arch, '1.4') <> E'Grüße\n' THEN
        RAISE EXCEPTION 'Unicode content did not round-trip';
    END IF;

    BEGIN
        PERFORM rcs_checkout(arch, '9.9');
        RAISE EXCEPTION 'missing revision was accepted';
    EXCEPTION WHEN invalid_parameter_value THEN
        NULL;
    END;

    BEGIN
        PERFORM rcs_checkin(arch, 'missing newline', 'invalid content');
        RAISE EXCEPTION 'content without a final newline was accepted';
    EXCEPTION WHEN feature_not_supported THEN
        NULL;
    END;

    IF rcs_is_valid('not an RCS archive') THEN
        RAISE EXCEPTION 'malformed archive was accepted';
    END IF;
    IF rcs_is_valid(E'head 1.2;\naccess;\nsymbols;\nlocks; strict;\ncomment @# @;\n\n1.2\ndate 2026.01.01.00.00.00; author demo; state Exp;\nbranches;\nnext 1.1;\n\n1.1\ndate 2026.01.01.00.00.00; author demo; state Exp;\nbranches;\nnext ;\n\ndesc\n@@\n\n1.2\nlog\n@head@\ntext\n@ok\n@\n1.1\nlog\n@old@\ntext\n@z1 1\n@') THEN
        RAISE EXCEPTION 'malformed RCS delta was accepted';
    END IF;
END
$$;

DO $$
DECLARE
    old_content text := repeat('same line' || E'\n', 1000);
    new_content text := repeat('same line' || E'\n', 500) ||
                        'changed' || E'\n' ||
                        repeat('same line' || E'\n', 500);
    arch rcs_archive;
    difference text;
BEGIN
    arch := rcs_checkin(''::rcs_archive, old_content, 'large base');
    arch := rcs_checkin(arch, new_content, 'one inserted line');
    IF rcs_checkout(arch, '1.1') <> old_content OR
       rcs_checkout(arch, '1.2') <> new_content THEN
        RAISE EXCEPTION 'large-file check-in did not round-trip';
    END IF;
    difference := rcs_diff(arch, '1.1', '1.2');
    IF difference <> E'--- 1.1\n+++ 1.2\n@@ -500,0 +501,1 @@\n+changed\n' THEN
        RAISE EXCEPTION 'diff did not isolate a small insertion';
    END IF;
END
$$;
