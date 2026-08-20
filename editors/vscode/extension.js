/* Cub support for VS Code.
 *
 * Three things happen here:
 *
 *   1. Formatting  -- `cubc fmt -` is fed the buffer and its output replaces
 *      the document, so Format Document and format-on-save both work.
 *   2. Diagnostics -- `cubc --check` runs as you type and its errors become
 *      squiggles, with the compiler's `help:` line kept as the explanation.
 *   3. Commands    -- run the current file, or look at the C it becomes.
 *
 * There is no build step: this is plain CommonJS, loaded as-is.
 */
const vscode = require('vscode');
const cp = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

/** Where cubc lives, from the user's settings. */
function compiler() {
  return vscode.workspace.getConfiguration('cub').get('compilerPath') || 'cubc';
}

/** Run cubc, returning { code, stdout, stderr }. Never rejects. */
function runCubc(args, stdin) {
  return new Promise((resolve) => {
    let child;
    try {
      child = cp.spawn(compiler(), args, { env: { ...process.env, NO_COLOR: '1' } });
    } catch (err) {
      return resolve({ code: -1, stdout: '', stderr: String(err), spawnFailed: true });
    }
    let stdout = '', stderr = '';
    child.stdout.on('data', (d) => { stdout += d; });
    child.stderr.on('data', (d) => { stderr += d; });
    child.on('error', (err) =>
      resolve({ code: -1, stdout: '', stderr: String(err), spawnFailed: true }));
    child.on('close', (code) => resolve({ code, stdout, stderr }));
    if (stdin !== undefined) { child.stdin.write(stdin); child.stdin.end(); }
  });
}

let warnedMissing = false;
function warnIfMissing(result) {
  if (!result.spawnFailed || warnedMissing) return;
  warnedMissing = true;
  vscode.window.showWarningMessage(
    `Cub: could not run "${compiler()}". Set "cub.compilerPath" to the full path ` +
    `of your cubc binary, or install it with "make install".`);
}

/* ------------------------------------------------------------------ */
/* diagnostics                                                         */
/* ------------------------------------------------------------------ */

/* cubc reports problems as:
 *
 *   file.cub:2:15: error: cannot add int and string
 *        2 |     let n = 1 + "two"
 *          |               ^
 *     help: turn the other side into text with `str(x)`
 *
 * The location line opens a diagnostic; a following `help:` line becomes
 * part of its message, because that is the half that says what to do.
 */
function parseDiagnostics(output, doc) {
  const diags = [];
  const location = /^(.*):(\d+):(\d+):\s+(error|warning|note):\s+(.*)$/;
  let last = null;

  for (const line of output.split('\n')) {
    const m = line.match(location);
    if (m) {
      const lineNo = Math.max(0, parseInt(m[2], 10) - 1);
      const colNo = Math.max(0, parseInt(m[3], 10) - 1);

      // Underline the whole word at the reported column, not a single char.
      let range;
      if (lineNo < doc.lineCount) {
        const wordRange = doc.getWordRangeAtPosition(new vscode.Position(lineNo, colNo));
        range = wordRange || new vscode.Range(lineNo, colNo, lineNo, colNo + 1);
      } else {
        range = new vscode.Range(lineNo, colNo, lineNo, colNo + 1);
      }

      const severity = m[4] === 'error' ? vscode.DiagnosticSeverity.Error
                     : m[4] === 'warning' ? vscode.DiagnosticSeverity.Warning
                     : vscode.DiagnosticSeverity.Information;

      last = new vscode.Diagnostic(range, m[5], severity);
      last.source = 'cubc';
      diags.push(last);
      continue;
    }
    const help = line.match(/^\s*help:\s+(.*)$/);
    if (help && last) last.message += `\n\nhelp: ${help[1]}`;
  }
  return diags;
}

