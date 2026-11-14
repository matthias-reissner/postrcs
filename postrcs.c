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

#define RCS_MAX_ARCHIVE_SIZE ((Size) 16 * 1024 * 1024)
#define RCS_MAX_CONTENT_SIZE ((Size) 8 * 1024 * 1024)
#define RCS_MAX_LOG_SIZE ((Size) 1024 * 1024)
#define RCS_MAX_DIFF_CELLS ((Size) 4 * 1024 * 1024)

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
    char *next;
    char *log;
    char *text;
    char **branches;
    int branch_count;
    Size branches_end;
    Size next_start;
    Size next_end;
    Size text_start;
    Size text_end;
} RcsRevision;

typedef struct RcsSymbol
{
    char *name;
    char *revision;
    Size start;
    Size end;
} RcsSymbol;

typedef struct RcsArchive
{
    char *source;
    Size length;
    RcsToken *tokens;
    int token_count;
    RcsRevision *revisions;
    int revision_count;
    RcsSymbol *symbols;
    int symbol_count;
    Size symbols_end;
    char *description;
    int description_token;
} RcsArchive;

typedef struct RcsEdit
{
    Size start;
    Size end;
    char *replacement;
} RcsEdit;

typedef struct RcsLines
{
    char **items;
    int count;
    int capacity;
} RcsLines;

typedef struct RcsDiffOp
{
    char kind;
    char *line;
    int old_line;
    int new_line;
} RcsDiffOp;

typedef struct RcsDiff
{
    RcsDiffOp *items;
    int count;
    int capacity;
} RcsDiff;

typedef struct RcsChange
{
    int start;
    int end;
    RcsLines replacement;
} RcsChange;

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

static bool valid_symbol_name(const char *name);

static void
parse_symbols(RcsArchive *archive)
{
    int i;
    int capacity = 0;

    for (i = 0; i < archive->token_count; i++)
        if (token_is(archive, i, "symbols"))
        {
            i++;
            while (i < archive->token_count &&
                   !token_is(archive, i, ";"))
            {
                char *entry = take_value(archive, &i, false);
                char *separator = strchr(entry, ':');
                RcsSymbol symbol;
                int j;

                if (separator == NULL || separator == entry ||
                    !valid_revision_number(separator + 1) ||
                    !valid_symbol_name(pnstrdup(entry, separator - entry)))
                    ereport(ERROR,
                            (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                             errmsg("invalid RCS symbolic name \"%s\"", entry)));
                symbol.name = pnstrdup(entry, separator - entry);
                symbol.revision = pstrdup(separator + 1);
                symbol.start = archive->tokens[i - 1].start;
                symbol.end = archive->tokens[i - 1].end;
                for (j = 0; j < archive->symbol_count; j++)
                    if (strcmp(archive->symbols[j].name, symbol.name) == 0)
                        ereport(ERROR,
                                (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                                 errmsg("duplicate RCS symbolic name \"%s\"",
                                        symbol.name)));
                if (archive->symbol_count == capacity)
                {
                    capacity = capacity ? capacity * 2 : 8;
                    archive->symbols = archive->symbols
                        ? repalloc(archive->symbols,
                                   sizeof(RcsSymbol) * capacity)
                        : palloc(sizeof(RcsSymbol) * capacity);
                }
                archive->symbols[archive->symbol_count++] = symbol;
            }
            if (i >= archive->token_count)
                ereport(ERROR,
                        (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                         errmsg("invalid RCS archive: unterminated symbols field")));
            archive->symbols_end = archive->tokens[i].start;
            return;
        }
}

static RcsArchive *
parse_archive(char *source, Size length)
{
    RcsArchive *archive = palloc0(sizeof(RcsArchive));
    int index = 0;
    int capacity = 0;

    if (length > RCS_MAX_ARCHIVE_SIZE)
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("RCS archive exceeds the 16 MiB size limit")));
    archive->source = source;
    archive->length = length;
    tokenize(archive);
    if (archive->token_count == 0)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                 errmsg("empty RCS archive")));
    parse_symbols(archive);

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
        {
            char *branch = take_value(archive, &index, false);
            int branch_capacity = revision.branch_count;

            if (!valid_revision_number(branch))
                ereport(ERROR,
                        (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                         errmsg("invalid RCS branch number \"%s\"", branch)));
            revision.branches = revision.branches
                ? repalloc(revision.branches,
                           sizeof(char *) * (branch_capacity + 1))
                : palloc(sizeof(char *));
            revision.branches[revision.branch_count++] = branch;
        }
        revision.branches_end = archive->tokens[index].start;
        require_token(archive, &index, ";");
        require_token(archive, &index, "next");
        if (!token_is(archive, index, ";"))
        {
            RcsToken *token = &archive->tokens[index];

            revision.next = take_value(archive, &index, false);
            revision.next_start = token->start;
            revision.next_end = token->end;
            if (!valid_revision_number(revision.next))
                ereport(ERROR,
                        (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                         errmsg("invalid RCS next revision \"%s\"",
                                revision.next)));
        }
        else
            revision.next = NULL;
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

