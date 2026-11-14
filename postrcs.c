#include "postgres.h"

#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "access/htup_details.h"
#include "fmgr.h"
#include "funcapi.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/elog.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"
#include "utils/tuplestore.h"

PG_MODULE_MAGIC;

typedef struct RcsToken
{
    char *value;
    Size start;
    Size end;
    bool quoted;
} RcsToken;

typedef struct RcsRevision
{
    char *number;
    char *date;
    char *author;
    char *state;
    char *log;
    char *text;
    Size text_start;
    Size text_end;
} RcsRevision;

typedef struct RcsArchive
{
    char *source;
    Size length;
    RcsToken *tokens;
    int token_count;
    RcsRevision *revisions;
    int revision_count;
    char *description;
    int description_token;
} RcsArchive;

typedef struct RcsLines
{
    char **items;
    int count;
    int capacity;
} RcsLines;

static void
append_line(RcsLines *lines, char *line)
{
    if (lines->count == lines->capacity)
    {
        lines->capacity = lines->capacity ? lines->capacity * 2 : 16;
        lines->items = lines->items
            ? repalloc(lines->items, sizeof(char *) * lines->capacity)
            : palloc(sizeof(char *) * lines->capacity);
    }
    lines->items[lines->count++] = line;
}

static void
token_append(RcsArchive *archive, int *capacity, char *value, Size start,
             Size end, bool quoted)
{
    if (archive->token_count == *capacity)
    {
        *capacity = *capacity ? *capacity * 2 : 64;
        archive->tokens = archive->tokens
            ? repalloc(archive->tokens, sizeof(RcsToken) * *capacity)
            : palloc(sizeof(RcsToken) * *capacity);
    }
    archive->tokens[archive->token_count].value = value;
    archive->tokens[archive->token_count].start = start;
    archive->tokens[archive->token_count].end = end;
    archive->tokens[archive->token_count].quoted = quoted;
    archive->token_count++;
}

static void
tokenize(RcsArchive *archive)
{
    Size pos = 0;
    int capacity = 0;

    while (pos < archive->length)
    {
        Size start;
        StringInfoData value;

        while (pos < archive->length &&
               isspace((unsigned char) archive->source[pos]))
            pos++;
        if (pos == archive->length)
            break;

        start = pos;
        if (archive->source[pos] == ';')
        {
            token_append(archive, &capacity, pstrdup(";"), pos, pos + 1,
                         false);
            pos++;
            continue;
        }

        if (archive->source[pos] == '@')
        {
            bool closed = false;

            initStringInfo(&value);
            pos++;
            while (pos < archive->length)
            {
                if (archive->source[pos] == '@')
                {
                    if (pos + 1 < archive->length &&
                        archive->source[pos + 1] == '@')
                    {
                        appendStringInfoChar(&value, '@');
                        pos += 2;
                        continue;
                    }
                    pos++;
                    closed = true;
                    break;
                }
                appendStringInfoChar(&value, archive->source[pos++]);
            }
            if (!closed)
                ereport(ERROR,
                        (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                         errmsg("unterminated RCS string")));
            token_append(archive, &capacity, value.data, start, pos, true);
            continue;
        }

        while (pos < archive->length &&
               !isspace((unsigned char) archive->source[pos]) &&
               archive->source[pos] != ';' && archive->source[pos] != '@')
            pos++;
        if (pos == start)
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                     errmsg("unexpected character in RCS archive")));
        token_append(archive, &capacity,
                     pnstrdup(archive->source + start, pos - start),
                     start, pos, false);
    }
}

static bool
token_is(RcsArchive *archive, int index, const char *value)
{
    return index >= 0 && index < archive->token_count &&
        !archive->tokens[index].quoted &&
        strcmp(archive->tokens[index].value, value) == 0;
}

static bool
valid_revision_number(const char *revision)
{
    bool digit = false;
    const unsigned char *p;

    for (p = (const unsigned char *) revision; *p; p++)
    {
        if (isdigit(*p))
            digit = true;
        else if (*p == '.' && digit)
            digit = false;
        else
            return false;
    }
    return digit;
}

