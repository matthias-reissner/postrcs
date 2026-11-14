CREATE FUNCTION rcs_import(text)
RETURNS rcs_archive
LANGUAGE SQL IMMUTABLE STRICT PARALLEL SAFE
AS $$ SELECT $1::rcs_archive $$;

CREATE FUNCTION rcs_export(rcs_archive)
RETURNS text
LANGUAGE SQL IMMUTABLE STRICT PARALLEL SAFE
AS $$ SELECT $1::text $$;

CREATE TYPE rcs_merge_result AS (
    content text,
    has_conflicts boolean
);

CREATE FUNCTION rcs_merge(rcs_archive, text, text, text)
RETURNS rcs_merge_result
AS 'MODULE_PATHNAME', 'rcs_merge'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