static RcsSymbol *
find_symbol(RcsArchive *archive, const char *name)
{
    int i;

    for (i = 0; i < archive->symbol_count; i++)
        if (strcmp(archive->symbols[i].name, name) == 0)
            return &archive->symbols[i];
    return NULL;
}

static char *
resolve_revision_name(RcsArchive *archive, const char *name)
{
    RcsSymbol *symbol = find_symbol(archive, name);

    return symbol ? symbol->revision : (char *) name;
}

static bool
is_branch_number(const char *number)
{
    int dots = 0;
    const char *p;

    for (p = number; *p; p++)
        if (*p == '.')
            dots++;
    return dots == 2;
}

static RcsRevision *
find_branch_tip(RcsArchive *archive, const char *branch_number)
{
    RcsRevision *tip = NULL;
    Size prefix_length = strlen(branch_number);
    long tip_minor = -1;
    int i;

    for (i = 0; i < archive->revision_count; i++)
    {
        RcsRevision *revision = &archive->revisions[i];
        const char *minor_text;
        char *end;
        long minor;

        if (strncmp(revision->number, branch_number, prefix_length) != 0 ||
            revision->number[prefix_length] != '.')
            continue;
        minor_text = revision->number + prefix_length + 1;
        if (*minor_text == '\0' || strpbrk(minor_text, ".") != NULL)
            continue;
        minor = strtol(minor_text, &end, 10);
        if (*end != '\0' || minor <= tip_minor)
            continue;
        tip_minor = minor;
        tip = revision;
    }
    return tip;
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

static void
append_diff_op(RcsDiff *diff, char kind, char *line, int old_line,
               int new_line)
{
    if (diff->count == diff->capacity)
    {
        diff->capacity = diff->capacity ? diff->capacity * 2 : 32;
        diff->items = diff->items
            ? repalloc(diff->items, sizeof(RcsDiffOp) * diff->capacity)
            : palloc(sizeof(RcsDiffOp) * diff->capacity);
    }
    diff->items[diff->count].kind = kind;
    diff->items[diff->count].line = line;
    diff->items[diff->count].old_line = old_line;
    diff->items[diff->count].new_line = new_line;
    diff->count++;
}

static void
compute_diff_ops(RcsLines *old_lines, RcsLines *new_lines, RcsDiff *diff)
{
    Size rows = (Size) old_lines->count + 1;
    Size columns = (Size) new_lines->count + 1;
    Size cells;
    uint32 *lcs;
    int i;
    int j;

    if (rows > RCS_MAX_DIFF_CELLS / columns)
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("diff exceeds the 4 million line-pair limit"),
                 errhint("Use fewer lines or split the comparison into smaller revisions.")));
    cells = rows * columns;
    lcs = palloc(sizeof(uint32) * cells);
    memset(lcs, 0, sizeof(uint32) * cells);

    for (i = old_lines->count - 1; i >= 0; i--)
        for (j = new_lines->count - 1; j >= 0; j--)
        {
            Size position = (Size) i * columns + j;

            if (strcmp(old_lines->items[i], new_lines->items[j]) == 0)
                lcs[position] = lcs[(Size) (i + 1) * columns + j + 1] + 1;
            else
                lcs[position] = Max(lcs[(Size) (i + 1) * columns + j],
                                    lcs[(Size) i * columns + j + 1]);
        }

    i = 0;
    j = 0;
    while (i < old_lines->count || j < new_lines->count)
    {
        if (i < old_lines->count && j < new_lines->count &&
            strcmp(old_lines->items[i], new_lines->items[j]) == 0)
        {
            append_diff_op(diff, ' ', old_lines->items[i], i, j);
            i++;
            j++;
        }
        else if (i < old_lines->count &&
                 (j == new_lines->count ||
                  lcs[(Size) (i + 1) * columns + j] >=
                  lcs[(Size) i * columns + j + 1]))
        {
            append_diff_op(diff, '-', old_lines->items[i], i, j);
            i++;
        }
        else
        {
            append_diff_op(diff, '+', new_lines->items[j], i, j);
            j++;
        }
    }
}

