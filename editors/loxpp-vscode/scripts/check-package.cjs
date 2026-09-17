'use strict';

const assert = require('node:assert');
const fs = require('node:fs');
const path = require('node:path');
const yauzl = require('yauzl');

const root = path.resolve(__dirname, '..');
const packageJson = JSON.parse(fs.readFileSync(path.join(root, 'package.json'), 'utf8'));
const vsixPath = path.join(root, `${packageJson.name}-${packageJson.version}.vsix`);

function openVsix(file) {
  return new Promise((resolve, reject) => {
    yauzl.open(file, { lazyEntries: true }, (error, zipfile) => {
      if (error) reject(error);
      else resolve(zipfile);
    });
  });
}

function readEntries(zipfile) {
  const names = [];
  const files = {};
  return new Promise((resolve, reject) => {
    zipfile.on('error', reject);
    zipfile.on('entry', (entry) => {
      names.push(entry.fileName);
      if (entry.fileName === 'extension.vsixmanifest' || entry.fileName === 'extension/package.json') {
        zipfile.openReadStream(entry, (error, stream) => {
          if (error) reject(error);
          else {
            const chunks = [];
            stream.on('data', (chunk) => chunks.push(chunk));
            stream.on('end', () => {
              files[entry.fileName] = Buffer.concat(chunks).toString('utf8');
              zipfile.readEntry();
            });
          }
        });
      } else {
        zipfile.readEntry();
      }
    });
    zipfile.on('end', () => resolve({ names, files }));
    zipfile.readEntry();
  });
}

async function main() {
  assert.ok(fs.existsSync(vsixPath), `missing VSIX: ${vsixPath}`);
  const zipfile = await openVsix(vsixPath);
  const { names, files } = await readEntries(zipfile);

  const expected = [
    'extension/package.json',
    'extension/language-configuration.json',
    'extension/syntaxes/lox.tmLanguage.json',
    'extension/out/src/extension.js',
    'extension.vsixmanifest'
  ];
  for (const name of expected) {
    assert.ok(names.includes(name), `VSIX must contain ${name}`);
  }

  for (const name of names) {
    assert.ok(!name.startsWith('extension/node_modules/'), `VSIX must not ship node_modules: ${name}`);
  }

  const manifest = files['extension.vsixmanifest'];
  assert.ok(manifest, 'missing extension.vsixmanifest');
  assert.ok(manifest.includes(`Version="${packageJson.version}"`), 'manifest version must match package.json');
  assert.ok(manifest.includes('Identity'), 'manifest must carry an Identity');

  assert.ok(packageJson.main.startsWith('./out/'), 'main must live under out/');
  assert.ok(packageJson.activationEvents.length === 0, 'activation must come from contributes');

  const packaged = JSON.parse(files['extension/package.json']);
  for (const key of ['name', 'version', 'publisher', 'main']) {
    assert.strictEqual(packaged[key], packageJson[key], `packaged ${key} must match the source package.json`);
  }
  assert.deepStrictEqual(
    packaged.contributes.languages,
    packageJson.contributes.languages,
    'packaged language contribution must match the source package.json'
  );
  assert.deepStrictEqual(
    packaged.contributes.grammars,
    packageJson.contributes.grammars,
    'packaged grammar contribution must match the source package.json'
  );

  console.log(`OK: ${path.basename(vsixPath)} has ${names.length} entries, all checks passed`);
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