static void
require_token(RcsArchive *archive, int *index, const char *value)
{
    if (!token_is(archive, *index, value))
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                 errmsg("invalid RCS archive: expected \"%s\"", value)));
    (*index)++;
}

static char *
take_value(RcsArchive *archive, int *index, bool must_be_quoted)
{
    RcsToken *token;

    if (*index >= archive->token_count)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                 errmsg("unexpected end of RCS archive")));
    token = &archive->tokens[*index];
    if (must_be_quoted && !token->quoted)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                 errmsg("invalid RCS archive: expected a quoted string")));
    if (!must_be_quoted && token->quoted)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                 errmsg("invalid RCS archive: expected a token")));
    (*index)++;
    return token->value;
}

static RcsArchive *
parse_archive(char *source, Size length)
{
    RcsArchive *archive = palloc0(sizeof(RcsArchive));
    int index = 0;
    int capacity = 0;

    archive->source = source;
    archive->length = length;
    tokenize(archive);
    if (archive->token_count == 0)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                 errmsg("empty RCS archive")));

    while (index + 1 < archive->token_count &&
           !token_is(archive, index, "desc"))
    {
        RcsRevision revision;

        if (!token_is(archive, index + 1, "date"))
        {
            index++;
            continue;
        }
        if (archive->tokens[index].quoted ||
            !valid_revision_number(archive->tokens[index].value))
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                     errmsg("invalid RCS revision number")));
        memset(&revision, 0, sizeof(revision));
        revision.number = take_value(archive, &index, false);
        require_token(archive, &index, "date");
        revision.date = take_value(archive, &index, false);
        require_token(archive, &index, ";");
        require_token(archive, &index, "author");
        revision.author = take_value(archive, &index, false);
        require_token(archive, &index, ";");
        require_token(archive, &index, "state");
        revision.state = token_is(archive, index, ";")
            ? pstrdup("") : take_value(archive, &index, false);
        require_token(archive, &index, ";");
        require_token(archive, &index, "branches");
        while (index < archive->token_count &&
               !token_is(archive, index, ";"))
            index++;
        require_token(archive, &index, ";");
        require_token(archive, &index, "next");
        if (!token_is(archive, index, ";"))
            (void) take_value(archive, &index, false);
        require_token(archive, &index, ";");

        if (archive->revision_count == capacity)
        {
            capacity = capacity ? capacity * 2 : 16;
            archive->revisions = archive->revisions
                ? repalloc(archive->revisions,
                           sizeof(RcsRevision) * capacity)
                : palloc(sizeof(RcsRevision) * capacity);
        }
        archive->revisions[archive->revision_count++] = revision;
    }

    if (index >= archive->token_count)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                 errmsg("invalid RCS archive: missing description")));
    archive->description_token = index;
    index++;
    archive->description = take_value(archive, &index, true);

    while (index < archive->token_count)
    {
        char *number = take_value(archive, &index, false);
        RcsRevision *revision = NULL;
        int i;

        for (i = 0; i < archive->revision_count; i++)
            if (strcmp(archive->revisions[i].number, number) == 0)
            {
                revision = &archive->revisions[i];
                break;
            }
        if (revision == NULL || revision->text != NULL)
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                     errmsg("invalid or duplicate RCS delta \"%s\"", number)));
        require_token(archive, &index, "log");
        revision->log = take_value(archive, &index, true);
        require_token(archive, &index, "text");
        if (index >= archive->token_count ||
            !archive->tokens[index].quoted)
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                     errmsg("invalid RCS archive: expected delta text")));
        revision->text = archive->tokens[index].value;
        revision->text_start = archive->tokens[index].start;
        revision->text_end = archive->tokens[index].end;
        index++;
    }

    if (archive->revision_count == 0)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                 errmsg("RCS archive contains no revisions")));
    for (index = 0; index < archive->revision_count; index++)
        if (archive->revisions[index].text == NULL)
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                     errmsg("RCS revision \"%s\" has no delta text",
                            archive->revisions[index].number)));
    return archive;
}

static RcsRevision *
find_revision(RcsArchive *archive, const char *number)
{
    int i;

    for (i = 0; i < archive->revision_count; i++)
        if (strcmp(archive->revisions[i].number, number) == 0)
            return &archive->revisions[i];
    return NULL;
}