static RcsChange *
diff_changes(RcsDiff *diff, int *change_count)
{
    RcsChange *changes = NULL;
    int capacity = 0;
    int i = 0;

    *change_count = 0;
    while (i < diff->count)
    {
        RcsChange change;

        if (diff->items[i].kind == ' ')
        {
            i++;
            continue;
        }
        memset(&change, 0, sizeof(change));
        change.start = diff->items[i].old_line;
        change.end = change.start;
        while (i < diff->count && diff->items[i].kind != ' ')
        {
            if (diff->items[i].kind == '-')
                change.end = diff->items[i].old_line + 1;
            else
                append_line(&change.replacement, diff->items[i].line);
            i++;
        }
        if (*change_count == capacity)
        {
            capacity = capacity ? capacity * 2 : 16;
            changes = changes
                ? repalloc(changes, sizeof(RcsChange) * capacity)
                : palloc(sizeof(RcsChange) * capacity);
        }
        changes[(*change_count)++] = change;
    }
    return changes;
}

static bool
change_overlaps_region(const RcsChange *change, int start, int end)
{
    if (change->start == change->end && start == end)
        return change->start == start;
    if (change->start == change->end)
        return change->start > start && change->start < end;
    if (start == end)
        return start > change->start && start < change->end;
    return change->start < end && start < change->end;
}

static void
append_base_lines(StringInfo output, RcsLines *base, int start, int end)
{
    int i;

    for (i = start; i < end; i++)
        appendStringInfoString(output, base->items[i]);
}

static void
append_change_lines(StringInfo output, const RcsChange *change)
{
    int i;

    for (i = 0; i < change->replacement.count; i++)
        appendStringInfoString(output, change->replacement.items[i]);
}

static char *
merge_variant(RcsLines *base, RcsChange *changes, int first, int end,
              int start, int finish)
{
    StringInfoData result;
    int position = start;
    int i;

    initStringInfo(&result);
    for (i = first; i < end; i++)
    {
        append_base_lines(&result, base, position, changes[i].start);
        append_change_lines(&result, &changes[i]);
        position = changes[i].end;
    }
    append_base_lines(&result, base, position, finish);
    return result.data;
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
        if (end == p ||
            (*end != '\n' && !(*end == '\r' && end[1] == '\n')) ||
            count <= 0)
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                     errmsg("invalid RCS edit script command")));
        if (*end == '\r')
            end++;
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
    char *resolved = resolve_revision_name(archive, revision);
    RcsRevision *head;
    RcsRevision *target;
    RcsRevision *current;
    char *contents;
    int steps = 0;

    if (is_branch_number(resolved))
    {
        head = find_branch_tip(archive, resolved);
        if (head == NULL)
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("branch \"%s\" has no revisions", revision)));
        resolved = head->number;
    }
    else if (is_trunk_revision(resolved))
        head = find_head(archive);
    else
    {
        char *branch_number = pstrdup(resolved);
        char *last_dot = strrchr(branch_number, '.');

        if (last_dot == NULL || last_dot == branch_number)
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("invalid revision number \"%s\"", revision)));
        *last_dot = '\0';
        head = find_branch_tip(archive, branch_number);
        if (head == NULL)
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("branch revision \"%s\" was not found", revision)));
    }
    target = find_revision(archive, resolved);
    if (target == NULL)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("revision \"%s\" was not found", revision)));
    current = head;
    contents = current->text;
    while (current != target)
    {
        if (++steps > archive->revision_count || current->next == NULL)
            break;
        current = find_revision(archive, current->next);
        if (current == NULL)
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                     errmsg("RCS revision chain references a missing revision")));
        contents = apply_script(contents, current->text);
    }
    if (current != target)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("revision \"%s\" was not found", revision)));
    return contents;
}

