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
    arch rcs_archive;
    base text := E'alpha\nbeta\ngamma\ndelta\nepsilon\nzeta\neta\n';
    changed text := E'ALPHA\nbeta\ngamma\nDELTA\nepsilon\nzeta\nETA\n';
    difference text;
BEGIN
    arch := rcs_checkin(''::rcs_archive, base, 'base');
    arch := rcs_checkin(arch, changed, 'three separated edits');
    difference := rcs_diff(arch, '1.1', '1.2');
    IF difference <> E'--- 1.1\n+++ 1.2\n@@ -1,1 +1,1 @@\n-alpha\n+ALPHA\n@@ -4,1 +4,1 @@\n-delta\n+DELTA\n@@ -7,1 +7,1 @@\n-eta\n+ETA\n' THEN
        RAISE EXCEPTION 'diff did not preserve separate change regions';
    END IF;
END
$$;

DO $$
DECLARE
    arch rcs_archive;
    merge_result rcs_merge_result;
    base text := E'alpha\nbeta\ngamma\n';
BEGIN
    arch := rcs_checkin(''::rcs_archive, base, 'merge base');
    arch := rcs_create_branch(arch, 'ours', '1.1');
    arch := rcs_create_branch(arch, 'theirs', '1.1');
    arch := rcs_branch_checkin(
        arch, 'ours', E'ours\nbeta\ngamma\n', 'change first line');
    arch := rcs_branch_checkin(
        arch, 'theirs', E'alpha\nbeta\ntheirs\n', 'change last line');
    merge_result := rcs_merge(arch, '1.1', 'ours', 'theirs');
    IF merge_result.has_conflicts OR
       merge_result.content <> E'ours\nbeta\ntheirs\n' THEN
        RAISE EXCEPTION 'unexpected merge result (conflicts %, content %)',
            merge_result.has_conflicts, merge_result.content;
    END IF;

    arch := rcs_create_branch(arch, 'same', '1.1');
    arch := rcs_branch_checkin(
        arch, 'same', E'ours\nbeta\ngamma\n', 'same change');
    merge_result := rcs_merge(arch, '1.1', 'ours', 'same');
    IF merge_result.has_conflicts OR
       merge_result.content <> E'ours\nbeta\ngamma\n' THEN
        RAISE EXCEPTION 'identical changes were not coalesced';
    END IF;

    arch := rcs_create_branch(arch, 'conflict', '1.1');
    arch := rcs_branch_checkin(
        arch, 'conflict', E'conflict\nbeta\ngamma\n', 'change first line');
    merge_result := rcs_merge(arch, '1.1', 'ours', 'conflict');
    IF NOT merge_result.has_conflicts OR
       merge_result.content <>
           E'<<<<<<< ours\nours\n=======\nconflict\n>>>>>>> theirs\nbeta\ngamma\n' THEN
        RAISE EXCEPTION 'overlapping changes did not produce conflict markers';
    END IF;
END
$$;

DO $$
DECLARE
    fixture text := replace(
        replace($RCS$head 1.2;
access;
symbols release_1_2:1.2;
locks; strict;
comment @# @;
expand @kv@;

1.2
date 2026.11.14.17.00.00; author demo; state Exp;
branches;
next 1.1;

1.1
date 2026.11.14.16.00.00; author demo; state Exp;
branches;
next ;

desc
@standard RCS archive fixture@

1.2
log
@second revision@
text
@first line
second line
@
1.1
log
@first revision@
text
@d2 1
@
$RCS$, E'\r\n', E'\n'),
        E'\n', E'\r\n');
    imported rcs_archive;
BEGIN
    imported := rcs_import(fixture);
    IF rcs_checkout(imported, '1.1') <> E'first line\r\n' OR
       rcs_checkout(imported, 'release_1_2') <>
           E'first line\r\nsecond line\r\n' OR
       rcs_export(imported) <> fixture THEN
        RAISE EXCEPTION 'RCS import/export compatibility failed';
    END IF;
END
$$;

DO $$
DECLARE
    arch rcs_archive;
    large_base text;
BEGIN
    SELECT string_agg('line-' || n::text, E'\n' ORDER BY n) || E'\n'
      INTO large_base
      FROM generate_series(1, 2048) AS s(n);
    arch := rcs_checkin(''::rcs_archive, large_base, 'large base');
    arch := rcs_checkin(
        arch, replace(large_base, 'line-1000', 'changed'), 'changed line');
    BEGIN
        PERFORM rcs_diff(arch, '1.1', '1.2');
        RAISE EXCEPTION 'oversized diff workload was accepted';
    EXCEPTION WHEN program_limit_exceeded THEN
        NULL;
    END;

    BEGIN
        PERFORM rcs_checkin(
            ''::rcs_archive, repeat('x', 8 * 1024 * 1024 + 1), 'too large');
        RAISE EXCEPTION 'oversized content was accepted';
    EXCEPTION WHEN program_limit_exceeded THEN
        NULL;
    END;

    IF rcs_is_valid(repeat('x', 16 * 1024 * 1024 + 1)) THEN
        RAISE EXCEPTION 'oversized archive was accepted';
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