static RcsRevision *
find_head(RcsArchive *archive)
{
    int i;
    int head_index = -1;

    for (i = 0; i < archive->token_count - 1; i++)
        if (token_is(archive, i, "head"))
        {
            int j;
            for (j = i + 1; j < archive->token_count &&
                 !token_is(archive, j, ";"); j++)
                if (strchr(archive->tokens[j].value, '.') != NULL)
                {
                    head_index = j;
                    break;
                }
            break;
        }
    if (head_index < 0)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                 errmsg("RCS archive has no head revision")));
    {
        RcsRevision *head =
            find_revision(archive, archive->tokens[head_index].value);

        if (head == NULL)
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                     errmsg("RCS head references a missing revision")));
        return head;
    }
}

static bool
is_trunk_revision(const char *revision)
{
    int dots = 0;
    const char *p;

    for (p = revision; *p; p++)
        if (*p == '.')
            dots++;
    return dots == 1;
}

static int
split_lines(const char *text, RcsLines *lines)
{
    const char *start = text;
    const char *p = text;

    while (*p)
    {
        if (*p++ == '\n')
        {
            append_line(lines, pnstrdup(start, p - start));
            start = p;
        }
    }
    if (p != start)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("RCS text must end with a newline")));
    return lines->count;
}

static char *
join_lines(RcsLines *lines)
{
    StringInfoData result;
    int i;

    initStringInfo(&result);
    for (i = 0; i < lines->count; i++)
        appendStringInfoString(&result, lines->items[i]);
    return result.data;
}

static char *
apply_script(const char *base, const char *script)
{
    RcsLines lines = {NULL, 0, 0};
    const char *p = script;

    split_lines(base, &lines);
    while (*p)
    {
        char operation;
        long line_number;
        long count;
        char *end;
        long i;

        operation = *p++;
        if (operation != 'a' && operation != 'd')
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                     errmsg("invalid RCS edit script")));
        line_number = strtol(p, &end, 10);
        if (end == p || *end != ' ')
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                     errmsg("invalid RCS edit script command")));
        p = end + 1;
        count = strtol(p, &end, 10);
        if (end == p || *end != '\n' || count <= 0)
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                     errmsg("invalid RCS edit script command")));
        p = end + 1;
        if (operation == 'd')
        {
            if (line_number < 1 || line_number > lines.count ||
                count > lines.count - (line_number - 1))
                ereport(ERROR,
                        (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                         errmsg("RCS delete command is out of range")));
            memmove(&lines.items[line_number - 1],
                    &lines.items[line_number - 1 + count],
                    sizeof(char *) * (lines.count - (line_number - 1 + count)));
            lines.count -= count;
        }
        else
        {
            int insertion = (int) line_number;
            char **added;

            if (line_number < 0 || line_number > lines.count)
                ereport(ERROR,
                        (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                         errmsg("RCS add command is out of range")));
            if (count > MaxAllocSize / sizeof(char *))
                ereport(ERROR,
                        (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                         errmsg("RCS edit script is too large")));
            if (count > INT_MAX - lines.count)
                ereport(ERROR,
                        (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                         errmsg("RCS edit script is too large")));
            added = count ? palloc(sizeof(char *) * count) : NULL;
            for (i = 0; i < count; i++)
            {
                const char *line_start = p;
                while (*p && *p != '\n')
                    p++;
                if (!*p)
                    ereport(ERROR,
                            (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                             errmsg("truncated RCS add command")));
                p++;
                added[i] = pnstrdup(line_start, p - line_start);
            }
            if (lines.count + count > lines.capacity)
            {
                int new_capacity = lines.capacity ? lines.capacity : 16;
                while (new_capacity < lines.count + count)
                    new_capacity *= 2;
                lines.items = lines.items
                    ? repalloc(lines.items, sizeof(char *) * new_capacity)
                    : palloc(sizeof(char *) * new_capacity);
                lines.capacity = new_capacity;
            }
            memmove(&lines.items[insertion + count], &lines.items[insertion],
                    sizeof(char *) * (lines.count - insertion));
            if (count)
                memcpy(&lines.items[insertion], added,
                       sizeof(char *) * count);
            lines.count += count;
        }
    }
    return join_lines(&lines);
}