static void
validate_archive(RcsArchive *archive)
{
    int i;

    (void) find_head(archive);
    for (i = 0; i < archive->revision_count; i++)
    {
        RcsRevision *revision = &archive->revisions[i];
        int j;

        if (revision->next && find_revision(archive, revision->next) == NULL)
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                     errmsg("RCS revision \"%s\" references missing next revision \"%s\"",
                            revision->number, revision->next)));
        for (j = 0; j < revision->branch_count; j++)
        {
            RcsRevision *child = find_revision(archive,
                                                revision->branches[j]);
            Size parent_length = strlen(revision->number);

            if (child == NULL ||
                strncmp(child->number, revision->number, parent_length) != 0 ||
                child->number[parent_length] != '.')
                ereport(ERROR,
                        (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                         errmsg("RCS revision \"%s\" references invalid branch \"%s\"",
                                revision->number, revision->branches[j])));
        }
        (void) checkout_internal(archive, revision->number);
    }

    for (i = 0; i < archive->symbol_count; i++)
    {
        RcsSymbol *symbol = &archive->symbols[i];

        if (is_branch_number(symbol->revision))
        {
            char *parent_number = pstrdup(symbol->revision);
            char *last_dot = strrchr(parent_number, '.');

            if (last_dot == NULL)
                ereport(ERROR,
                        (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                         errmsg("invalid branch symbol \"%s\"", symbol->name)));
            *last_dot = '\0';
            if (find_revision(archive, parent_number) == NULL)
                ereport(ERROR,
                        (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                         errmsg("branch symbol \"%s\" references missing parent revision",
                                symbol->name)));
        }
        else if (find_revision(archive, symbol->revision) == NULL)
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                     errmsg("RCS symbol \"%s\" references missing revision \"%s\"",
                            symbol->name, symbol->revision)));
    }
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
    int prefix = 0;
    int suffix = 0;
    int i;

    split_lines(old_contents, &old_lines);
    split_lines(new_contents, &new_lines);
    while (prefix < old_lines.count && prefix < new_lines.count &&
           strcmp(old_lines.items[prefix], new_lines.items[prefix]) == 0)
        prefix++;
    while (suffix < old_lines.count - prefix &&
           suffix < new_lines.count - prefix &&
           strcmp(old_lines.items[old_lines.count - suffix - 1],
                  new_lines.items[new_lines.count - suffix - 1]) == 0)
        suffix++;

    if (new_lines.count - prefix - suffix > 0)
        appendStringInfo(result, "d%d %d\n", prefix + 1,
                         new_lines.count - prefix - suffix);
    if (old_lines.count - prefix - suffix > 0)
    {
        appendStringInfo(result, "a%d %d\n", prefix,
                         old_lines.count - prefix - suffix);
        for (i = prefix; i < old_lines.count - suffix; i++)
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

static int
compare_edits(const void *left, const void *right)
{
    const RcsEdit *a = (const RcsEdit *) left;
    const RcsEdit *b = (const RcsEdit *) right;

    if (a->start < b->start)
        return -1;
    if (a->start > b->start)
        return 1;
    return 0;
}

static char *
apply_edits(const char *source, Size length, RcsEdit *edits, int count)
{
    StringInfoData result;
    Size position = 0;
    int i;

    qsort(edits, count, sizeof(RcsEdit), compare_edits);
    initStringInfo(&result);
    for (i = 0; i < count; i++)
    {
        if (edits[i].start < position || edits[i].end < edits[i].start ||
            edits[i].end > length)
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("overlapping edits in RCS archive")));
        appendBinaryStringInfo(&result, source + position,
                               edits[i].start - position);
        appendStringInfoString(&result, edits[i].replacement);
        position = edits[i].end;
    }
    appendBinaryStringInfo(&result, source + position, length - position);
    return result.data;
}

static bool
valid_symbol_name(const char *name)
{
    const unsigned char *p = (const unsigned char *) name;

    if (*p == '\0' || !(isalpha(*p) || *p == '_'))
        return false;
    for (p++; *p; p++)
        if (!(isalnum(*p) || *p == '_' || *p == '-'))
            return false;
    return true;
}

static void
set_symbol_edit(RcsArchive *archive, const char *name, const char *revision,
                RcsEdit *edit)
{
    RcsSymbol *existing = find_symbol(archive, name);
    char *value = psprintf("%s:%s", name, revision);

    if (existing)
    {
        edit->start = existing->start;
        edit->end = existing->end;
        edit->replacement = value;
    }
    else
    {
        if (archive->symbols_end == 0)
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                     errmsg("RCS archive has no symbols field")));
        edit->start = archive->symbols_end;
        edit->end = archive->symbols_end;
        edit->replacement = psprintf(" %s", value);
    }
}

static bool
parent_has_branch(RcsRevision *parent, const char *branch_revision)
{
    int i;

    for (i = 0; i < parent->branch_count; i++)
        if (strcmp(parent->branches[i], branch_revision) == 0)
            return true;
    return false;
}

static void
validate_checkin_content(const char *contents)
{
    Size length = strlen(contents);

    if (length > RCS_MAX_CONTENT_SIZE)
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("RCS content exceeds the 8 MiB size limit")));
    if (length > 0 && contents[length - 1] != '\n')
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("RCS text must end with a newline")));
}

static void
validate_log(const char *log)
{
    if (strlen(log) > RCS_MAX_LOG_SIZE)
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("RCS log message exceeds the 1 MiB size limit")));
}

static void
validate_author(const char *author)
{
    const unsigned char *p;

    for (p = (const unsigned char *) author; *p; p++)
        if (isspace(*p) || *p == ';' || *p == '@')
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("database role name cannot be represented in an RCS author field")));
}

