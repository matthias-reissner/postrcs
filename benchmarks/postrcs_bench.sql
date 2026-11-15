\set ON_ERROR_STOP on
\timing on

CREATE EXTENSION IF NOT EXISTS postrcs;

CREATE TEMP TABLE postrcs_benchmark (
    archive rcs_archive NOT NULL
);

INSERT INTO postrcs_benchmark (archive)
SELECT rcs_checkin(
    ''::rcs_archive,
    string_agg('line-' || line_number::text || E'\n', '' ORDER BY line_number),
    '1000-line baseline'
)
FROM generate_series(1, 1000) AS lines(line_number);

UPDATE postrcs_benchmark
SET archive = rcs_checkin(
    archive,
    (
        SELECT string_agg(
            CASE
                WHEN line_number IN (200, 500, 800)
                    THEN 'changed-' || line_number::text
                ELSE 'line-' || line_number::text
            END || E'\n',
            '' ORDER BY line_number
        )
        FROM generate_series(1, 1000) AS lines(line_number)
    ),
    'three separated edits'
);

SELECT pg_column_size(archive) AS archive_bytes,
       length(rcs_checkout(archive, '1.1')) AS base_content_bytes,
       length(rcs_diff(archive, '1.1', '1.2')) AS diff_bytes
FROM postrcs_benchmark;

\echo 'Near-limit diff and merge workload (1,900 lines per revision)'

CREATE TEMP TABLE postrcs_merge_benchmark (
    base_content text NOT NULL,
    archive rcs_archive NOT NULL
);

INSERT INTO postrcs_merge_benchmark (base_content, archive)
SELECT content, rcs_checkin(''::rcs_archive, content, '1,900-line base')
FROM (
    SELECT string_agg('line-' || line_number::text || E'\n', ''
                      ORDER BY line_number) AS content
    FROM generate_series(1, 1900) AS lines(line_number)
) AS generated;

UPDATE postrcs_merge_benchmark
SET archive = rcs_create_branch(archive, 'benchmark_ours', '1.1');

UPDATE postrcs_merge_benchmark
SET archive = rcs_create_branch(archive, 'benchmark_theirs', '1.1');

UPDATE postrcs_merge_benchmark
SET archive = rcs_branch_checkin(
    archive,
    'benchmark_ours',
    replace(base_content, E'line-500\n', E'ours-500\n'),
    'change line 500'
);

UPDATE postrcs_merge_benchmark
SET archive = rcs_branch_checkin(
    archive,
    'benchmark_theirs',
    replace(base_content, E'line-1400\n', E'theirs-1400\n'),
    'change line 1400'
);

SELECT pg_column_size(archive) AS archive_bytes,
       length(rcs_diff(archive, 'benchmark_ours', 'benchmark_theirs'))
           AS branch_diff_bytes
FROM postrcs_merge_benchmark;

WITH merged AS (
    SELECT rcs_merge(
        archive, '1.1', 'benchmark_ours', 'benchmark_theirs'
    ) AS result
    FROM postrcs_merge_benchmark
)
SELECT (result).has_conflicts AS has_conflicts,
       length((result).content) AS merged_content_bytes
FROM merged;