static char *
checkout_internal(RcsArchive *archive, const char *revision)
{
    RcsRevision *head = find_head(archive);
    RcsRevision *current;
    char *contents;
    int steps = 0;

    if (!is_trunk_revision(revision))
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("checkout currently supports trunk revisions only")));
    current = head;
    contents = current->text;
    while (strcmp(current->number, revision) != 0)
    {
        int i;
        char *next_number = NULL;

        if (++steps > archive->revision_count)
            break;
        for (i = 0; i < archive->token_count - 1; i++)
            if (token_is(archive, i, current->number) &&
                token_is(archive, i + 1, "date"))
            {
                int j;
                for (j = i + 1; j < archive->token_count &&
                     !token_is(archive, j, "next"); j++)
                    ;
                if (j + 1 < archive->token_count &&
                    !token_is(archive, j + 1, ";"))
                    next_number = archive->tokens[j + 1].value;
                break;
            }
        if (next_number == NULL)
            break;
        current = find_revision(archive, next_number);
        if (current == NULL)
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                     errmsg("RCS trunk references a missing revision")));
        contents = apply_script(contents, current->text);
    }
    if (strcmp(current->number, revision) != 0)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("revision \"%s\" was not found", revision)));
    return contents;
}

static char *
quote_rcs(const char *value)
{
    StringInfoData result;
    const char *p;

    initStringInfo(&result);
    appendStringInfoChar(&result, '@');
    for (p = value; *p; p++)
    {
        appendStringInfoChar(&result, *p);
        if (*p == '@')
            appendStringInfoChar(&result, '@');
    }
    appendStringInfoChar(&result, '@');
    return result.data;
}

static void
append_edit_script(StringInfo result, const char *old_contents,
                   const char *new_contents)
{
    RcsLines old_lines = {NULL, 0, 0};
    RcsLines new_lines = {NULL, 0, 0};
    int i;

    split_lines(old_contents, &old_lines);
    split_lines(new_contents, &new_lines);
    if (new_lines.count)
        appendStringInfo(result, "d1 %d\n", new_lines.count);
    if (old_lines.count)
    {
        appendStringInfo(result, "a0 %d\n", old_lines.count);
        for (i = 0; i < old_lines.count; i++)
            appendStringInfoString(result, old_lines.items[i]);
    }
}

static char *
format_rcs_date(void)
{
    TimestampTz now = GetCurrentTimestamp();
    struct pg_tm tm;
    fsec_t fsec;
    StringInfoData result;

    if (timestamp2tm(now, NULL, &tm, &fsec, NULL, NULL) != 0)
        ereport(ERROR,
                (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                 errmsg("could not convert current time to RCS date")));
    initStringInfo(&result);
    appendStringInfo(&result, "%04d.%02d.%02d.%02d.%02d.%02d",
                     tm.tm_year, tm.tm_mon, tm.tm_mday, tm.tm_hour,
                     tm.tm_min, tm.tm_sec);
    return result.data;
}

static void
append_quoted(StringInfo result, const char *value)
{
    char *quoted = quote_rcs(value);
    appendStringInfoString(result, quoted);
}

PG_FUNCTION_INFO_V1(rcs_is_valid);
Datum
rcs_is_valid(PG_FUNCTION_ARGS)
{
    text *input = PG_GETARG_TEXT_PP(0);
    char *source = text_to_cstring(input);
    bool valid = true;

    if (VARSIZE_ANY_EXHDR(input) == 0)
        PG_RETURN_BOOL(true);

    PG_TRY();
    {
        (void) parse_archive(source, strlen(source));
    }
    PG_CATCH();
    {
        ErrorData *error = CopyErrorData();

        FlushErrorState();
        if (error->sqlerrcode != ERRCODE_INVALID_TEXT_REPRESENTATION)
            ReThrowError(error);
        FreeErrorData(error);
        valid = false;
    }
    PG_END_TRY();
    PG_RETURN_BOOL(valid);
}

