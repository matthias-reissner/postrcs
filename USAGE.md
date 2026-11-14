# PostRCS verwenden

PostRCS speichert ein natives RCS-Archiv als Text in einer PostgreSQL-Spalte.
Es versioniert Spalten nicht automatisch: Die Anwendung ruft `rcs_checkin` auf
und speichert den zurückgegebenen Archivwert selbst.

## Extension in einer Datenbank aktivieren

Nachdem die PostRCS-Dateien auf dem PostgreSQL-Server installiert wurden,
aktiviert ein Datenbankadministrator die Extension einmal pro Datenbank:

```sql
CREATE EXTENSION postrcs;
```

Prüfen, ob sie in der aktuellen Datenbank aktiviert ist:

```sql
SELECT extname, extversion
FROM pg_extension
WHERE extname = 'postrcs';
```

Nach Installation der 1.1-Dateien aktualisierst du eine bestehende 1.0-
Installation einmal pro Datenbank:

```sql
ALTER EXTENSION postrcs UPDATE;
```

## Tabelle und erste Revision

Eine `rcs_archive`-Spalte kann ein Archiv pro Datensatz aufnehmen:

```sql
CREATE TABLE documents (
    id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    title text NOT NULL,
    history rcs_archive NOT NULL DEFAULT ''::rcs_archive
);
```

Die erste Revision beginnt mit einem leeren Archiv. `rcs_checkin` gibt das
aktualisierte Archiv zurück; dieses muss in der Tabelle gespeichert werden:

```sql
INSERT INTO documents (title, history)
VALUES (
    'Einführungsdokument',
    rcs_checkin(''::rcs_archive, E'Erste Zeile\n', 'Erstellung')
)
RETURNING id;
```

Das Beispiel erzeugt die Revision `1.1`. Der Inhalt wird als PostgreSQL-Text
übergeben. Das `E`-Stringliteral interpretiert `\n` als Zeilenumbruch.

## Änderungen einchecken

Rufe `rcs_checkin(archiv, neuer_inhalt, kommentar)` auf und speichere den
Rückgabewert. Bei einem vorhandenen Archiv wird die nächste trunk-Revision
angelegt:

```sql
UPDATE documents
SET history = rcs_checkin(
    history,
    E'Erste Zeile\nZweite Zeile\n',
    'Zweite Zeile ergänzt'
)
WHERE id = 1;
```

Autor und Datum werden automatisch aus dem aktuellen Datenbankbenutzer und
der Serverzeit übernommen. Verwende für Inhalt und Kommentar keine `NULL`-
Werte.

## Revisionen auslesen

Die aktuelle trunk-Revision liefert `rcs_head`. Eine Revision kann mit ihrer
Nummer oder einem symbolischen Namen ausgecheckt werden:

```sql
SELECT rcs_head(history) AS head,
       rcs_checkout(history, '1.1') AS content
FROM documents
WHERE id = 1;
```

Die Metadaten aller im Archiv enthaltenen Revisionen lassen sich mit
`rcs_revisions` abfragen:

```sql
SELECT revision, author, revision_date, state, log
FROM documents,
     LATERAL rcs_revisions(history)
WHERE id = 1;
```

`rcs_diff` vergleicht zwei Revisionen und gibt einen Unified-Diff als Text
zurück:

```sql
SELECT rcs_diff(history, '1.1', '1.2') AS difference
FROM documents
WHERE id = 1;
```

Namen lassen sich mit `rcs_resolve_revision` auflösen. Gleiche Inhalte
ergeben einen leeren Diff. Der Diff und die gespeicherten Deltas erhalten
gemeinsame Präfixe und Suffixe; mehrere getrennte Änderungen können weiterhin
als ein größerer Änderungsbereich dargestellt werden.

## Branches und Tags

Ein benannter Branch wird von einer bestehenden trunk-Revision abgezweigt.
Danach wird jede Branch-Revision mit `rcs_branch_checkin` eingecheckt:

```sql
UPDATE documents
SET history = rcs_create_branch(history, 'feature', '1.2')
WHERE id = 1;

UPDATE documents
SET history = rcs_branch_checkin(
    history,
    'feature',
    E'Erste Zeile\nBranch-Änderung\n',
    'Feature begonnen'
)
WHERE id = 1;
```

Wiederhole den zweiten Aufruf für weitere Revisionen desselben Branches.
`rcs_checkout(history, 'feature')` liefert dessen aktuelle Revision;
einzelne Branch-Revisionen wie `1.2.1.1` lassen sich ebenfalls auschecken.
Verschachtelte Branches werden derzeit nicht unterstützt.

Ein Tag benennt eine bestehende Revision. Tags können aktualisiert werden;
Branch-Namen bleiben Branch-Verweise:

```sql
UPDATE documents
SET history = rcs_tag(history, 'release_1_2', '1.2')
WHERE id = 1;

SELECT rcs_checkout(history, 'release_1_2')
FROM documents
WHERE id = 1;
```

Alle Symbole und ihre Ziele lassen sich abfragen:

```sql
SELECT name, revision, is_branch
FROM documents,
     LATERAL rcs_symbols(history)
WHERE id = 1;
```

`rcs_create_branch`, `rcs_branch_checkin` und `rcs_tag` liefern jeweils ein
neues Archiv zurück. Speichere den Rückgabewert in der Tabellenspalte.

## Archive prüfen

Der Typ `rcs_archive` prüft beim Einfügen und Ändern die RCS-Archivstruktur.
Zusätzlich kann ein beliebiger Text mit `rcs_is_valid` geprüft werden:

```sql
SELECT rcs_is_valid(history::text)
FROM documents
WHERE id = 1;
```

## Derzeitige Einschränkungen

- Branches können nur von trunk-Revisionen erstellt werden; verschachtelte
  Branches werden nicht unterstützt.
- Es werden Textinhalte versioniert, keine Binärdateien.
- Nicht-leere Inhalte müssen mit einem Zeilenumbruch enden.
- `rcs_archive` speichert ein Archiv in einer Spalte; es erzeugt keine
  automatische Historie für andere Spalten.
