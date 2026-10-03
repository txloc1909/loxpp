#!/bin/bash
# gaps.sh — evidence for the two Codecrafters HTTP stages pure Lox++ cannot
# reach. This is deliberately NOT run in CI: it demonstrates a missing
# capability, not a passing behaviour.
#
#   1. base-08 POST /files — a Content-Length body needs a bounded socket
#      read. read() waits for EOF and readline() waits for '\n'; neither
#      terminates on a length-delimited body, so the request body is
#      unreachable from inside the language.
#
#   2. persistent-02 — two simultaneous keep-alive connections need a
#      concurrency primitive. One accept()/readline() blocks the whole VM, so
#      while connection A is open, connection B is never accepted.
#
# Usage: tests/gaps.sh [your_http.sh]
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
PROG="${1:-$HERE/../your_http.sh}"

exec python3 - "$PROG" <<'PY'
import os, socket, sys, tempfile, time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(sys.argv[1])), "tests"))
from run_stages import free_port, parse_response, start_server  # noqa: E402

prog = sys.argv[1]
directory = tempfile.mkdtemp(prefix="loxpp-http-gaps-")
port = free_port()
proc, log, up = start_server(prog, directory, port)
if not up:
    print("server did not start:\n" + open(log).read())
    sys.exit(2)

rc = 0
try:
    # --- gap 1: POST body -------------------------------------------------
    s = socket.create_connection(("127.0.0.1", port), timeout=5)
    s.settimeout(5)
    s.sendall(
        b"POST /files/newfile HTTP/1.1\r\n"
        b"Content-Type: application/octet-stream\r\n"
        b"Content-Length: 5\r\n\r\n12345"
    )
    status, _, _ = parse_response(s)
    s.close()
    created = os.path.exists(os.path.join(directory, "newfile"))
    print("gap 1 (base-08 POST body): response %r, file created: %s" % (status, created))
    if status.startswith("HTTP/1.1 201") or created:
        print("  UNEXPECTED: server served the POST body")
        rc = 1

    # --- gap 2: concurrent persistent connections -------------------------
    a = socket.create_connection(("127.0.0.1", port), timeout=5)
    a.settimeout(5)
    a.sendall(b"GET /echo/a HTTP/1.1\r\n\r\n")
    parse_response(a)
    b = socket.create_connection(("127.0.0.1", port), timeout=5)
    b.settimeout(2)
    b.sendall(b"GET /echo/b HTTP/1.1\r\n\r\n")
    t0 = time.time()
    try:
        b.recv(4096)
        print("gap 2 (persistent-02): connection B was served")
        rc = 1
    except socket.timeout:
        print(
            "gap 2 (persistent-02): connection B got no response after %.0fs "
            "while A stayed open" % (time.time() - t0)
        )
    a.close()
    b.close()
finally:
    proc.terminate()
    try:
        proc.wait(timeout=3)
    except Exception:
        proc.kill()

print("\ngaps reproduced" if rc == 0 else "\ngap assumptions did not hold")
sys.exit(rc)
PY
