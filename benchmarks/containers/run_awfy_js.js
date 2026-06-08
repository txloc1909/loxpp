#!/usr/bin/env node
/**
 * Run a single AWFY JS benchmark via the Run harness.
 * Usage: node run_awfy_js.js <benchmark-file.js>
 *
 * The AWFY JS benchmarks are loaded by the run.js harness using PascalCase names.
 * This script maps filenames to the class names expected by run.js.
 */
const path = require('path');

const BENCH_DIR = '/benchmarks/awfy';

// Class name overrides for non-trivial cases
const CLASS_MAP = {
  nbody: 'NBody',
  deltablue: 'DeltaBlue',
  cd: 'CD',
  json: 'Json',
};

const file = process.argv[2];
const base = path.basename(file, '.js');
const name = CLASS_MAP[base] || (base.charAt(0).toUpperCase() + base.slice(1));

process.chdir(BENCH_DIR);
const { Run } = require(path.join(BENCH_DIR, 'run.js'));
const r = new Run(name);
r.numIterations = 5;
r.runBenchmark();
