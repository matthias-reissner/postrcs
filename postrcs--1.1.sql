CREATE FUNCTION rcs_is_valid(text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'rcs_is_valid'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE DOMAIN rcs_archive AS text
CHECK (rcs_is_valid(VALUE));

CREATE FUNCTION rcs_checkout(rcs_archive, text)
RETURNS text
AS 'MODULE_PATHNAME', 'rcs_checkout'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION rcs_checkin(rcs_archive, text, text)
RETURNS rcs_archive
AS 'MODULE_PATHNAME', 'rcs_checkin'
LANGUAGE C VOLATILE STRICT PARALLEL UNSAFE;

CREATE FUNCTION rcs_diff(rcs_archive, text, text)
RETURNS text
AS 'MODULE_PATHNAME', 'rcs_diff'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION rcs_revisions(
    archive rcs_archive,
    OUT revision text,
    OUT author text,
    OUT revision_date text,
    OUT state text,
    OUT log text
)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'rcs_revisions'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION rcs_head(rcs_archive)
RETURNS text
AS 'MODULE_PATHNAME', 'rcs_head'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION rcs_resolve_revision(rcs_archive, text)
RETURNS text
AS 'MODULE_PATHNAME', 'rcs_resolve_revision'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION rcs_create_branch(rcs_archive, text, text)
RETURNS rcs_archive
AS 'MODULE_PATHNAME', 'rcs_create_branch'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION rcs_branch_checkin(rcs_archive, text, text, text)
RETURNS rcs_archive
AS 'MODULE_PATHNAME', 'rcs_branch_checkin'
LANGUAGE C VOLATILE STRICT PARALLEL UNSAFE;

CREATE FUNCTION rcs_tag(rcs_archive, text, text)
RETURNS rcs_archive
AS 'MODULE_PATHNAME', 'rcs_tag'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION rcs_symbols(
    archive rcs_archive,
    OUT name text,
    OUT revision text,
    OUT is_branch boolean
)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'rcs_symbols'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
