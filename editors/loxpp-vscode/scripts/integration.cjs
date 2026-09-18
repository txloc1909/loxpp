const path = require('node:path');
const fs = require('node:fs/promises');
const os = require('node:os');
const { runTests } = require('@vscode/test-electron');

async function main() {
  const root = path.resolve(__dirname, '..');
  const temp = await fs.mkdtemp(path.join(os.tmpdir(), 'loxpp-vscode-'));
  try {
    const workspace = path.join(temp, 'workspace');
    await fs.mkdir(workspace);
    await fs.writeFile(path.join(workspace, 'probe.lox'), 'var value = ;\n');
    await runTests({
      version: '1.85.2',
      extensionDevelopmentPath: root,
      extensionTestsPath: path.join(root, 'out/test/integration.js'),
      extensionTestsEnv: {
        LOXPP_LSP_PATH: process.env.LOXPP_LSP_PATH || path.resolve(root, '../../build/loxpp-lsp')
      },
      launchArgs: [
        workspace, '--no-sandbox', '--disable-gpu', '--disable-workspace-trust',
        '--skip-welcome', '--skip-release-notes', '--disable-extensions',
        '--user-data-dir', path.join(temp, 'user'),
        '--extensions-dir', path.join(temp, 'extensions')
      ]
    });
  } finally {
    await fs.rm(temp, { recursive: true, force: true });
  }
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