PG_FUNCTION_INFO_V1(rcs_is_valid);
Datum
rcs_is_valid(PG_FUNCTION_ARGS)
{
    text *input = PG_GETARG_TEXT_PP(0);
    char *source;
    bool valid = true;
    MemoryContext oldcontext = CurrentMemoryContext;

    if (VARSIZE_ANY_EXHDR(input) == 0)
        PG_RETURN_BOOL(true);
    if (VARSIZE_ANY_EXHDR(input) > RCS_MAX_ARCHIVE_SIZE)
        PG_RETURN_BOOL(false);
    source = text_to_cstring(input);

    PG_TRY();
    {
        RcsArchive *archive = parse_archive(source, strlen(source));

        validate_archive(archive);
    }
    PG_CATCH();
    {
        ErrorData *error;

        MemoryContextSwitchTo(oldcontext);
        error = CopyErrorData();

        FlushErrorState();
        if (error->sqlerrcode != ERRCODE_INVALID_TEXT_REPRESENTATION &&
            error->sqlerrcode != ERRCODE_INVALID_PARAMETER_VALUE &&
            error->sqlerrcode != ERRCODE_FEATURE_NOT_SUPPORTED)
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
    RcsArchive *archive = NULL;
    RcsRevision *head = NULL;

    validate_author(author);
    validate_log(log);

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

    validate_checkin_content(contents);

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

PG_FUNCTION_INFO_V1(rcs_head);
Datum
rcs_head(PG_FUNCTION_ARGS)
{
    char *source = text_to_cstring(PG_GETARG_TEXT_PP(0));
    RcsArchive *archive = parse_archive(source, strlen(source));

    PG_RETURN_TEXT_P(cstring_to_text(find_head(archive)->number));
}

PG_FUNCTION_INFO_V1(rcs_resolve_revision);
Datum
rcs_resolve_revision(PG_FUNCTION_ARGS)
{
    char *source = text_to_cstring(PG_GETARG_TEXT_PP(0));
    char *name = text_to_cstring(PG_GETARG_TEXT_PP(1));
    RcsArchive *archive = parse_archive(source, strlen(source));
    char *resolved = resolve_revision_name(archive, name);
    RcsSymbol *symbol = find_symbol(archive, name);

    if (!find_revision(archive, resolved) &&
        !(is_branch_number(resolved) &&
          ((symbol && is_branch_number(symbol->revision)) ||
           find_branch_tip(archive, resolved))))
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("revision or symbol \"%s\" was not found", name)));
    PG_RETURN_TEXT_P(cstring_to_text(resolved));
}

PG_FUNCTION_INFO_V1(rcs_create_branch);
Datum
rcs_create_branch(PG_FUNCTION_ARGS)
{
    text *archive_arg = PG_GETARG_TEXT_PP(0);
    char *source = text_to_cstring(archive_arg);
    char *name = text_to_cstring(PG_GETARG_TEXT_PP(1));
    char *parent_number = text_to_cstring(PG_GETARG_TEXT_PP(2));
    RcsArchive *archive;
    RcsRevision *parent;
    RcsSymbol *existing;
    char *resolved;
    char *prefix;
    long next_branch = 1;
    char *branch_number;
    RcsEdit edit;
    int i;

    if (VARSIZE_ANY_EXHDR(archive_arg) == 0)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("cannot create a branch in an empty archive")));
    if (!valid_symbol_name(name))
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_NAME),
                 errmsg("invalid RCS branch name \"%s\"", name)));
    archive = parse_archive(source, strlen(source));
    existing = find_symbol(archive, name);
    if (existing)
        ereport(ERROR,
                (errcode(ERRCODE_DUPLICATE_OBJECT),
                 errmsg("RCS symbol \"%s\" already exists", name)));
    resolved = resolve_revision_name(archive, parent_number);
    parent = find_revision(archive, resolved);
    if (parent == NULL || !is_trunk_revision(resolved))
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("branches can currently be created only from trunk revisions")));

    prefix = psprintf("%s.", parent->number);
    for (i = 0; i < parent->branch_count; i++)
    {
        const char *suffix;
        char *end;
        long branch_id;

        if (strncmp(parent->branches[i], prefix, strlen(prefix)) != 0)
            continue;
        suffix = parent->branches[i] + strlen(prefix);
        branch_id = strtol(suffix, &end, 10);
        if (end == suffix || (*end != '.' && *end != '\0'))
            continue;
        if (branch_id >= next_branch && branch_id < INT_MAX)
            next_branch = branch_id + 1;
    }
    for (i = 0; i < archive->symbol_count; i++)
    {
        const char *suffix;
        char *end;
        long branch_id;

        if (!is_branch_number(archive->symbols[i].revision) ||
            strncmp(archive->symbols[i].revision, prefix,
                    strlen(prefix)) != 0)
            continue;
        suffix = archive->symbols[i].revision + strlen(prefix);
        branch_id = strtol(suffix, &end, 10);
        if (end == suffix || *end != '\0')
            continue;
        if (branch_id >= next_branch && branch_id < INT_MAX)
            next_branch = branch_id + 1;
    }
    branch_number = psprintf("%s%ld", prefix, next_branch);
    set_symbol_edit(archive, name, branch_number, &edit);
    PG_RETURN_TEXT_P(cstring_to_text(
        apply_edits(source, archive->length, &edit, 1)));
}

