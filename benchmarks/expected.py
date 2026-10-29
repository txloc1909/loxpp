"""The checksum each benchmark program must print, pinned.

run.py compares every backend against these values, so a result that is wrong
on every backend fails the run. Each entry holds the value and where it came
from. The value of a program must come from the program's own self-check or
from a known answer. A value copied from what the backends print is not an
answer, so no entry may rest only on that.

Source kinds:
  published   a result published for the benchmark
  math        follows from arithmetic on the program's parameters
  self-check  the program checks its own result and prints the verdict
  reimpl      a second implementation, outside Lox++, gives the same value
  constant    the program returns a constant, so the value checks only that
              the run finished
"""
from __future__ import annotations

# program: (value as printed, compare as float?, source kind, source)
EXPECTED = {
    "binary_trees": ("2047", False, "math",
                     "complete tree of depth 10 has 2^11 - 1 nodes"),
    "bounce": ("1331", False, "published",
               "AWFY Bounce inner-iteration result"),
    "fannkuch": ("16", False, "published",
                 "CLBG fannkuch-redux maximum flips for n = 7"),
    "fasta": ("1000", False, "constant",
              "benchmark() returns n; the generated sequence is not checked"),
    "fib": ("317811", False, "math", "fib(28)"),
    "json": ("153", False, "reimpl",
             "Python json.loads of the same input string: 153 operations"),
    "k_nucleotide": ("2997", False, "math", "1000 + 999 + 998 k-mers"),
    "list": ("10", False, "published", "AWFY List result"),
    "mandelbrot": ("5739", False, "reimpl",
                   "Python version of the same loop, size 120"),
    "nbody": ("-0.16905274198936157", True, "reimpl",
              "Python version of the same loop, 12000 steps; the same code "
              "gives the published energies -0.169075164 at step 0 and "
              "-0.169087605 at step 1000 (CLBG), and with 250000 steps the "
              "published AWFY value -0.1690859889909308"),
    "permute": ("8660", False, "published", "AWFY Permute result"),
    "queens": ("true", False, "published", "AWFY Queens result"),
    "reverse_complement": ("1000", False, "math", "length of a 1000-base input"),
    "richards": ("true", False, "self-check",
                 "Scheduler.start() checks the canonical queue and hold counts"),
    "sieve": ("669", False, "math", "there are 669 primes below 5000"),
    "spectral_norm": ("1.274219991", True, "published",
                      "CLBG spectral-norm, n = 100"),
    "storage": ("5461", False, "published",
                "AWFY Storage result; also 1 + 4 + 16 + 64 + 256 + 1024 + 4096"),
    "towers": ("8191", False, "math", "2^13 - 1 moves for 13 disks"),
}

# The latency programs print LATENCY ... <checksum>; only the checksum is pinned.
LATENCY_EXPECTED = {
    "gc_latency": ("476005", False, "math",
                   "sum over seq 0..10999 of (101*seq + 5050), mod 1000003"),
}

# Relative tolerance for a value compared as a float. The published values
# carry 10 digits, and a backend may print a float with fewer or more.
FLOAT_REL_TOL = 1e-9
