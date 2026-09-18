import { strict as assert } from 'node:assert';
import * as vscode from 'vscode';

async function waitForDiagnostics(
  uri: vscode.Uri,
  predicate: (diagnostics: readonly vscode.Diagnostic[]) => boolean,
  label: string
): Promise<readonly vscode.Diagnostic[]> {
  const start = Date.now();
  for (;;) {
    const diagnostics = vscode.languages.getDiagnostics(uri);
    if (predicate(diagnostics)) {
      return diagnostics;
    }
    if (Date.now() - start > 30000) {
      assert.fail(`timed out waiting for ${label}`);
    }
    await new Promise((resolve) => setTimeout(resolve, 250));
  }
}

export async function run(): Promise<void> {
  const lspPath = process.env.LOXPP_LSP_PATH;
  assert.ok(lspPath, 'LOXPP_LSP_PATH must point at the loxpp-lsp binary');

  await vscode.workspace.getConfiguration('loxpp.server').update('path', lspPath, vscode.ConfigurationTarget.Global);

  const document = await vscode.workspace.openTextDocument({
    language: 'lox',
    content: 'var value = ;\n'
  });
  const editor = await vscode.window.showTextDocument(document);
  await vscode.commands.executeCommand('loxpp.restartServer');

  const diagnostics = await waitForDiagnostics(
    document.uri,
    (items) => items.length > 0,
    'diagnostics from loxpp-lsp'
  );
  assert.ok(
    diagnostics.some((item) => item.severity === vscode.DiagnosticSeverity.Error),
    'expected at least one error diagnostic'
  );

  await editor.edit((builder) => {
    builder.replace(
      new vscode.Range(new vscode.Position(0, 0), new vscode.Position(1, 0)),
      'var greeting = "hello";\nprint greeting;\n'
    );
  });
  await waitForDiagnostics(document.uri, (items) => items.length === 0, 'clean diagnostics for valid code');

  const symbols = await vscode.commands.executeCommand<vscode.DocumentSymbol[]>(
    'vscode.executeDocumentSymbolProvider',
    document.uri
  );
  assert.ok(Array.isArray(symbols) && symbols.length > 0, 'expected document symbols for valid code');
  assert.ok(symbols.some((symbol) => symbol.name === 'greeting'), 'expected a symbol named greeting');

  const usage = new vscode.Position(1, 8);
  const hover = await vscode.commands.executeCommand<vscode.Hover[]>(
    'vscode.executeHoverProvider',
    document.uri,
    usage
  );
  assert.ok(Array.isArray(hover) && hover.length > 0, 'expected hover for a user symbol');
  assert.ok(hover.some((entry) => entry.contents.length > 0), 'expected non-empty hover contents');

  const definitions = await vscode.commands.executeCommand<vscode.Location[]>(
    'vscode.executeDefinitionProvider',
    document.uri,
    usage
  );
  assert.ok(Array.isArray(definitions) && definitions.length > 0, 'expected a definition for a user symbol');

  await vscode.commands.executeCommand('vscode.open', document.uri);
  await vscode.commands.executeCommand('workbench.action.closeActiveEditor');
}