PG_FUNCTION_INFO_V1(rcs_tag);
Datum
rcs_tag(PG_FUNCTION_ARGS)
{
    text *archive_arg = PG_GETARG_TEXT_PP(0);
    char *source = text_to_cstring(archive_arg);
    char *name = text_to_cstring(PG_GETARG_TEXT_PP(1));
    char *revision_name = text_to_cstring(PG_GETARG_TEXT_PP(2));
    RcsArchive *archive;
    char *revision;
    RcsEdit edit;

    if (VARSIZE_ANY_EXHDR(archive_arg) == 0)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("cannot tag a revision in an empty archive")));
    if (!valid_symbol_name(name))
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_NAME),
                 errmsg("invalid RCS tag name \"%s\"", name)));
    archive = parse_archive(source, strlen(source));
    revision = resolve_revision_name(archive, revision_name);
    if (!find_revision(archive, revision) &&
        !(is_branch_number(revision) && find_branch_tip(archive, revision)))
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("revision or symbol \"%s\" was not found",
                        revision_name)));
    {
        RcsSymbol *existing = find_symbol(archive, name);

        if (existing && is_branch_number(existing->revision))
            ereport(ERROR,
                    (errcode(ERRCODE_DUPLICATE_OBJECT),
                     errmsg("RCS symbol \"%s\" is a branch and cannot be replaced by a tag",
                            name)));
    }
    set_symbol_edit(archive, name, revision, &edit);
    PG_RETURN_TEXT_P(cstring_to_text(
        apply_edits(source, archive->length, &edit, 1)));
}

PG_FUNCTION_INFO_V1(rcs_branch_checkin);
Datum
rcs_branch_checkin(PG_FUNCTION_ARGS)
{
    text *archive_arg = PG_GETARG_TEXT_PP(0);
    char *source = text_to_cstring(archive_arg);
    char *branch_name = text_to_cstring(PG_GETARG_TEXT_PP(1));
    char *contents = text_to_cstring(PG_GETARG_TEXT_PP(2));
    char *log = text_to_cstring(PG_GETARG_TEXT_PP(3));
    char *author = GetUserNameFromId(GetUserId(), false);
    char *date = format_rcs_date();
    RcsArchive *archive;
    RcsSymbol *symbol;
    RcsRevision *parent;
    RcsRevision *branch_head;
    char *branch_number;
    char *parent_number;
    char *new_number;
    char *new_text;
    StringInfoData metadata;
    StringInfoData delta;
    RcsEdit edits[2];
    int edit_count = 0;
    char *updated_source;

    if (VARSIZE_ANY_EXHDR(archive_arg) == 0)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("cannot check in to a branch in an empty archive")));
    validate_checkin_content(contents);
    validate_log(log);
    validate_author(author);
    archive = parse_archive(source, strlen(source));
    symbol = find_symbol(archive, branch_name);
    if (symbol == NULL || !is_branch_number(symbol->revision))
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("branch \"%s\" does not exist", branch_name),
                 errhint("Create it first with rcs_create_branch.")));
    branch_number = symbol->revision;
    parent_number = pstrdup(branch_number);
    {
        char *last_dot = strrchr(parent_number, '.');

        if (last_dot == NULL)
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                     errmsg("invalid RCS branch number \"%s\"", branch_number)));
        *last_dot = '\0';
    }
    parent = find_revision(archive, parent_number);
    if (parent == NULL)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                 errmsg("branch \"%s\" references missing parent revision \"%s\"",
                        branch_name, parent_number)));
    branch_head = find_branch_tip(archive, branch_number);
    if (branch_head == NULL)
    {
        new_number = psprintf("%s.1", branch_number);
        if (!parent_has_branch(parent, new_number))
        {
            edits[edit_count].start = parent->branches_end;
            edits[edit_count].end = parent->branches_end;
            edits[edit_count++].replacement = psprintf(" %s", new_number);
        }
    }
    else
    {
        char *minor_text = branch_head->number +
            strlen(branch_number) + 1;
        char *end;
        long minor = strtol(minor_text, &end, 10);

        if (*end != '\0' || minor <= 0 || minor >= INT_MAX)
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                     errmsg("invalid branch revision number \"%s\"",
                            branch_head->number)));
        new_number = psprintf("%s.%ld", branch_number, minor + 1);
        {
            char *previous = checkout_internal(archive, branch_head->number);
            StringInfoData reverse_script;

            initStringInfo(&reverse_script);
            append_edit_script(&reverse_script, previous, contents);
            edits[edit_count].start = branch_head->text_start;
            edits[edit_count].end = branch_head->text_end;
            edits[edit_count++].replacement = quote_rcs(reverse_script.data);
        }
    }

    initStringInfo(&metadata);
    appendStringInfo(&metadata, "%s\ndate %s; author %s; state Exp;\n"
                     "branches;\nnext %s;\n\n",
                     new_number, date, author,
                     branch_head ? branch_head->number : "");
    edits[edit_count].start =
        archive->tokens[archive->description_token].start;
    edits[edit_count].end = edits[edit_count].start;
    edits[edit_count++].replacement = metadata.data;
    updated_source = apply_edits(source, archive->length, edits, edit_count);

    initStringInfo(&delta);
    appendStringInfo(&delta, "\n%s\nlog\n", new_number);
    append_quoted(&delta, log);
    appendStringInfoString(&delta, "\ntext\n");
    append_quoted(&delta, contents);
    appendStringInfoChar(&delta, '\n');
    new_text = palloc(strlen(updated_source) + delta.len + 1);
    memcpy(new_text, updated_source, strlen(updated_source));
    memcpy(new_text + strlen(updated_source), delta.data, delta.len + 1);
    PG_RETURN_TEXT_P(cstring_to_text(new_text));
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
    RcsDiff diff = {NULL, 0, 0};
    int i;

    if (strcmp(left, right) == 0)
        PG_RETURN_TEXT_P(cstring_to_text(""));

    split_lines(left, &left_lines);
    split_lines(right, &right_lines);
    compute_diff_ops(&left_lines, &right_lines, &diff);
    initStringInfo(&result);
    appendStringInfo(&result, "--- %s\n+++ %s\n",
                     left_revision, right_revision);
    for (i = 0; i < diff.count;)
    {
        int old_start;
        int new_start;
        int old_count = 0;
        int new_count = 0;
        int end;
        int j;

        if (diff.items[i].kind == ' ')
        {
            i++;
            continue;
        }
        old_start = diff.items[i].old_line;
        new_start = diff.items[i].new_line;
        end = i;
        while (end < diff.count && diff.items[end].kind != ' ')
        {
            if (diff.items[end].kind == '-')
                old_count++;
            else
                new_count++;
            end++;
        }
        appendStringInfo(&result, "@@ -%d,%d +%d,%d @@\n",
                         old_count ? old_start + 1 : old_start, old_count,
                         new_count ? new_start + 1 : new_start, new_count);
        for (j = i; j < end; j++)
        {
            appendStringInfoChar(&result, diff.items[j].kind);
            appendStringInfoString(&result, diff.items[j].line);
        }
        i = end;
    }
    PG_RETURN_TEXT_P(cstring_to_text(result.data));
}

