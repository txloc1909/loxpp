#!/usr/bin/env python3
# run_stages.py — public-source test oracle for the Lox++ HTTP server.
#
# Every case is transcribed from codecrafters-io/build-your-own-http-server
# stage_descriptions/*.md (public, no membership needed). It drives the real
# challenge entrypoint, your_http.sh, over raw TCP so it can also decode the
# gzip body with Python's gzip module.
#
# The suite covers the twelve stages pure Lox++ can reach. base-08 (POST body)
# and persistent-02 (concurrent keep-alive) are unreachable without a bounded
# socket read and concurrency respectively; see ../README.md and ../tests/gaps.sh.
#
# Usage: tests/run_stages.py [your_http.sh]

import gzip
import os
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_PROG = os.path.join(HERE, "..", "your_http.sh")

PASS = 0
FAIL = 0


def check(name, cond, detail=""):
    global PASS, FAIL
    if cond:
        PASS += 1
        print("  ok   %s" % name)
    else:
        FAIL += 1
        print("  FAIL %s  %s" % (name, detail))


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def start_server(prog, directory, port):
    env = dict(os.environ, LOXPP_HTTP_PORT=str(port))
    log = tempfile.NamedTemporaryFile(prefix="loxpp-http-", suffix=".log", delete=False)
    proc = subprocess.Popen(
        [prog, "--directory", directory], env=env, stdout=log, stderr=subprocess.STDOUT
    )
    deadline = time.time() + 5
    while time.time() < deadline:
        if proc.poll() is not None:
            return proc, log.name, False
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
            return proc, log.name, True
        except OSError:
            time.sleep(0.05)
    return proc, log.name, False


def parse_response(sock):
    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = sock.recv(4096)
        if not chunk:
            break
        buf += chunk
    head, _, rest = buf.partition(b"\r\n\r\n")
    lines = head.split(b"\r\n")
    status = lines[0].decode("latin-1")
    headers = {}
    for line in lines[1:]:
        if b":" in line:
            k, _, v = line.partition(b":")
            headers[k.strip().lower().decode("latin-1")] = v.strip().decode("latin-1")
    length = headers.get("content-length")
    if length is not None:
        need = int(length)
        while len(rest) < need:
            chunk = sock.recv(4096)
            if not chunk:
                break
            rest += chunk
        body = rest[:need]
    else:
        body = rest
    return status, headers, body


def request(port, raw, half_close=False, keep=None):
    sock = keep or socket.create_connection(("127.0.0.1", port), timeout=5)
    sock.settimeout(5)
    sock.sendall(raw)
    if half_close:
        sock.shutdown(socket.SHUT_WR)
    result = parse_response(sock)
    if keep is None:
        sock.close()
    return result