PG_FUNCTION_INFO_V1(rcs_checkout);
Datum
rcs_checkout(PG_FUNCTION_ARGS)
{
    char *source = text_to_cstring(PG_GETARG_TEXT_PP(0));
    char *revision = text_to_cstring(PG_GETARG_TEXT_PP(1));
    RcsArchive *archive = parse_archive(source, strlen(source));

    PG_RETURN_TEXT_P(cstring_to_text(checkout_internal(archive, revision)));
}

PG_FUNCTION_INFO_V1(rcs_checkin);
Datum
rcs_checkin(PG_FUNCTION_ARGS)
{
    text *archive_arg = PG_GETARG_TEXT_PP(0);
    char *source = text_to_cstring(archive_arg);
    char *contents = text_to_cstring(PG_GETARG_TEXT_PP(1));
    char *log = text_to_cstring(PG_GETARG_TEXT_PP(2));
    char *author = GetUserNameFromId(GetUserId(), false);
    char *date = format_rcs_date();
    StringInfoData output;
    char *new_number;
    const unsigned char *author_char;
    RcsArchive *archive = NULL;
    RcsRevision *head = NULL;

    for (author_char = (const unsigned char *) author; *author_char;
         author_char++)
        if (isspace(*author_char) || *author_char == ';' || *author_char == '@')
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("database role name cannot be represented in an RCS author field")));

    if (VARSIZE_ANY_EXHDR(archive_arg) > 0)
    {
        archive = parse_archive(source, strlen(source));
        head = find_head(archive);
        if (!is_trunk_revision(head->number))
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("check-in currently supports trunk archives only")));
        if (!valid_revision_number(head->number) ||
            !strchr(head->number, '.'))
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                     errmsg("invalid RCS head revision")));
        {
            char *dot = strchr(head->number, '.');
            long minor;
            char *end;
            minor = strtol(dot + 1, &end, 10);
            if (*end != '\0' || minor <= 0 || minor >= INT_MAX)
                ereport(ERROR,
                        (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                         errmsg("invalid RCS trunk head revision")));
            new_number = psprintf("%.*s.%ld", (int) (dot - head->number),
                                  head->number, minor + 1);
        }
        (void) checkout_internal(archive, head->number);
    }
    else
        new_number = pstrdup("1.1");

    if (contents[0] != '\0' && contents[strlen(contents) - 1] != '\n')
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("RCS text must end with a newline")));

    initStringInfo(&output);
    if (archive == NULL)
    {
        appendStringInfoString(&output, "head 1.1;\naccess;\nsymbols;\n");
        appendStringInfoString(&output, "locks; strict;\ncomment @# @;\n\n");
        appendStringInfoString(&output, "1.1\ndate ");
        appendStringInfo(&output, "%s; author %s; state Exp;\n",
                         date, author);
        appendStringInfoString(&output, "branches;\nnext ;\n\ndesc\n@@\n\n");
        appendStringInfoString(&output, "1.1\nlog\n");
        append_quoted(&output, log);
        appendStringInfoString(&output, "\ntext\n");
        append_quoted(&output, contents);
        appendStringInfoChar(&output, '\n');
    }
    else
    {
        Size desc_start =
            archive->tokens[archive->description_token].start;
        Size old_text_start = head->text_start;
        Size old_text_end = head->text_end;
        StringInfoData reverse_script;
        Size head_value_start = 0;
        Size head_value_end = 0;
        int token_index;
        char *previous_contents =
            checkout_internal(archive, head->number);

        initStringInfo(&reverse_script);
        append_edit_script(&reverse_script, previous_contents, contents);
        for (token_index = 0; token_index + 1 < archive->token_count;
             token_index++)
            if (token_is(archive, token_index, "head"))
            {
                head_value_start = archive->tokens[token_index + 1].start;
                head_value_end = archive->tokens[token_index + 1].end;
                break;
            }
        if (head_value_end == 0 || head_value_end > desc_start)
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                     errmsg("RCS archive has no valid head field")));
        appendBinaryStringInfo(&output, source, head_value_start);
        appendStringInfoString(&output, new_number);
        appendBinaryStringInfo(&output, source + head_value_end,
                               desc_start - head_value_end);
        appendStringInfo(&output, "%s\ndate %s; author %s; state Exp;\n"
                         "branches;\nnext %s;\n\n",
                         new_number, date, author, head->number);
        appendBinaryStringInfo(&output, source + desc_start,
                               old_text_start - desc_start);
        append_quoted(&output, reverse_script.data);
        appendBinaryStringInfo(&output, source + old_text_end,
                               strlen(source) - old_text_end);
        appendStringInfo(&output, "\n%s\nlog\n", new_number);
        append_quoted(&output, log);
        appendStringInfoString(&output, "\ntext\n");
        append_quoted(&output, contents);
        appendStringInfoChar(&output, '\n');
    }
    PG_RETURN_TEXT_P(cstring_to_text(output.data));
}