PG_FUNCTION_INFO_V1(rcs_merge);
Datum
rcs_merge(PG_FUNCTION_ARGS)
{
    char *source = text_to_cstring(PG_GETARG_TEXT_PP(0));
    char *base_revision = text_to_cstring(PG_GETARG_TEXT_PP(1));
    char *ours_revision = text_to_cstring(PG_GETARG_TEXT_PP(2));
    char *theirs_revision = text_to_cstring(PG_GETARG_TEXT_PP(3));
    RcsArchive *archive = parse_archive(source, strlen(source));
    char *base_text = checkout_internal(archive, base_revision);
    char *ours_text = checkout_internal(archive, ours_revision);
    char *theirs_text = checkout_internal(archive, theirs_revision);
    RcsLines base_lines = {NULL, 0, 0};
    RcsLines ours_lines = {NULL, 0, 0};
    RcsLines theirs_lines = {NULL, 0, 0};
    RcsDiff ours_diff = {NULL, 0, 0};
    RcsDiff theirs_diff = {NULL, 0, 0};
    RcsChange *ours_changes;
    RcsChange *theirs_changes;
    int ours_count;
    int theirs_count;
    int ours_index = 0;
    int theirs_index = 0;
    int base_index = 0;
    bool has_conflicts = false;
    StringInfoData merged;
    TupleDesc tuple_desc;
    Datum values[2];
    bool nulls[2] = {false, false};
    HeapTuple tuple;

    split_lines(base_text, &base_lines);
    split_lines(ours_text, &ours_lines);
    split_lines(theirs_text, &theirs_lines);
    compute_diff_ops(&base_lines, &ours_lines, &ours_diff);
    compute_diff_ops(&base_lines, &theirs_lines, &theirs_diff);
    ours_changes = diff_changes(&ours_diff, &ours_count);
    theirs_changes = diff_changes(&theirs_diff, &theirs_count);
    initStringInfo(&merged);

    while (ours_index < ours_count || theirs_index < theirs_count)
    {
        bool take_ours;
        RcsChange *next;
        RcsChange *other = NULL;

        if (ours_index >= ours_count)
            take_ours = false;
        else if (theirs_index >= theirs_count)
            take_ours = true;
        else if (ours_changes[ours_index].start !=
                 theirs_changes[theirs_index].start)
            take_ours = ours_changes[ours_index].start <
                theirs_changes[theirs_index].start;
        else
            take_ours = ours_changes[ours_index].start ==
                ours_changes[ours_index].end ||
                theirs_changes[theirs_index].start !=
                theirs_changes[theirs_index].end;

        next = take_ours ? &ours_changes[ours_index]
            : &theirs_changes[theirs_index];
        if (take_ours && theirs_index < theirs_count)
            other = &theirs_changes[theirs_index];
        else if (!take_ours && ours_index < ours_count)
            other = &ours_changes[ours_index];

        if (other && change_overlaps_region(next, other->start, other->end))
        {
            int cluster_start = Min(next->start, other->start);
            int cluster_end = Max(next->end, other->end);
            int ours_first = ours_index;
            int theirs_first = theirs_index;
            int ours_end;
            int theirs_end;
            bool expanded;
            char *ours_variant;
            char *theirs_variant;

            ours_index++;
            theirs_index++;
            do
            {
                expanded = false;
                while (ours_index < ours_count &&
                       change_overlaps_region(&ours_changes[ours_index],
                                              cluster_start, cluster_end))
                {
                    cluster_start = Min(cluster_start,
                                        ours_changes[ours_index].start);
                    cluster_end = Max(cluster_end,
                                      ours_changes[ours_index].end);
                    ours_index++;
                    expanded = true;
                }
                while (theirs_index < theirs_count &&
                       change_overlaps_region(&theirs_changes[theirs_index],
                                              cluster_start, cluster_end))
                {
                    cluster_start = Min(cluster_start,
                                        theirs_changes[theirs_index].start);
                    cluster_end = Max(cluster_end,
                                      theirs_changes[theirs_index].end);
                    theirs_index++;
                    expanded = true;
                }
            } while (expanded);

            ours_end = ours_index;
            theirs_end = theirs_index;
            append_base_lines(&merged, &base_lines, base_index,
                              cluster_start);
            ours_variant = merge_variant(&base_lines, ours_changes,
                                         ours_first, ours_end,
                                         cluster_start, cluster_end);
            theirs_variant = merge_variant(&base_lines, theirs_changes,
                                           theirs_first, theirs_end,
                                           cluster_start, cluster_end);
            if (strcmp(ours_variant, theirs_variant) == 0)
                appendStringInfoString(&merged, ours_variant);
            else
            {
                appendStringInfoString(&merged, "<<<<<<< ours\n");
                appendStringInfoString(&merged, ours_variant);
                appendStringInfoString(&merged, "=======\n");
                appendStringInfoString(&merged, theirs_variant);
                appendStringInfoString(&merged, ">>>>>>> theirs\n");
                has_conflicts = true;
            }
            base_index = cluster_end;
        }
        else
        {
            append_base_lines(&merged, &base_lines, base_index, next->start);
            append_change_lines(&merged, next);
            base_index = next->end;
            if (take_ours)
                ours_index++;
            else
                theirs_index++;
        }
    }
    append_base_lines(&merged, &base_lines, base_index, base_lines.count);

    if (get_call_result_type(fcinfo, NULL, &tuple_desc) != TYPEFUNC_COMPOSITE)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("rcs_merge must return a composite type")));
    tuple_desc = BlessTupleDesc(tuple_desc);
    values[0] = CStringGetTextDatum(merged.data);
    values[1] = BoolGetDatum(has_conflicts);
    tuple = heap_form_tuple(tuple_desc, values, nulls);
    PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
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