async function check(doc, collection) {
  if (doc.languageId !== 'cub') return;

  // cubc reads files, so an unsaved buffer goes through a temp copy.
  let target = doc.fileName;
  let temp = null;
  if (doc.isDirty || doc.isUntitled) {
    temp = path.join(os.tmpdir(), `cub-check-${process.pid}-${Date.now()}.cub`);
    fs.writeFileSync(temp, doc.getText());
    target = temp;
  }

  const result = await runCubc(['--check', target]);
  if (temp) { try { fs.unlinkSync(temp); } catch (e) { /* already gone */ } }

  if (result.spawnFailed) { warnIfMissing(result); collection.delete(doc.uri); return; }
  collection.set(doc.uri, parseDiagnostics(result.stderr, doc));
}

/* ------------------------------------------------------------------ */
/* activation                                                          */
/* ------------------------------------------------------------------ */

function activate(context) {
  const diagnostics = vscode.languages.createDiagnosticCollection('cub');
  context.subscriptions.push(diagnostics);

  /* ---- formatting ---- */
  context.subscriptions.push(
    vscode.languages.registerDocumentFormattingEditProvider('cub', {
      async provideDocumentFormattingEdits(document) {
        const text = document.getText();
        const result = await runCubc(['fmt', '-'], text);

        if (result.spawnFailed) { warnIfMissing(result); return []; }
        if (result.code !== 0) {
          // A file that does not lex cannot be formatted; say so quietly.
          vscode.window.setStatusBarMessage(
            'Cub: cannot format a file with syntax errors', 3000);
          return [];
        }
        if (result.stdout === text) return [];

        const whole = new vscode.Range(
          document.positionAt(0), document.positionAt(text.length));
        return [vscode.TextEdit.replace(whole, result.stdout)];
      }
    })
  );

  /* ---- diagnostics ---- */
  let timer = null;
  const scheduleCheck = (doc) => {
    const cfg = vscode.workspace.getConfiguration('cub');
    if (!cfg.get('checkOnType')) return;
    if (timer) clearTimeout(timer);
    timer = setTimeout(() => check(doc, diagnostics), cfg.get('checkDelay') || 400);
  };

  context.subscriptions.push(
    vscode.workspace.onDidOpenTextDocument((doc) => check(doc, diagnostics)),
    vscode.workspace.onDidSaveTextDocument((doc) => check(doc, diagnostics)),
    vscode.workspace.onDidChangeTextDocument((e) => {
      if (e.document.languageId === 'cub') scheduleCheck(e.document);
    }),
    vscode.workspace.onDidCloseTextDocument((doc) => diagnostics.delete(doc.uri))
  );
  vscode.workspace.textDocuments.forEach((doc) => check(doc, diagnostics));

  /* ---- commands ---- */
  const activeCubFile = () => {
    const editor = vscode.window.activeTextEditor;
    if (!editor || editor.document.languageId !== 'cub') {
      vscode.window.showInformationMessage('Cub: open a .cub file first.');
      return null;
    }
    return editor.document;
  };

  context.subscriptions.push(
    vscode.commands.registerCommand('cub.run', async () => {
      const doc = activeCubFile();
      if (!doc) return;
      await doc.save();
      const terminal = vscode.window.terminals.find((t) => t.name === 'Cub')
        || vscode.window.createTerminal('Cub');
      terminal.show();
      terminal.sendText(`${compiler()} run ${JSON.stringify(doc.fileName)}`);
    }),

    vscode.commands.registerCommand('cub.check', async () => {
      const doc = activeCubFile();
      if (!doc) return;
      await check(doc, diagnostics);
      const found = diagnostics.get(doc.uri) || [];
      vscode.window.showInformationMessage(
        found.length === 0 ? 'Cub: no problems found.'
                           : `Cub: ${found.length} problem(s) found.`);
    }),

    vscode.commands.registerCommand('cub.emitC', async () => {
      const doc = activeCubFile();
      if (!doc) return;
      await doc.save();
      const out = path.join(os.tmpdir(), `cub-emit-${process.pid}.c`);
      const result = await runCubc([doc.fileName, '--emit-c', '-o', out]);
      if (result.spawnFailed) { warnIfMissing(result); return; }
      if (result.code !== 0) {
        vscode.window.showErrorMessage('Cub: the file has errors, so no C was written.');
        return;
      }
      const shown = await vscode.workspace.openTextDocument(out);
      vscode.window.showTextDocument(shown, { preview: true });
    })
  );
}

function deactivate() {}

module.exports = { activate, deactivate };