PG_FUNCTION_INFO_V1(rcs_diff);
Datum
rcs_diff(PG_FUNCTION_ARGS)
{
    char *source = text_to_cstring(PG_GETARG_TEXT_PP(0));
    char *left_revision = text_to_cstring(PG_GETARG_TEXT_PP(1));
    char *right_revision = text_to_cstring(PG_GETARG_TEXT_PP(2));
    RcsArchive *archive = parse_archive(source, strlen(source));
    char *left = checkout_internal(archive, left_revision);
    char *right = checkout_internal(archive, right_revision);
    StringInfoData result;
    RcsLines left_lines = {NULL, 0, 0};
    RcsLines right_lines = {NULL, 0, 0};
    int i;

    initStringInfo(&result);
    if (strcmp(left, right) == 0)
        PG_RETURN_TEXT_P(cstring_to_text(""));

    split_lines(left, &left_lines);
    split_lines(right, &right_lines);
    appendStringInfo(&result, "--- %s\n+++ %s\n@@ -%d,%d +%d,%d @@\n",
                     left_revision, right_revision,
                     left_lines.count ? 1 : 0, left_lines.count,
                     right_lines.count ? 1 : 0, right_lines.count);
    for (i = 0; i < left_lines.count; i++)
    {
        appendStringInfoChar(&result, '-');
        appendStringInfoString(&result, left_lines.items[i]);
    }
    for (i = 0; i < right_lines.count; i++)
    {
        appendStringInfoChar(&result, '+');
        appendStringInfoString(&result, right_lines.items[i]);
    }
    PG_RETURN_TEXT_P(cstring_to_text(result.data));
}

PG_FUNCTION_INFO_V1(rcs_revisions);
Datum
rcs_revisions(PG_FUNCTION_ARGS)
{
    {
        ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
        MemoryContext oldcontext;
        TupleDesc tupledesc;
        Tuplestorestate *store;
        RcsArchive *archive;
        char *source;
        int i;

        if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo) ||
            !(rsinfo->allowedModes & SFRM_Materialize))
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("materialized set mode is required")));
        oldcontext = MemoryContextSwitchTo(
            rsinfo->econtext->ecxt_per_query_memory);
        source = text_to_cstring(PG_GETARG_TEXT_PP(0));
        archive = parse_archive(source, strlen(source));
        if (get_call_result_type(fcinfo, NULL, &tupledesc) != TYPEFUNC_COMPOSITE)
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("rcs_revisions must be called in a record context")));
        tupledesc = BlessTupleDesc(tupledesc);
        store = tuplestore_begin_heap(true, false, work_mem);
        for (i = 0; i < archive->revision_count; i++)
        {
            Datum values[5];
            bool nulls[5] = {false, false, false, false, false};
            HeapTuple tuple;
            RcsRevision *revision = &archive->revisions[i];

            values[0] = CStringGetTextDatum(revision->number);
            values[1] = CStringGetTextDatum(revision->author);
            values[2] = CStringGetTextDatum(revision->date);
            values[3] = CStringGetTextDatum(revision->state);
            values[4] = CStringGetTextDatum(revision->log);
            tuple = heap_form_tuple(tupledesc, values, nulls);
            tuplestore_puttuple(store, tuple);
            heap_freetuple(tuple);
        }
        rsinfo->returnMode = SFRM_Materialize;
        rsinfo->setResult = store;
        rsinfo->setDesc = tupledesc;
        MemoryContextSwitchTo(oldcontext);
        PG_RETURN_NULL();
    }
}
