const parser = require('@typescript-eslint/parser');
const plugin = require('@typescript-eslint/eslint-plugin');

module.exports = [
  {
    files: ['src/**/*.ts', 'test/**/*.ts'],
    languageOptions: { parser, parserOptions: { ecmaVersion: 2022, sourceType: 'module' } },
    plugins: { '@typescript-eslint': plugin },
    rules: {
      ...plugin.configs.recommended.rules,
      '@typescript-eslint/no-floating-promises': 'off'
    }
  },
  {
    files: ['test/**/*.cjs', 'scripts/**/*.cjs'],
    languageOptions: { ecmaVersion: 2022, sourceType: 'commonjs' },
    rules: { 'no-unused-vars': 'error', 'no-constant-condition': 'error' }
  }
];
