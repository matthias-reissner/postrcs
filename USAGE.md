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

Die aktuelle Revision kann mit ihrer Nummer ausgecheckt werden:

```sql
SELECT rcs_checkout(history, '1.1') AS content
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

Gleiche Inhalte ergeben einen leeren Diff.

## Archive prüfen

Der Typ `rcs_archive` prüft beim Einfügen und Ändern die RCS-Archivstruktur.
Zusätzlich kann ein beliebiger Text mit `rcs_is_valid` geprüft werden:

```sql
SELECT rcs_is_valid(history::text)
FROM documents
WHERE id = 1;
```

## Derzeitige Einschränkungen

- Check-in und Checkout unterstützen nur trunk-Revisionen; Branches werden
  nicht unterstützt.
- Es werden Textinhalte versioniert, keine Binärdateien.
- Nicht-leere Inhalte müssen mit einem Zeilenumbruch enden.
- Der Diff enthält die vollständigen alten und neuen Inhalte statt nur der
  geänderten Zeilen.
- `rcs_archive` speichert ein Archiv in einer Spalte; es erzeugt keine
  automatische Historie für andere Spalten.
