#!/usr/bin/env python3
"""Run a single AWFY Python benchmark via the Run harness.

Usage: python3 run_awfy_python.py <benchmark-file.py>

The AWFY Python benchmarks require the run.py harness and need the module
to be imported by class name (not filename). This script handles the mapping.
"""
import sys
import os

BENCH_DIR = '/benchmarks/awfy/Python'
sys.path.insert(0, BENCH_DIR)

# AWFY Python class names that differ from simple capitalisation
CLASS_MAP = {
    'nbody': 'NBody',
    'deltablue': 'DeltaBlue',
    'cd': 'CD',
}

bench_file = sys.argv[1]
module_name = os.path.basename(bench_file).replace('.py', '').lower()
class_name = CLASS_MAP.get(module_name, module_name.capitalize())

from run import Run  # noqa: E402 (path set above)
r = Run(class_name)
r.set_num_iterations(5)
r.run_benchmark()
