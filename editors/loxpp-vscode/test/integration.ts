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
  await vscode.window.showTextDocument(document);
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

  const hover = await vscode.commands.executeCommand<vscode.Hover[]>(
    'vscode.executeHoverProvider',
    document.uri,
    new vscode.Position(0, 1)
  );
  assert.ok(Array.isArray(hover), 'hover must respond');

  const symbols = await vscode.commands.executeCommand<vscode.DocumentSymbol[]>(
    'vscode.executeDocumentSymbolProvider',
    document.uri
  );
  assert.ok(Array.isArray(symbols), 'document symbols must respond');

  const definitions = await vscode.commands.executeCommand<vscode.Location[]>(
    'vscode.executeDefinitionProvider',
    document.uri,
    new vscode.Position(0, 1)
  );
  assert.ok(Array.isArray(definitions), 'definition provider must respond');

  await vscode.commands.executeCommand('vscode.open', document.uri);
  await vscode.commands.executeCommand('workbench.action.closeActiveEditor');
}
