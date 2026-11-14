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
