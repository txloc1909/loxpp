import * as vscode from 'vscode';
import { LanguageClient, TransportKind } from 'vscode-languageclient/node';

let client: LanguageClient | undefined;
let pending: Promise<void> = Promise.resolve();
let stopping = false;

async function stop(): Promise<void> {
  const previous = client;
  client = undefined;
  if (previous) {
    await previous.dispose();
  }
}

export async function activate(context: vscode.ExtensionContext): Promise<void> {
  stopping = false;
  const output = vscode.window.createOutputChannel('Lox++');
  context.subscriptions.push(output);

  async function restart(): Promise<void> {
    await stop();
    const config = vscode.workspace.getConfiguration('loxpp.server');
    if (stopping || !vscode.workspace.isTrusted || !config.get<boolean>('enable', true)) {
      return;
    }
    const command = config.get<string>('path', 'loxpp-lsp').trim();
    if (!command) {
      void vscode.window.showErrorMessage('Set loxpp.server.path to the loxpp-lsp executable.');
      return;
    }
    const next = new LanguageClient('loxpp', 'Lox++', {
      command,
      transport: TransportKind.stdio,
      options: { shell: false }
    }, {
      documentSelector: [
        { scheme: 'file', language: 'lox' },
        { scheme: 'untitled', language: 'lox' }
      ],
      outputChannel: output,
      initializationOptions: {},
      connectionOptions: { maxRestartCount: 3 }
    });
    client = next;
    try {
      await next.start();
    } catch (error) {
      output.appendLine(String(error));
      if (client === next) {
        client = undefined;
      }
      await next.dispose();
      void vscode.window.showErrorMessage(
        `Cannot start Lox++ language server: ${command}. Install loxpp-lsp or set loxpp.server.path. See the Lox++ output channel.`
      );
    }
  }

  function scheduleRestart(): Promise<void> {
    pending = pending.then(restart).catch((error: unknown) => {
      output.appendLine(String(error));
      void vscode.window.showErrorMessage('Lox++ server failed. See the Lox++ output channel.');
    });
    return pending;
  }

  context.subscriptions.push(
    vscode.commands.registerCommand('loxpp.restartServer', scheduleRestart),
    vscode.workspace.onDidChangeConfiguration((event) => {
      if (event.affectsConfiguration('loxpp.server')) {
        void scheduleRestart();
      }
    }),
    vscode.workspace.onDidGrantWorkspaceTrust(() => { void scheduleRestart(); })
  );
  await scheduleRestart();
}

export async function deactivate(): Promise<void> {
  stopping = true;
  await pending;
  await stop();
}
