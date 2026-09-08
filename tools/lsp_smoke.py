#!/usr/bin/env python3
"""Smoke test for the loxpp-lsp language server.

Spawns the server, speaks LSP over stdio, and checks the core features:
initialize handshake, push diagnostics on a clean and a broken file, hover on
a stdlib name, document symbols, and go-to-definition on a local use.

Usage:
    python3 tools/lsp_smoke.py <path-to-loxpp-lsp> [--bad-file <path>]

--bad-file replaces the built-in broken source with the content of <path>.
Point it at a file with the syntax error removed and the "one diagnostic"
check fails on purpose -- that is checkpoint step 3.

Exit code 0 when every check passes, 1 otherwise. All progress goes to
stderr; stdout is left clean.
"""

import argparse
import json
import subprocess
import sys
import threading

CLEAN_SOURCE = """\
fun greet(name) {
    var msg = "hi " + name;
    return msg;
}

var who = "world";
print greet(who);
print str(123);
"""

BAD_SOURCE = "print 1 +;\n"

CLEAN_URI = "file:///smoke/clean.lox"
BAD_URI = "file:///smoke/bad.lox"


class LspClient:
    def __init__(self, argv):
        self.proc = subprocess.Popen(
            argv,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        self._next_id = 0
        self._buf = b""
        self._diagnostics = {}
        self._lock = threading.Lock()
        self._stderr_dump = []
        self._stderr_thread = threading.Thread(target=self._drain_stderr, daemon=True)
        self._stderr_thread.start()

    def _drain_stderr(self):
        for line in self.proc.stderr:
            self._stderr_dump.append(line.decode("utf-8", "replace").rstrip())

    def _write(self, message):
        body = json.dumps(message).encode("utf-8")
        header = f"Content-Length: {len(body)}\r\n\r\n".encode("ascii")
        self.proc.stdin.write(header + body)
        self.proc.stdin.flush()

    def _read_message(self, timeout=10.0):
        result = {}

        def worker():
            content_length = None
            while True:
                while b"\r\n\r\n" not in self._buf:
                    chunk = self.proc.stdout.read(1)
                    if not chunk:
                        result["message"] = None
                        return
                    self._buf += chunk
                head, _, rest = self._buf.partition(b"\r\n\r\n")
                for line in head.split(b"\r\n"):
                    if line.lower().startswith(b"content-length:"):
                        content_length = int(line.split(b":", 1)[1].strip())
                self._buf = rest
                while len(self._buf) < content_length:
                    chunk = self.proc.stdout.read(content_length - len(self._buf))
                    if not chunk:
                        result["message"] = None
                        return
                    self._buf += chunk
                body = self._buf[:content_length]
                self._buf = self._buf[content_length:]
                result["message"] = json.loads(body.decode("utf-8"))
                return

        thread = threading.Thread(target=worker, daemon=True)
        thread.start()
        thread.join(timeout)
        if thread.is_alive():
            raise TimeoutError("no LSP message within %.0fs" % timeout)
        return result["message"]

    def request(self, method, params, timeout=10.0):
        self._next_id += 1
        my_id = self._next_id
        self._write({"jsonrpc": "2.0", "id": my_id, "method": method, "params": params})
        while True:
            message = self._read_message(timeout)
            if message is None:
                raise EOFError("server closed the stream waiting for %s" % method)
            if message.get("id") == my_id:
                if "error" in message:
                    raise RuntimeError("%s failed: %s" % (method, message["error"]))
                return message.get("result")
            self._absorb_notification(message)

    def notify(self, method, params):
        self._write({"jsonrpc": "2.0", "method": method, "params": params})

    def _absorb_notification(self, message):
        if message.get("method") == "textDocument/publishDiagnostics":
            p = message["params"]
            with self._lock:
                self._diagnostics[p["uri"]] = p["diagnostics"]

    def pump_until_diagnostics(self, uri, timeout=10.0):
        with self._lock:
            if uri in self._diagnostics:
                return self._diagnostics[uri]
        while True:
            message = self._read_message(timeout)
            if message is None:
                raise EOFError("stream closed before diagnostics for %s" % uri)
            self._absorb_notification(message)
            with self._lock:
                if uri in self._diagnostics:
                    return self._diagnostics[uri]

    def shutdown(self):
        try:
            self.request("shutdown", None, timeout=5.0)
            self.notify("exit", None)
        except Exception:
            pass
        try:
            return self.proc.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            return -1

    def stderr_text(self):
        return "\n".join(self._stderr_dump)


FAILURES = []


def check(condition, message):
    status = "ok  " if condition else "FAIL"
    print("  [%s] %s" % (status, message), file=sys.stderr)
    if not condition:
        FAILURES.append(message)


def line_char(source, needle, occurrence=1):
    """0-based (line, character) of the nth occurrence of needle in source."""
    index = -1
    for _ in range(occurrence):
        index = source.index(needle, index + 1)
    prefix = source[:index]
    line = prefix.count("\n")
    character = index - (prefix.rfind("\n") + 1)
    return line, character


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("server")
    parser.add_argument("--bad-file")
    args = parser.parse_args()

    bad_source = BAD_SOURCE
    if args.bad_file:
        with open(args.bad_file, "r", encoding="utf-8") as handle:
            bad_source = handle.read()

    client = LspClient([args.server, "--stdio"])

    try:
        init = client.request(
            "initialize",
            {"processId": None, "rootUri": None, "capabilities": {}},
        )
        caps = init.get("capabilities", {})
        check(caps.get("positionEncoding") == "utf-16",
              "initialize advertises positionEncoding utf-16")
        check(caps.get("hoverProvider") is True, "initialize advertises hover")
        check("renameProvider" not in caps,
              "initialize does not advertise rename")
        check("documentFormattingProvider" not in caps,
              "initialize does not advertise formatting")
        client.notify("initialized", {})

        # -- clean file: no diagnostics --------------------------------
        client.notify("textDocument/didOpen", {"textDocument": {
            "uri": CLEAN_URI, "languageId": "lox", "version": 1,
            "text": CLEAN_SOURCE}})
        clean_diags = client.pump_until_diagnostics(CLEAN_URI)
        check(clean_diags == [], "clean file has no diagnostics")

        # -- broken file: exactly one diagnostic ---------------------
        client.notify("textDocument/didOpen", {"textDocument": {
            "uri": BAD_URI, "languageId": "lox", "version": 1,
            "text": bad_source}})
        bad_diags = client.pump_until_diagnostics(BAD_URI)
        check(len(bad_diags) == 1,
              "broken file has exactly one diagnostic (got %d)" % len(bad_diags))
        if len(bad_diags) == 1:
            d = bad_diags[0]
            check(d.get("severity") == 1, "diagnostic severity is Error")
            # '1 +;' -> "Expect expression." anchored at end of input, which
            # is the line after the single content line.
            start = d["range"]["start"]
            check(start["line"] == bad_source.count("\n")
                  and start["character"] == 0,
                  "diagnostic range is the end-of-input position (got %s)"
                  % start)

        # -- hover on a stdlib name ----------------------------------
        hl, hc = line_char(CLEAN_SOURCE, "str(123)")
        hover = client.request("textDocument/hover", {
            "textDocument": {"uri": CLEAN_URI},
            "position": {"line": hl, "character": hc + 1}})
        hover_text = ""
        if hover and isinstance(hover.get("contents"), dict):
            hover_text = hover["contents"].get("value", "")
        check("str(value)" in hover_text,
              "hover on 'str' shows its signature (got %r)" % hover_text[:60])

        # -- document symbols --------------------------------------
        symbols = client.request("textDocument/documentSymbol", {
            "textDocument": {"uri": CLEAN_URI}})
        names = sorted(s["name"] for s in (symbols or []))
        check("greet" in names and "who" in names,
              "documentSymbol lists top-level names (got %s)" % names)

        # -- go to definition on a local use ---------------------
        ul, uc = line_char(CLEAN_SOURCE, "name;")  # 'name' inside greet body
        definition = client.request("textDocument/definition", {
            "textDocument": {"uri": CLEAN_URI},
            "position": {"line": ul, "character": uc + 1}})
        if isinstance(definition, list):
            definition = definition[0] if definition else None
        dl, dc = line_char(CLEAN_SOURCE, "name)")  # the parameter declaration
        ok_def = (definition is not None
                  and definition["range"]["start"]["line"] == dl
                  and definition["range"]["start"]["character"] == dc)
        check(ok_def, "definition of 'name' points at the parameter (got %s)"
              % (definition["range"] if definition else None))

    finally:
        code = client.shutdown()
        if code not in (0, None):
            print("  [warn] server exit code %s" % code, file=sys.stderr)
        stderr_text = client.stderr_text()
        if stderr_text:
            print("--- server stderr ---", file=sys.stderr)
            print(stderr_text, file=sys.stderr)

    if FAILURES:
        print("\n%d check(s) failed:" % len(FAILURES), file=sys.stderr)
        for item in FAILURES:
            print("  - " + item, file=sys.stderr)
        return 1
    print("\nAll smoke checks passed.", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