def main():
    prog = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else DEFAULT_PROG)
    if not os.path.exists(prog):
        print("entrypoint not found: %s" % prog)
        return 1

    directory = tempfile.mkdtemp(prefix="loxpp-http-files-")
    with open(os.path.join(directory, "foo"), "wb") as f:
        f.write(b"Hello, World!")

    port = free_port()
    proc, log_path, up = start_server(prog, directory, port)

    try:
        check("base-01 bind to a port", up)
        if not up:
            print("server log:\n%s" % open(log_path).read())
            return 1

        status, headers, body = request(port, b"GET / HTTP/1.1\r\nHost: x\r\n\r\n")
        check("base-02 respond 200", status == "HTTP/1.1 200 OK", status)

        status, _, _ = request(port, b"GET /missing HTTP/1.1\r\nHost: x\r\n\r\n")
        check("base-03 respond 404", status == "HTTP/1.1 404 Not Found", status)

        status, headers, body = request(port, b"GET /echo/abc HTTP/1.1\r\nHost: x\r\n\r\n")
        check(
            "base-04 echo body",
            status == "HTTP/1.1 200 OK"
            and headers.get("content-type") == "text/plain"
            and headers.get("content-length") == "3"
            and body == b"abc",
            "%s %r %r" % (status, headers, body),
        )

        status, _, body = request(
            port, b"GET /user-agent HTTP/1.1\r\nUser-Agent: foobar/1.2.3\r\n\r\n"
        )
        check("base-05 user-agent", body == b"foobar/1.2.3", repr(body))
        status, _, body = request(
            port, b"GET /user-agent HTTP/1.1\r\nuser-agent: lowercase/1\r\n\r\n"
        )
        check("base-05 header name case-insensitive", body == b"lowercase/1", repr(body))

        # base-06: multiple concurrent connections. The documented nc clients
        # half-close after sending, so the serial server serves them in turn.
        socks = [socket.create_connection(("127.0.0.1", port), timeout=5) for _ in range(5)]
        for s in socks:
            s.settimeout(5)
            s.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n")
            s.shutdown(socket.SHUT_WR)
        results = [parse_response(s) for s in socks]
        for s in socks:
            s.close()
        check(
            "base-06 concurrent connections",
            all(r[0] == "HTTP/1.1 200 OK" for r in results),
            "; ".join(r[0] for r in results),
        )

        status, headers, body = request(port, b"GET /files/foo HTTP/1.1\r\n\r\n")
        check(
            "base-07 get file",
            status == "HTTP/1.1 200 OK"
            and headers.get("content-type") == "application/octet-stream"
            and headers.get("content-length") == "13"
            and body == b"Hello, World!",
            "%s %r" % (status, body),
        )
        status, _, _ = request(port, b"GET /files/nope HTTP/1.1\r\n\r\n")
        check("base-07 missing file 404", status == "HTTP/1.1 404 Not Found", status)

        status, headers, body = request(
            port, b"GET /echo/abc HTTP/1.1\r\nAccept-Encoding: gzip\r\n\r\n"
        )
        check(
            "compression-01 gzip header",
            headers.get("content-encoding") == "gzip",
            repr(headers),
        )
        status, headers, _ = request(
            port, b"GET /echo/abc HTTP/1.1\r\nAccept-Encoding: invalid-encoding\r\n\r\n"
        )
        check(
            "compression-01 no unsupported encoding",
            "content-encoding" not in headers,
            repr(headers),
        )
        status, headers, _ = request(
            port, b"GET /echo/abc HTTP/1.1\r\nAccept-Encoding:\tgzip\r\n\r\n"
        )
        check(
            "compression-01 HTAB around field value",
            headers.get("content-encoding") == "gzip",
            repr(headers),
        )

        status, headers, body = request(
            port,
            b"GET /echo/abc HTTP/1.1\r\nAccept-Encoding: invalid-encoding-1, gzip, invalid-encoding-2\r\n\r\n",
        )
        check(
            "compression-02 gzip among schemes",
            headers.get("content-encoding") == "gzip",
            repr(headers),
        )
        status, headers, _ = request(
            port,
            b"GET /echo/abc HTTP/1.1\r\nAccept-Encoding: invalid-encoding-1, invalid-encoding-2\r\n\r\n",
        )
        check(
            "compression-02 no unsupported scheme",
            "content-encoding" not in headers,
            repr(headers),
        )

        status, headers, body = request(
            port, b"GET /echo/abc HTTP/1.1\r\nAccept-Encoding: gzip\r\n\r\n"
        )
        try:
            decoded = gzip.decompress(body)
        except Exception as exc:  # noqa: BLE001 - report any decode failure
            decoded = b"<decode error %s>" % str(exc).encode()
        check(
            "compression-03 gzip round-trips",
            headers.get("content-encoding") == "gzip"
            and headers.get("content-length") == str(len(body))
            and decoded == b"abc",
            "len=%s decoded=%r" % (headers.get("content-length"), decoded),
        )

        # persistent-01: two sequential requests on one connection.
        sock = socket.create_connection(("127.0.0.1", port), timeout=5)
        sock.settimeout(5)
        sock.sendall(b"GET /echo/banana HTTP/1.1\r\n\r\n")
        first = parse_response(sock)
        sock.sendall(b"GET /user-agent HTTP/1.1\r\nUser-Agent: blueberry\r\n\r\n")
        second = parse_response(sock)
        sock.close()
        check(
            "persistent-01 keep-alive two requests",
            first[2] == b"banana" and second[2] == b"blueberry",
            "%r %r" % (first[2], second[2]),
        )

        # persistent-03: Connection: close closes the connection.
        sock = socket.create_connection(("127.0.0.1", port), timeout=5)
        sock.settimeout(5)
        sock.sendall(b"GET /echo/orange HTTP/1.1\r\n\r\n")
        status, headers, body = parse_response(sock)
        check(
            "persistent-03 first request stays open",
            body == b"orange" and "connection" not in headers,
            "%r %r" % (body, headers),
        )
        sock.sendall(b"GET / HTTP/1.1\r\nConnection: close\r\n\r\n")
        status, headers, body = parse_response(sock)
        closed = sock.recv(1) == b""
        sock.close()
        check(
            "persistent-03 Connection: close",
            headers.get("connection") == "close" and closed,
            repr(headers),
        )
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()

    print("\n%d passed, %d failed" % (PASS, FAIL))
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