PG_FUNCTION_INFO_V1(rcs_symbols);
Datum
rcs_symbols(PG_FUNCTION_ARGS)
{
    ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
    MemoryContext oldcontext;
    TupleDesc tupledesc;
    Tuplestorestate *store;
    char *source;
    RcsArchive *archive;
    int i;

    if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo) ||
        !(rsinfo->allowedModes & SFRM_Materialize))
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("materialized set mode is required")));
    oldcontext = MemoryContextSwitchTo(rsinfo->econtext->ecxt_per_query_memory);
    source = text_to_cstring(PG_GETARG_TEXT_PP(0));
    archive = parse_archive(source, strlen(source));
    if (get_call_result_type(fcinfo, NULL, &tupledesc) != TYPEFUNC_COMPOSITE)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("rcs_symbols must be called in a record context")));
    tupledesc = BlessTupleDesc(tupledesc);
    store = tuplestore_begin_heap(true, false, work_mem);
    for (i = 0; i < archive->symbol_count; i++)
    {
        Datum values[3];
        bool nulls[3] = {false, false, false};
        HeapTuple tuple;
        RcsSymbol *symbol = &archive->symbols[i];

        values[0] = CStringGetTextDatum(symbol->name);
        values[1] = CStringGetTextDatum(symbol->revision);
        values[2] = BoolGetDatum(is_branch_number(symbol->revision));
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
