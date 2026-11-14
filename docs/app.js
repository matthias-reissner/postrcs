(() => {
  const contentInput = document.querySelector("#document-content");
  const logInput = document.querySelector("#revision-log");
  const checkinButton = document.querySelector("#checkin-button");
  const resetButton = document.querySelector("#reset-button");
  const revisionSelect = document.querySelector("#revision-select");
  const checkoutButton = document.querySelector("#checkout-button");
  const diffButton = document.querySelector("#diff-button");
  const resultOutput = document.querySelector("#result-output");
  const archiveOutput = document.querySelector("#archive-output");
  const revisionCount = document.querySelector("#revision-count");
  const statusMessage = document.querySelector("#status-message");
  const copyOutputButton = document.querySelector("#copy-output-button");
  const copyArchiveButton = document.querySelector("#copy-archive-button");
  const downloadArchiveButton = document.querySelector("#download-archive-button");

  const initialContent = contentInput.value;
  const emptyMessage = "Checke deine erste Revision ein, um hier Inhalt oder Diff zu sehen.";
  let revisions = [];
  let archiveText = "";
  let outputText = "";

  function splitLines(value) {
    if (value === "") {
      return [];
    }
    if (!value.endsWith("\n")) {
      throw new Error("Nicht-leerer Inhalt muss mit einem Zeilenumbruch enden.");
    }
    return value.match(/[^\n]*\n/g) || [];
  }

  function quoteRcs(value) {
    return `@${value.replace(/@/g, "@@")}@`;
  }

  function makeReverseDelta(newerContent, olderContent) {
    const newerLines = splitLines(newerContent);
    const olderLines = splitLines(olderContent);
    let script = "";

    if (newerLines.length > 0) {
      script += `d1 ${newerLines.length}\n`;
    }
    if (olderLines.length > 0) {
      script += `a0 ${olderLines.length}\n${olderLines.join("")}`;
    }
    return script;
  }

  function applyEditScript(baseContent, script) {
    const lines = splitLines(baseContent);
    let position = 0;

    while (position < script.length) {
      const match = /^([ad])(\d+) (\d+)\n/.exec(script.slice(position));
      if (!match) {
        throw new Error("Ungültiges RCS-Delta im Browser-Archiv.");
      }

      const operation = match[1];
      const lineNumber = Number(match[2]);
      const count = Number(match[3]);
      if (!Number.isSafeInteger(lineNumber) || !Number.isSafeInteger(count) || count <= 0) {
        throw new Error("Ungültiger Zeilenbereich im RCS-Delta.");
      }
      position += match[0].length;

      if (operation === "d") {
        const start = lineNumber - 1;
        if (start < 0 || start >= lines.length || count > lines.length - start) {
          throw new Error("Löschbereich außerhalb des Datei-Inhalts.");
        }
        lines.splice(start, count);
        continue;
      }

      if (lineNumber < 0 || lineNumber > lines.length) {
        throw new Error("Einfügebereich außerhalb des Datei-Inhalts.");
      }
      const additions = [];
      for (let index = 0; index < count; index++) {
        const newline = script.indexOf("\n", position);
        if (newline < 0) {
          throw new Error("Unvollständige Zeilen im RCS-Delta.");
        }
        additions.push(script.slice(position, newline + 1));
        position = newline + 1;
      }
      lines.splice(lineNumber, 0, ...additions);
    }

    return lines.join("");
  }

  function checkout(revisionNumber) {
    const targetIndex = revisions.findIndex((revision) => revision.number === revisionNumber);
    if (targetIndex < 0) {
      throw new Error(`Revision ${revisionNumber} existiert nicht.`);
    }

    let content = revisions[0].text;
    for (let index = 0; index < targetIndex; index++) {
      content = applyEditScript(content, revisions[index + 1].text);
    }
    return content;
  }

  function serializeArchive() {
    if (revisions.length === 0) {
      return "";
    }

    const head = revisions[0].number;
    const headers = revisions.map((revision, index) => {
      const next = revisions[index + 1]?.number || "";
      return `${revision.number}\ndate ${revision.date}; author ${revision.author}; state ${revision.state};\nbranches;\nnext ${next};\n\n`;
    }).join("");
    const deltas = [...revisions].reverse().map((revision) => {
      return `${revision.number}\nlog\n${quoteRcs(revision.log)}\ntext\n${quoteRcs(revision.text)}\n\n`;
    }).join("");

    return `head ${head};\naccess;\nsymbols;\nlocks; strict;\ncomment @# @;\n\n${headers}desc\n@@\n\n${deltas}`;
  }

  function formatDate(date) {
    const parts = [
      date.getFullYear(),
      date.getMonth() + 1,
      date.getDate(),
      date.getHours(),
      date.getMinutes(),
      date.getSeconds()
    ];
    return parts.map((part, index) => String(part).padStart(index === 0 ? 4 : 2, "0")).join(".");
  }

  function nextRevisionNumber(current) {
    const parts = current.split(".");
    if (parts.length !== 2 || parts.some((part) => !/^\d+$/.test(part))) {
      throw new Error("Die Demo unterstützt nur trunk-Revisionen.");
    }
    return `${parts[0]}.${Number(parts[1]) + 1}`;
  }

  function announce(message, isError = false) {
    statusMessage.textContent = message;
    statusMessage.classList.toggle("error", isError);
  }

  function updateView() {
    archiveText = serializeArchive();
    archiveOutput.textContent = archiveText || "Das Archiv wird nach dem ersten Check-in erzeugt.";
    revisionCount.textContent = `${revisions.length} ${revisions.length === 1 ? "Revision" : "Revisionen"}`;
    revisionSelect.replaceChildren();

    if (revisions.length === 0) {
      const option = document.createElement("option");
      option.value = "";
      option.textContent = "Noch keine Revisionen";
      revisionSelect.append(option);
    } else {
      revisions.forEach((revision) => {
        const option = document.createElement("option");
        option.value = revision.number;
        option.textContent = `${revision.number} · ${revision.log || "ohne Kommentar"}`;
        revisionSelect.append(option);
      });
    }

    revisionSelect.disabled = revisions.length === 0;
    checkoutButton.disabled = revisions.length === 0;
    diffButton.disabled = revisions.length < 2;
    copyArchiveButton.disabled = revisions.length === 0;
    downloadArchiveButton.disabled = revisions.length === 0;
    copyOutputButton.disabled = !outputText;
  }

  function formatUnifiedDiff(fromRevision, toRevision, fromText, toText) {
    if (fromText === toText) {
      return "";
    }
    const oldLines = splitLines(fromText);
    const newLines = splitLines(toText);
    const oldStart = oldLines.length ? 1 : 0;
    const newStart = newLines.length ? 1 : 0;
    return [
      `--- ${fromRevision}`,
      `+++ ${toRevision}`,
      `@@ -${oldStart},${oldLines.length} +${newStart},${newLines.length} @@`,
      ...oldLines.map((line) => `-${line.slice(0, -1)}`),
      ...newLines.map((line) => `+${line.slice(0, -1)}`)
    ].join("\n") + "\n";
  }

  checkinButton.addEventListener("click", () => {
    try {
      const nextContent = contentInput.value;
      splitLines(nextContent);

      if (revisions.length === 0) {
        revisions.push({
          number: "1.1",
          date: formatDate(new Date()),
          author: "demo",
          state: "Exp",
          log: logInput.value,
          text: nextContent
        });
      } else {
        const oldHead = checkout(revisions[0].number);
        revisions[0].text = makeReverseDelta(nextContent, oldHead);
        revisions.unshift({
          number: nextRevisionNumber(revisions[0].number),
          date: formatDate(new Date()),
          author: "demo",
          state: "Exp",
          log: logInput.value,
          text: nextContent
        });
      }

      outputText = nextContent;
      resultOutput.textContent = nextContent || "(Leere Datei)";
      updateView();
      revisionSelect.value = revisions[0].number;
      logInput.value = "";
      announce(`Revision ${revisions[0].number} wurde lokal eingecheckt.`);
    } catch (error) {
      announce(error.message, true);
    }
  });

  checkoutButton.addEventListener("click", () => {
    try {
      const revisionNumber = revisionSelect.value;
      outputText = checkout(revisionNumber);
      resultOutput.textContent = outputText || "(Leere Datei)";
      copyOutputButton.disabled = false;
      announce(`Revision ${revisionNumber} ausgecheckt.`);
    } catch (error) {
      announce(error.message, true);
    }
  });

  diffButton.addEventListener("click", () => {
    try {
      const fromRevision = revisionSelect.value;
      const headRevision = revisions[0].number;
      outputText = formatUnifiedDiff(
        fromRevision,
        headRevision,
        checkout(fromRevision),
        checkout(headRevision)
      );
      resultOutput.textContent = outputText || "(Keine Unterschiede)";
      copyOutputButton.disabled = !outputText;
      announce(`Diff zwischen ${fromRevision} und HEAD ${headRevision}.`);
    } catch (error) {
      announce(error.message, true);
    }
  });

  resetButton.addEventListener("click", () => {
    revisions = [];
    archiveText = "";
    outputText = "";
    contentInput.value = initialContent;
    logInput.value = "Erste Revision";
    resultOutput.textContent = emptyMessage;
    updateView();
    announce("Playground zurückgesetzt. Deine Daten bleiben nur in diesem Browser.");
  });

  async function copyText(value, successMessage) {
    try {
      await navigator.clipboard.writeText(value);
      announce(successMessage);
    } catch {
      announce("Kopieren ist hier nicht verfügbar. Markiere den Text und kopiere ihn manuell.", true);
    }
  }

  document.querySelectorAll("[data-copy]").forEach((button) => {
    button.addEventListener("click", () => {
      const code = button.parentElement.querySelector("code").textContent;
      copyText(code, "Befehl in die Zwischenablage kopiert.");
    });
  });

  copyOutputButton.addEventListener("click", () => {
    copyText(outputText, "Ergebnis wurde kopiert.");
  });

  copyArchiveButton.addEventListener("click", () => {
    copyText(archiveText, "RCS-Archiv wurde kopiert.");
  });

  downloadArchiveButton.addEventListener("click", () => {
    if (!archiveText) {
      return;
    }
    const blob = new Blob([archiveText], { type: "text/plain;charset=utf-8" });
    const url = URL.createObjectURL(blob);
    const link = document.createElement("a");
    link.href = url;
    link.download = "demo,v";
    link.click();
    URL.revokeObjectURL(url);
    announce("RCS-Archiv wurde heruntergeladen.");
  });

  updateView();
})();
