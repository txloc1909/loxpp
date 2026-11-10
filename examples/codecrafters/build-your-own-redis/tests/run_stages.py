#!/usr/bin/env python3
# run_stages.py — public-source test oracle for the Lox++ Redis server.
#
# The stage cases are transcribed from codecrafters-io/build-your-own-redis
# stage_descriptions/*.md (public, no membership needed). Local regression
# cases are marked "local:". The suite drives the real challenge entrypoint,
# your_program.sh, over raw TCP with a small RESP client.
#
# Usage: tests/run_stages.py [your_program.sh]

import os
import random
import re
import shutil
import socket
import string
import struct
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_PROG = os.path.join(HERE, "..", "your_program.sh")

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


def rand_word(n=8):
    return "".join(random.choice(string.ascii_lowercase) for _ in range(n))


# ---------------------------------------------------------------------------
# Server process
# ---------------------------------------------------------------------------


class Server:
    def __init__(self, prog, args, cwd=None):
        self.port = free_port()
        self.log = tempfile.NamedTemporaryFile(
            prefix="loxpp-redis-", suffix=".log", delete=False)
        self.proc = subprocess.Popen(
            [prog, "--port", str(self.port)] + args,
            stdout=self.log, stderr=subprocess.STDOUT, cwd=cwd)
        self.up = self._wait_up()

    def _wait_up(self):
        deadline = time.time() + 5
        while time.time() < deadline:
            if self.proc.poll() is not None:
                return False
            try:
                socket.create_connection(
                    ("127.0.0.1", self.port), timeout=0.2).close()
                return True
            except OSError:
                time.sleep(0.05)
        return False

    def client(self):
        return Client(self.port)

    def log_text(self):
        with open(self.log.name, errors="replace") as f:
            return f.read()

    def stop(self):
        self.proc.terminate()
        try:
            self.proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()


# ---------------------------------------------------------------------------
# RESP client
# ---------------------------------------------------------------------------


def encode(*args):
    out = [b"*%d\r\n" % len(args)]
    for a in args:
        if isinstance(a, int):
            a = str(a)
        if isinstance(a, str):
            a = a.encode("latin-1")
        out.append(b"$%d\r\n%s\r\n" % (len(a), a))
    return b"".join(out)


TIMEOUT = "<no reply within the limit>"
CLOSED = "<connection closed>"
BAD_REPLY = "<bytes that are not RESP>"


class Error(str):
    """A RESP simple error, kept apart from a simple string."""


class Client:
    def __init__(self, port):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=5)
        self.sock.settimeout(5)
        self.buf = b""

    def close(self):
        self.sock.close()

    def send_raw(self, raw):
        self.sock.sendall(raw)

    def _fill(self):
        chunk = self.sock.recv(65536)
        if not chunk:
            raise EOFError("server closed the connection")
        self.buf += chunk

    def _line(self):
        while b"\r\n" not in self.buf:
            self._fill()
        line, _, self.buf = self.buf.partition(b"\r\n")
        return line

    def _exact(self, n):
        while len(self.buf) < n:
            self._fill()
        data, self.buf = self.buf[:n], self.buf[n:]
        return data

    def read(self):
        line = self._line()
        kind, rest = line[:1], line[1:].decode("latin-1")
        if kind == b"+":
            return rest
        if kind == b"-":
            return Error(rest)
        if kind == b":":
            return int(rest)
        if kind == b"$":
            n = int(rest)
            if n < 0:
                return None
            data = self._exact(n + 2)
            return data[:n].decode("latin-1")
        if kind == b"*":
            n = int(rest)
            if n < 0:
                return None
            return [self.read() for _ in range(n)]
        raise ValueError("bad RESP type byte: %r" % line)

    def call(self, *args):
        self.send_raw(encode(*args))
        return self.read()

    def try_read(self, timeout=2):
        """Reads one reply. Returns TIMEOUT when none comes within timeout
        seconds, and CLOSED when the server closes the connection, so a
        missing reply fails one named case instead of the whole group."""
        self.sock.settimeout(timeout)
        try:
            return self.read()
        except socket.timeout:
            return TIMEOUT
        except (EOFError, OSError):
            return CLOSED
        except ValueError:
            return BAD_REPLY
        finally:
            self.sock.settimeout(5)


# ---------------------------------------------------------------------------
# Stage groups. Each returns nothing; it records cases with check().
# ---------------------------------------------------------------------------


def stages_base(prog):
    srv = Server(prog, [])
    try:
        check("base-01 bind to a port", srv.up, srv.log_text())
        if not srv.up:
            return

        c = srv.client()
        check("base-02 respond to PING", c.call("PING") == "PONG")

        c.send_raw(encode("PING") + encode("PING") + encode("ping"))
        got = [c.read() for _ in range(3)]
        check("base-03 multiple PINGs on one connection",
              got == ["PONG"] * 3, repr(got))

        a, b = srv.client(), srv.client()
        a.send_raw(encode("PING"))
        b.send_raw(encode("PING"))
        ra, rb = a.read(), b.read()
        check("base-04 concurrent clients", ra == "PONG" and rb == "PONG",
              repr((ra, rb)))
        # A connection that stays open must not stop the others.
        d = srv.client()
        check("local: a third client while two stay open",
              d.call("PING") == "PONG")
        a.close()
        b.close()
        d.close()

        word = rand_word()
        check("base-05 ECHO", c.call("ECHO", word) == word)
        check("base-05 ECHO is case-insensitive",
              c.call("eChO", word) == word)

        key, val = rand_word(), rand_word()
        check("base-06 SET", c.call("SET", key, val) == "OK")
        check("base-06 GET", c.call("GET", key) == val)
        check("local: GET of a missing key is a null bulk string",
              c.call("GET", rand_word()) is None)

        key, val = rand_word(), rand_word()
        check("base-07 SET with PX",
              c.call("SET", key, val, "px", "100") == "OK")
        check("base-07 GET before expiry", c.call("GET", key) == val)
        time.sleep(0.2)
        check("base-07 GET after expiry", c.call("GET", key) is None)

        # A whole-second clock expires a 100 ms key only when a second
        # boundary falls in the 200 ms wait: one round in five. Five rounds
        # in a row catch it with a chance of 1 - 0.2**5.
        rounds = 0
        for _ in range(5):
            key = rand_word()
            c.call("SET", key, "v", "PX", "100")
            time.sleep(0.2)
            if c.call("GET", key) is None:
                rounds += 1
        check("local: PX 100 expires in each of 5 rounds", rounds == 5,
              "%d of 5" % rounds)

        key = rand_word()
        c.call("SET", key, "v", "PX", "1000")
        time.sleep(0.2)
        check("local: a 1000 ms key is alive at 200 ms",
              c.call("GET", key) == "v")

        binary = "".join(chr(i) for i in range(256)) + "\r\n"
        check("local: ECHO keeps every byte value",
              c.call("ECHO", binary) == binary)

        c.send_raw(b"PING\r\n")
        check("local: inline command", c.read() == "PONG")

        # A malformed request gets a protocol error, the server closes that
        # connection, and the server stays up for the other clients.
        bad = [
            ("bad array length", b"*abc\r\n"),
            ("array too long", b"*99999999\r\n"),
            ("not a bulk string", b"*1\r\n:5\r\n"),
            ("negative bulk length", b"*1\r\n$-1\r\n"),
            ("bulk length too large", b"*1\r\n$999999999\r\n"),
            ("no CRLF after the bulk", b"*1\r\n$4\r\nPINGxx"),
        ]
        for name, raw in bad:
            m = srv.client()
            m.send_raw(raw)
            try:
                got = m.read()
            except (OSError, EOFError) as e:
                got = e
            check("local: %s is a protocol error" % name,
                  isinstance(got, Error) and got.startswith("ERR Protocol error"),
                  repr(got))
            m.close()
            fresh = srv.client()
            try:
                alive = fresh.call("PING") == "PONG"
            except (OSError, EOFError):
                alive = False
            check("local: the server is up after %s" % name, alive,
                  srv.log_text()[-300:])
            fresh.close()
            if not alive:
                return

        # A command does not suspend. A long command name or inline line
        # must not stop the other clients for long.
        for name, raw in [
            ("a 256 KB command name", encode("A" * 262144)),
            # Just under the 64 KB inline limit, where splitWords still runs:
            # a split that grows a word byte by byte takes about 4.7 s here.
            ("a 65000-byte inline word", b"ECHO " + b"a" * 65000 + b"\r\n"),
        ]:
            m = srv.client()
            m.send_raw(raw)
            time.sleep(0.05)
            other = srv.client()
            start = time.time()
            other.send_raw(encode("PING"))
            got = other.try_read(10)
            took = time.time() - start
            check("local: %s does not stop other clients" % name,
                  got == "PONG" and took < 1.0, "%r after %.1f s" % (got, took))
            m.close()
            other.close()

        got = c.call("NOSUCHCOMMAND")
        check("local: unknown command is an error",
              isinstance(got, Error), repr(got))
        check("local: the connection survives an error",
              c.call("PING") == "PONG")
        c.close()
    finally:
        srv.stop()


# ---------------------------------------------------------------------------
# Fairness: one client must not stop the server for the other clients. The
# probes and their limits come from the referee ruling on PR #1125.
# ---------------------------------------------------------------------------


def rss_kib(pid):
    try:
        with open("/proc/%d/status" % pid) as f:
            for line in f:
                if line.startswith("VmRSS:"):
                    return int(line.split()[1])
    except OSError:
        pass
    return 0


def measure_while(srv, action):
    """Runs action() while a measurer opens a new connection every 20 ms and
    times one PING on it. Returns (worst seconds, wrong replies, peak RSS in
    KiB)."""
    stop = threading.Event()
    worst = [0.0]
    wrong = []
    peak = [rss_kib(srv.proc.pid)]

    def measurer():
        while not stop.is_set():
            start = time.time()
            try:
                m = socket.create_connection(("127.0.0.1", srv.port), timeout=15)
                m.sendall(encode("PING"))
                data = b""
                while not data.endswith(b"\r\n"):
                    chunk = m.recv(64)
                    if not chunk:
                        break
                    data += chunk
                m.close()
                if data != b"+PONG\r\n":
                    wrong.append(data)
            except OSError as e:
                wrong.append(repr(e))
            worst[0] = max(worst[0], time.time() - start)
            peak[0] = max(peak[0], rss_kib(srv.proc.pid))
            time.sleep(0.02)

    th = threading.Thread(target=measurer)
    th.start()
    try:
        action()
    finally:
        stop.set()
        th.join(30)
    print("       measured: worst PING %.3f s, peak RSS %d KiB"
          % (worst[0], peak[0]))
    return worst[0], wrong, peak[0]


def flood(port, seconds, read_replies, chunk=None):
    """One client that sends pipelines (of PING, unless chunk says what) for
    the given time. It reads and drops the replies, or never reads them."""
    sock = socket.create_connection(("127.0.0.1", port), timeout=5)
    sock.settimeout(0.2)
    if chunk is None:
        chunk = encode("PING") * 1000
    deadline = time.time() + seconds
    done = threading.Event()

    def drain():
        sock.settimeout(0.2)
        while not done.is_set():
            try:
                if not sock.recv(1 << 16):
                    return
            except socket.timeout:
                continue
            except OSError:
                return

    if read_replies:
        threading.Thread(target=drain, daemon=True).start()
    off = 0
    while time.time() < deadline:
        try:
            off += sock.send(chunk[off:])
            if off == len(chunk):
                off = 0
        except socket.timeout:
            continue
        except OSError:
            break
    done.set()
    sock.close()


def flooders(port, count, seconds, read_replies, chunk=None):
    def run():
        ths = [threading.Thread(target=flood, args=(port, seconds, read_replies, chunk))
               for _ in range(count)]
        for t in ths:
            t.start()
        for t in ths:
            t.join()
    return run


def send_all(port, raw):
    def run():
        sock = socket.create_connection(("127.0.0.1", port), timeout=30)
        try:
            sock.sendall(raw)
            sock.settimeout(30)
            sock.recv(256)
        except OSError:
            pass
        sock.close()
    return run


def parallel(actions):
    def run():
        ths = [threading.Thread(target=a) for a in actions]
        for t in ths:
            t.start()
        for t in ths:
            t.join()
    return run


def send_until_stalled(port, chunk, most, quiet, seconds):
    """Sends chunk again and again, without reading, until the server has
    taken `most` bytes, or takes nothing for `quiet` seconds, or `seconds`
    pass. Returns the count of bytes taken."""
    sock = socket.create_connection(("127.0.0.1", port), timeout=5)
    sock.settimeout(0.2)
    sent = off = 0
    deadline = time.time() + seconds
    last = time.time()
    while sent < most and time.time() < deadline and time.time() - last < quiet:
        try:
            n = sock.send(chunk[off:])
        except socket.timeout:
            continue
        except OSError:
            break
        sent += n
        off = (off + n) % len(chunk)
        last = time.time()
    sock.close()
    return sent


def stages_query_limit(prog):
    # A small limit, so that the probe sends little: Redis's default is 1 GB.
    srv = Server(prog, ["--client-query-buffer-limit", "1048576"])
    try:
        check("query limit: server starts", srv.up, srv.log_text())
        if not srv.up:
            return
        c = srv.client()
        c.send_raw(encode("ECHO", "a" * 600000) + b"*3\r\n$4\r\nECHO\r\n")
        check("local: a command under the query limit runs",
              c.try_read(5) == "a" * 600000)
        c.send_raw(b"$600000\r\n" + b"a" * 600000 + b"\r\n" + b"$600000\r\n")
        check("local: a command over the query limit closes the client",
              c.try_read(5) is CLOSED)
        c.close()
        d = srv.client()
        check("local: the server serves after the query limit",
              d.call("PING") == "PONG")
        check("local: the query limit is not printed", srv.log_text() == "",
              srv.log_text()[:200])
        d.close()
    finally:
        srv.stop()
    # An inline command also counts against the limit. Only a limit under
    # the inline limit of 64 KB can show it.
    srv = Server(prog, ["--client-query-buffer-limit", "100"])
    try:
        check("query limit 100: server starts", srv.up, srv.log_text())
        if not srv.up:
            return
        c = srv.client()
        c.send_raw(b"ECHO " + b"a" * 200 + b"\r\n")
        check("local: an inline command over the query limit closes the client",
              c.try_read(5) is CLOSED)
        c.close()
        d = srv.client()
        check("local: an inline command under the query limit runs",
              d.call("ECHO", "short") == "short")
        d.close()
    finally:
        srv.stop()


def stages_fairness(prog):
    srv = Server(prog, [])
    try:
        check("fairness: server starts", srv.up, srv.log_text())
        if not srv.up:
            return

        worst, wrong, _ = measure_while(srv, flooders(srv.port, 4, 5, True))
        check("local: P1 4 pipelining clients that read, PING within 0.5 s",
              worst <= 0.5 and not wrong, "worst %.3f s, wrong %r" % (worst, wrong[:3]))

        worst, wrong, peak = measure_while(srv, flooders(srv.port, 4, 5, False))
        check("local: P2 4 pipelining clients that do not read, PING within 0.5 s",
              worst <= 0.5 and not wrong, "worst %.3f s, wrong %r" % (worst, wrong[:3]))
        check("local: P2 server RSS stays at most 64 MB",
              peak <= 64 * 1024, "peak %d KiB" % peak)

        # The bytes, not the time, decide this check, so a slow host does
        # not hide a reader that has no credit bound: such a reader takes
        # every byte that the client sends.
        accepted = send_until_stalled(srv.port, encode("PING") * 1000,
                                      32 << 20, 2.0, 30.0)
        check("local: P2 a client that does not read can send at most 32 MB",
              accepted < 32 << 20, "accepted %d bytes" % accepted)

        c = srv.client()
        c.send_raw(b"PING " + b"a" * 65532 + b"\r\n")
        got = c.try_read(5)
        check("local: P3 an inline line of 65537 bytes is a protocol error",
              isinstance(got, Error) and "too big inline request" in got, repr(got)[:100])
        check("local: P3 the server closes that connection", c.try_read(2) is CLOSED)
        c.close()

        worst, wrong, _ = measure_while(
            srv, send_all(srv.port, b"PING " + b"a" * (64 << 20) + b"\r\n"))
        check("local: P3 a 64 MB inline line stalls PING at most 1.0 s",
              worst <= 1.0 and not wrong, "worst %.3f s" % worst)

        worst, wrong, _ = measure_while(
            srv, send_all(srv.port, b"*1048576\r\n" + b"$0\r\n\r\n" * 1048576))
        check("local: P4 an array of 1048576 empty bulks stalls PING at most 1.0 s",
              worst <= 1.0 and not wrong, "worst %.3f s" % worst)
        # The ruling's limit is 1.0 s. Four such arrays at the same time hold
        # the yields of toArgs and fromArgs one by one: with both yields the
        # stall is about 0.05-0.07 s, with one of them removed 0.38-0.68 s.
        worst, wrong, _ = measure_while(srv, parallel(
            [send_all(srv.port, b"*1048576\r\n" + b"$0\r\n\r\n" * 1048576)] * 4))
        check("local: P4 four such arrays at once, the arguments are handed over in steps (at most 0.2 s)",
              worst <= 0.2 and not wrong, "worst %.3f s" % worst)

        worst, wrong, _ = measure_while(
            srv, send_all(srv.port, b"PING " + b"a" * 65529 + b"\r\n"))
        check("local: P5 an inline line at the 65536-byte limit stalls PING at most 0.5 s",
              worst <= 0.5 and not wrong, "worst %.3f s" % worst)

        worst, wrong, _ = measure_while(
            srv, flooders(srv.port, 4, 3, False, b"\r\n" * 65536))
        check("local: P6 4 clients that send only empty lines, PING within 0.5 s",
              worst <= 0.5 and not wrong, "worst %.3f s" % worst)

        # P7: a reply of 2097146 elements. The session writes it in steps
        # and yields between them, so the other clients go on. Without the
        # yield the stall is about 1.1 s.
        n = 1048573
        c = srv.client()
        c.send_raw(encode("RPUSH", "p7", *(["abcd"] * n)))
        first = c.try_read(30)
        c.send_raw(encode("RPUSH", "p7", *(["abcd"] * n)))
        second = c.try_read(30)
        check("local: P7 two RPUSH of 1048573 elements",
              first == n and second == 2 * n, "%r %r" % (first, second))
        expected = b"*%d\r\n" % (2 * n) + b"$4\r\nabcd\r\n" * (2 * n)
        got = [b""]

        def lrange_all():
            c.sock.settimeout(30)
            c.sock.sendall(encode("LRANGE", "p7", "0", "-1"))
            parts, size = [], 0
            while size < len(expected):
                try:
                    chunk = c.sock.recv(1 << 20)
                except OSError:
                    break
                if not chunk:
                    break
                parts.append(chunk)
                size += len(chunk)
            got[0] = b"".join(parts)
        worst, wrong, _ = measure_while(srv, lrange_all)
        check("local: P7 LRANGE of 2097146 elements gives every element",
              got[0] == expected, "got %d bytes of %d" % (len(got[0]), len(expected)))
        check("local: P7 LRANGE of 2097146 elements stalls PING at most 0.2 s",
              worst <= 0.2 and not wrong, "worst %.3f s" % worst)

        # P8: the first LPUSH on a long list that RPUSH built. A push that
        # copies the list stalls about 0.36 s at 4194292 elements.
        c.send_raw(encode("RPUSH", "p7", *(["abcd"] * n)))
        third = c.try_read(30)
        c.send_raw(encode("RPUSH", "p7", *(["abcd"] * n)))
        fourth = c.try_read(30)
        pushed = [None]

        def lpush_one():
            time.sleep(0.1)
            c.send_raw(encode("LPUSH", "p7", "head"))
            pushed[0] = c.try_read(30)
            time.sleep(0.1)
        worst, wrong, _ = measure_while(srv, lpush_one)
        c.send_raw(encode("LRANGE", "p7", "0", "1"))
        top = c.try_read(30)
        check("local: P8 LPUSH on a list of 4194292 elements gives the new length",
              third == 3 * n and fourth == 4 * n and pushed[0] == 4 * n + 1
              and top == ["head", "abcd"],
              "%r %r %r %r" % (third, fourth, pushed[0], top))
        check("local: P8 LPUSH on a list of 4194292 elements stalls PING at most 0.1 s",
              worst <= 0.1 and not wrong, "worst %.3f s" % worst)

        # P9: BLPOP on 1048574 keys, the most that one command can name.
        # Joining the queues of all keys is one step, as in Redis
        # (redis-server 7.0 stalls about 0.6 s). Leaving them yields between
        # steps.
        keys = ["p9-%d" % i for i in range(1048574)]
        many = srv.client()

        def blpop_many():
            many.send_raw(encode("BLPOP", *keys, "0"))
            time.sleep(5)
        worst, wrong, _ = measure_while(srv, blpop_many)
        check("local: P9 BLPOP on 1048574 keys stalls PING at most 1 s",
              worst <= 1.0 and not wrong, "worst %.3f s" % worst)
        served = [None]

        def serve_many():
            c.send_raw(encode("RPUSH", keys[-1], "v"))
            c.try_read(30)
            served[0] = many.try_read(30)
            time.sleep(0.3)
        worst, wrong, _ = measure_while(srv, serve_many)
        check("local: P9 an RPUSH serves it", served[0] == [keys[-1], "v"],
              "%r" % (served[0],))
        check("local: P9 it leaves the other 1048573 queues and PING waits at most 0.2 s",
              worst <= 0.2 and not wrong, "worst %.3f s" % worst)
        many.close()
        c.close()

        # 1000 clients that send more than a window of commands and reset
        # the connection without reading. Each connection's tasks must end:
        # the session closes its socket, so the open files of the server go
        # back to the count before.
        def open_fds():
            try:
                return len(os.listdir("/proc/%d/fd" % srv.proc.pid))
            except OSError:
                return -1
        before = open_fds()
        burst = encode("PING") * 200
        for _ in range(1000):
            r = socket.create_connection(("127.0.0.1", srv.port), timeout=5)
            r.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                         struct.pack("ii", 1, 0))
            try:
                r.sendall(burst)
            except OSError:
                pass
            r.close()
        deadline = time.time() + 10
        while time.time() < deadline and open_fds() > before + 5:
            time.sleep(0.1)
        after = open_fds()
        check("local: 1000 reset connections leave no open socket",
              0 <= after <= before + 5, "%d open files before, %d after"
              % (before, after))
        # A connection counts in connected_clients until both of its tasks
        # end, so a task that outlives its connection keeps it above 1.
        c = srv.client()
        deadline = time.time() + 10
        clients = None
        while time.time() < deadline:
            info = c.call("INFO", "clients")
            clients = [l for l in info.split("\r\n")
                       if l.startswith("connected_clients:")]
            if clients == ["connected_clients:1"]:
                break
            time.sleep(0.1)
        check("local: 1000 reset connections leave no task",
              clients == ["connected_clients:1"], repr(clients))
        c.close()

        c = srv.client()
        check("local: the server serves after the fairness probes",
              c.call("PING") == "PONG")
        c.close()
    finally:
        srv.stop()


def stages_lists(prog):
    srv = Server(prog, [])
    try:
        check("lists: server starts", srv.up, srv.log_text())
        if not srv.up:
            return
        c = srv.client()

        key = rand_word()
        check("lists-01 RPUSH creates a list", c.call("RPUSH", key, "element") == 1)

        key = rand_word()
        check("lists-02 RPUSH appends (1)", c.call("RPUSH", key, "element1") == 1)
        check("lists-02 RPUSH appends (2)", c.call("RPUSH", key, "element2") == 2)

        key = rand_word()
        check("lists-03 RPUSH many (3)", c.call("RPUSH", key, "e1", "e2", "e3") == 3)
        check("lists-03 RPUSH many (5)", c.call("RPUSH", key, "e4", "e5") == 5)
        check("local: RPUSH keeps the order",
              c.call("LRANGE", key, "0", "-1") == ["e1", "e2", "e3", "e4", "e5"])

        key = rand_word()
        c.call("RPUSH", key, "a", "b", "c", "d", "e")
        check("lists-04 LRANGE 0 2", c.call("LRANGE", key, "0", "2") == ["a", "b", "c"])
        check("lists-04 LRANGE 1 3", c.call("LRANGE", key, "1", "3") == ["b", "c", "d"])
        check("lists-04 LRANGE stop past the end",
              c.call("LRANGE", key, "2", "10") == ["c", "d", "e"])
        check("lists-04 LRANGE start past the end",
              c.call("LRANGE", key, "7", "9") == [])
        check("lists-04 LRANGE start > stop", c.call("LRANGE", key, "3", "1") == [])
        check("lists-04 LRANGE missing list",
              c.call("LRANGE", rand_word(), "0", "1") == [])

        check("lists-05 LRANGE 2 -1", c.call("LRANGE", key, "2", "-1") == ["c", "d", "e"])
        check("lists-05 LRANGE -2 -1", c.call("LRANGE", key, "-2", "-1") == ["d", "e"])
        check("lists-05 LRANGE -9 1 (clamped)",
              c.call("LRANGE", key, "-9", "1") == ["a", "b"])
        check("lists-05 LRANGE 0 -9", c.call("LRANGE", key, "0", "-9") == [])

        key = rand_word()
        check("lists-06 LPUSH (1)", c.call("LPUSH", key, "c") == 1)
        check("lists-06 LPUSH (3)", c.call("LPUSH", key, "b", "a") == 3)
        check("lists-06 LPUSH order",
              c.call("LRANGE", key, "0", "-1") == ["a", "b", "c"])

        key = rand_word()
        n = random.randint(3, 9)
        c.call("RPUSH", key, *[rand_word(3) for _ in range(n)])
        check("lists-07 LLEN", c.call("LLEN", key) == n)
        check("lists-07 LLEN missing list", c.call("LLEN", rand_word()) == 0)

        key = rand_word()
        c.call("RPUSH", key, "one", "two", "three", "four", "five")
        check("lists-08 LPOP", c.call("LPOP", key) == "one")
        check("lists-08 LRANGE after LPOP",
              c.call("LRANGE", key, "0", "-1") == ["two", "three", "four", "five"])
        check("local: LPOP missing list is a null bulk string",
              c.call("LPOP", rand_word()) is None)

        key = rand_word()
        c.call("RPUSH", key, "one", "two", "three", "four", "five")
        check("lists-09 LPOP 2", c.call("LPOP", key, "2") == ["one", "two"])
        check("lists-09 LRANGE after LPOP 2",
              c.call("LRANGE", key, "0", "-1") == ["three", "four", "five"])
        check("local: LPOP count past the end",
              c.call("LPOP", key, "9") == ["three", "four", "five"])
        check("local: the empty list is removed", c.call("LLEN", key) == 0)
        check("local: LPOP on the emptied list is a null bulk string",
              c.call("LPOP", key) is None)
        c.call("SET", key, "now-a-string")
        check("local: the emptied key can hold a string",
              c.call("GET", key) == "now-a-string")

        # lists-10: BLPOP with no limit, served when another client pushes.
        key = rand_word()
        blocked = srv.client()
        blocked.send_raw(encode("BLPOP", key, "0"))
        time.sleep(0.2)
        pusher = srv.client()
        check("lists-10 RPUSH to a waited list replies 1",
              pusher.call("RPUSH", key, "foo") == 1)
        check("lists-10 BLPOP gets the element", blocked.try_read() == [key, "foo"])
        check("local: the served element left the list", c.call("LLEN", key) == 0)

        # lists-10: the client that waited first is served first.
        key = rand_word()
        first, second = srv.client(), srv.client()
        first.send_raw(encode("BLPOP", key, "0"))
        time.sleep(0.1)
        second.send_raw(encode("BLPOP", key, "0"))
        time.sleep(0.1)
        pusher.call("RPUSH", key, "foo")
        check("lists-10 the first waiter is served", first.try_read() == [key, "foo"])
        got = second.try_read(0.3)
        check("lists-10 the second waiter still waits", got is TIMEOUT, repr(got))
        pusher.call("RPUSH", key, "bar")
        check("local: the second waiter gets the next push",
              second.try_read() == [key, "bar"])

        # lists-11: BLPOP with a limit.
        key = rand_word()
        start = time.time()
        blocked.send_raw(encode("BLPOP", key, "0.1"))
        got = blocked.try_read(2)
        took = time.time() - start
        check("lists-11 BLPOP times out with a null array", got is None, repr(got))
        check("lists-11 the wait lasts about the limit", 0.09 <= took < 1.0,
              "%.3f s" % took)
        key = rand_word()
        blocked.send_raw(encode("BLPOP", key, "0.5"))
        time.sleep(0.1)
        pusher.call("RPUSH", key, "foo")
        check("lists-11 BLPOP before the limit gets the element",
              blocked.try_read() == [key, "foo"])

        # local: a waiter that timed out is not served later.
        key = rand_word()
        check("local: timed-out BLPOP", blocked.call("BLPOP", key, "0.05") is None)
        pusher.call("RPUSH", key, "kept")
        check("local: a timed-out waiter takes no element",
              c.call("LRANGE", key, "0", "-1") == ["kept"])

        # local: a waiter that timed out is skipped while another waiter of
        # the same key still waits.
        key = rand_word()
        blocked.send_raw(encode("BLPOP", key, "0.05"))
        time.sleep(0.02)
        second.send_raw(encode("BLPOP", key, "0"))
        check("local: the first waiter times out", blocked.try_read() is None)
        pusher.call("RPUSH", key, "x")
        check("local: the push skips the timed-out waiter",
              second.try_read() == [key, "x"])

        # local: a waiter whose client goes away takes no element.
        key = rand_word()
        gone = srv.client()
        gone.send_raw(encode("BLPOP", key, "0"))
        time.sleep(0.1)
        gone.close()
        time.sleep(0.1)
        pusher.call("RPUSH", key, "kept")
        check("local: a closed waiter takes no element",
              c.call("LRANGE", key, "0", "-1") == ["kept"])

        # local: commands sent after BLPOP wait until it is served.
        key = rand_word()
        blocked.send_raw(encode("BLPOP", key, "0") + encode("PING"))
        time.sleep(0.1)
        pusher.call("RPUSH", key, "x")
        check("local: BLPOP reply comes before the next reply",
              blocked.try_read() == [key, "x"] and blocked.try_read() == "PONG")

        # local: BLPOP on many keys takes the first one with an element.
        k1, k2 = rand_word(), rand_word()
        c.call("RPUSH", k2, "v2")
        check("local: BLPOP on two keys", c.call("BLPOP", k1, k2, "0") == [k2, "v2"])

        key = rand_word()
        c.call("SET", key, "s")
        got = c.call("LPUSH", key, "x")
        check("local: LPUSH on a string key is WRONGTYPE",
              isinstance(got, Error) and got.startswith("WRONGTYPE"), repr(got))
        c.call("RPUSH", "L" + key, "x")
        # A reply that does not come fails this case by name, and the
        # cases after it go on with a new connection.
        try:
            got = c.call("GET", "L" + key)
        except (OSError, EOFError, ValueError):
            got = CLOSED
        check("local: GET on a list key is WRONGTYPE",
              isinstance(got, Error) and got.startswith("WRONGTYPE"), repr(got))
        if got is CLOSED:
            c = srv.client()

        # Bug class of #1123: the work of a list command stays linear, so a
        # long list does not stop the other clients.
        n = 100000
        for name, build in [
            ("RPUSH of 100000 elements", lambda k: c.send_raw(encode("RPUSH", k, *["x"] * n))),
            ("LPUSH of 100000 elements", lambda k: c.send_raw(encode("LPUSH", k, *["x"] * n))),
        ]:
            key = rand_word()
            start = time.time()
            build(key)
            got = c.try_read(30)
            check("local: %s is fast" % name,
                  got == n and time.time() - start < 5,
                  "%r after %.1f s" % (got, time.time() - start))
            start = time.time()
            c.send_raw(encode("LRANGE", key, "0", "-1"))
            got = c.try_read(30)
            check("local: LRANGE of %s is fast" % name,
                  isinstance(got, list) and len(got) == n and time.time() - start < 5,
                  "%.1f s" % (time.time() - start))
            start = time.time()
            c.send_raw(encode("LPOP", key, str(n)))
            got = c.try_read(30)
            check("local: LPOP %d after %s is fast" % (n, name),
                  isinstance(got, list) and len(got) == n and time.time() - start < 5,
                  "%.1f s" % (time.time() - start))
        key = rand_word()
        start = time.time()
        for i in range(2000):
            c.send_raw(encode("LPUSH", key, "x"))
        for i in range(2000):
            c.read()
        for i in range(2000):
            c.send_raw(encode("LPOP", key))
        for i in range(2000):
            c.read()
        check("local: 2000 single LPUSH then 2000 LPOP are fast",
              time.time() - start < 5 and c.call("LLEN", key) == 0,
              "%.1f s" % (time.time() - start))

        # Many waits that time out on one key do not grow its queue without
        # bound: the next push still serves the next live waiter.
        key = rand_word()
        for _ in range(50):
            blocked.call("BLPOP", key, "0.01")
        blocked.send_raw(encode("BLPOP", key, "0"))
        time.sleep(0.1)
        pusher.call("RPUSH", key, "after-timeouts")
        check("local: a waiter after 50 time-outs is served",
              blocked.try_read() == [key, "after-timeouts"])

        # A waiter on two keys, served by one, is counted out of the other.
        k1, k2 = rand_word(), rand_word()
        blocked.send_raw(encode("BLPOP", k1, k2, k1, "0"))
        time.sleep(0.1)
        pusher.call("RPUSH", k2, "v")
        check("local: BLPOP on k1 k2 k1 served by k2",
              blocked.try_read() == [k2, "v"])
        pusher.call("RPUSH", k1, "stays")
        check("local: no waiter remains on k1",
              c.call("LRANGE", k1, "0", "-1") == ["stays"])

        # While a session waits in BLPOP, its reader reads on past its
        # credit, so that it sees a client that goes.
        def clients_now():
            info = c.call("INFO", "clients")
            for line in info.split("\r\n"):
                if line.startswith("connected_clients:"):
                    return int(line.split(":")[1])
            return -1

        def clients_back_to(count):
            deadline = time.time() + 5
            now = clients_now()
            while time.time() < deadline and now != count:
                time.sleep(0.05)
                now = clients_now()
            return now

        base = clients_now()
        key = rand_word()
        gone = srv.client()
        gone.send_raw(encode("BLPOP", key, "0") + encode("PING") * 100)
        time.sleep(0.2)
        gone.close()
        now = clients_back_to(base)
        check("local: BLPOP with no limit ends when its client goes after 100 more commands",
              now == base, "connected_clients %d, before %d" % (now, base))

        # The same, when the reader already waits for credit as the session
        # starts to wait: the session yields while it takes the 1100 keys
        # of this BLPOP out of its message, and the reader has used its
        # credit by then.
        gone = srv.client()
        gone.send_raw(encode("BLPOP", *[rand_word() for _ in range(1100)], "0")
                      + encode("PING") * 100)
        time.sleep(0.3)
        gone.close()
        now = clients_back_to(base)
        check("local: BLPOP on 1100 keys with no limit ends when its client goes after 100 more commands",
              now == base, "connected_clients %d, before %d" % (now, base))

        key = rand_word()
        flood = srv.client()
        flood.send_raw(encode("BLPOP", key, "0") + encode("PING") * 2000)
        check("local: a client that sends 2000 commands during one BLPOP is closed",
              flood.try_read(5) is CLOSED)
        flood.close()
        now = clients_back_to(base)
        check("local: that client's tasks end",
              now == base, "connected_clients %d, before %d" % (now, base))

        key = rand_word()
        waiting = srv.client()
        waiting.send_raw(encode("BLPOP", key, "5") + encode("PING") * 900)
        time.sleep(0.2)
        pusher.call("RPUSH", key, "v")
        got = waiting.try_read(5)
        pongs = 0
        while pongs < 900 and waiting.try_read(5) == "PONG":
            pongs += 1
        check("local: 900 commands sent during BLPOP run after it, in order",
              got == [key, "v"] and pongs == 900, "%r, %d PONG" % (got, pongs))
        waiting.close()

        # A waiter that resets its connection just before a push, and a
        # second waiter behind it. The pusher can serve the first after its
        # reader saw the reset; the element then goes back to the list, and
        # to the second waiter.
        rounds, kept, passed_on = 300, 0, 0
        keys = ["gone-%s-%d" % (rand_word(), i) for i in range(rounds)]
        for n_round, key in enumerate(keys):
            w = socket.create_connection(("127.0.0.1", srv.port))
            w.sendall(encode("BLPOP", key, "0"))
            nxt = None
            if n_round % 2 == 1:
                nxt = srv.client()
                nxt.send_raw(encode("BLPOP", key, "0"))
            time.sleep(0.005)
            w.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
            w.close()
            pusher.call("RPUSH", key, "v")
            if nxt is not None:
                if nxt.try_read(1) == [key, "v"]:
                    passed_on += 1
                nxt.close()
        time.sleep(0.3)
        for n_round, key in enumerate(keys):
            if n_round % 2 == 0 and pusher.call("LLEN", key) == 1:
                kept += 1
        check("local: an element pushed as its waiter resets stays in the list (150 rounds)",
              kept == rounds // 2, "%d of %d kept" % (kept, rounds // 2))
        check("local: an element pushed as its waiter resets goes to the next waiter (150 rounds)",
              passed_on == rounds // 2, "%d of %d passed on" % (passed_on, rounds // 2))

        # P10: LPOP of a little less than half of a list of 8388584
        # elements that LPUSH built. A pop that removes the elements one by
        # one stalls about 0.2 s here.
        big = srv.client()
        n = 1048573
        raw = encode("LPUSH", "p10", *(["abcd"] * n))
        for _ in range(8):
            big.send_raw(raw)
            big.try_read(60)
        half = 4 * n - 1
        popped = [None]

        def lpop_half():
            big.send_raw(encode("LPOP", "p10", str(half)))
            popped[0] = big.try_read(60)
            time.sleep(0.2)
        worst, wrong, _ = measure_while(srv, lpop_half)
        big.send_raw(encode("LLEN", "p10"))
        left = big.try_read(30)
        check("local: P10 LPOP of 4194291 of 8388584 elements gives them",
              isinstance(popped[0], list) and len(popped[0]) == half
              and left == 8 * n - half, "%r" % (left,))
        check("local: P10 LPOP of 4194291 of 8388584 elements stalls PING at most 0.14 s",
              worst <= 0.14 and not wrong, "worst %.3f s" % worst)
        big.close()

        for cl in (c, blocked, pusher, first, second):
            cl.close()
    finally:
        srv.stop()


def stages_streams(prog):
    srv = Server(prog, [])
    try:
        check("streams: server starts", srv.up, srv.log_text())
        if not srv.up:
            return
        c = Asker(srv)

        key = rand_word()
        c.call("SET", key, "value")
        check("streams-01 TYPE of a string", c.call("TYPE", key) == "string")
        check("streams-01 TYPE of a missing key", c.call("TYPE", rand_word()) == "none")
        lkey = rand_word()
        c.call("RPUSH", lkey, "x")
        check("local: TYPE of a list", c.call("TYPE", lkey) == "list")

        key = rand_word()
        check("streams-02 XADD gives the ID", c.call("XADD", key, "0-1", "foo", "bar") == "0-1")
        check("streams-02 TYPE of a stream", c.call("TYPE", key) == "stream")

        key = rand_word()
        smaller = "ERR The ID specified in XADD is equal or smaller than the target stream top item"
        check("streams-03 XADD 1-1", c.call("XADD", key, "1-1", "foo", "bar") == "1-1")
        check("streams-03 XADD 1-2", c.call("XADD", key, "1-2", "bar", "baz") == "1-2")
        got = c.call("XADD", key, "1-2", "baz", "foo")
        check("streams-03 XADD of the top ID is an error",
              isinstance(got, Error) and got == smaller, repr(got))
        got = c.call("XADD", key, "0-3", "baz", "foo")
        check("streams-03 XADD below the top is an error",
              isinstance(got, Error) and got == smaller, repr(got))
        got = c.call("XADD", key, "0-0", "baz", "foo")
        check("streams-03 XADD 0-0 is an error",
              got == "ERR The ID specified in XADD must be greater than 0-0", repr(got))
        got = c.call("XADD", rand_word(), "0-0", "baz", "foo")
        check("local: XADD 0-0 on a missing key is an error",
              got == "ERR The ID specified in XADD must be greater than 0-0", repr(got))

        key = rand_word()
        check("streams-04 XADD 0-* on a new stream", c.call("XADD", key, "0-*", "a", "b") == "0-1")
        check("streams-04 XADD 1-*", c.call("XADD", key, "1-*", "a", "b") == "1-0")
        check("streams-04 XADD 1-* again", c.call("XADD", key, "1-*", "a", "b") == "1-1")
        got = c.call("XADD", key, "0-*", "a", "b")
        check("local: XADD 0-* below the top is an error", got == smaller, repr(got))

        key = rand_word()
        before = int(time.time() * 1000)
        got = c.call("XADD", key, "*", "foo", "bar")
        after = int(time.time() * 1000)
        ms, _, seq = got.partition("-")
        check("streams-05 XADD * gives <ms>-0 with the time now",
              ms.isdigit() and seq == "0" and before - 1000 <= int(ms) <= after + 1000, repr(got))
        got = c.call("XADD", key, ms + "-*", "foo", "bar")
        check("local: XADD <ms>-* after * gives the next sequence", got == ms + "-1", repr(got))

        key = rand_word()
        c.call("XADD", key, "0-1", "foo", "bar")
        c.call("XADD", key, "0-2", "bar", "baz")
        c.call("XADD", key, "0-3", "baz", "foo")
        e1, e2, e3 = ["0-1", ["foo", "bar"]], ["0-2", ["bar", "baz"]], ["0-3", ["baz", "foo"]]
        check("streams-06 XRANGE 0-2 0-3", c.call("XRANGE", key, "0-2", "0-3") == [e2, e3])
        check("streams-06 XRANGE 0 0 (no sequence part)",
              c.call("XRANGE", key, "0", "0") == [e1, e2, e3])
        check("streams-07 XRANGE - 0-2", c.call("XRANGE", key, "-", "0-2") == [e1, e2])
        check("streams-08 XRANGE 0-2 +", c.call("XRANGE", key, "0-2", "+") == [e2, e3])
        check("local: XRANGE COUNT 2", c.call("XRANGE", key, "-", "+", "COUNT", "2") == [e1, e2])
        check("local: XRANGE of a missing key", c.call("XRANGE", rand_word(), "-", "+") == [])
        check("local: XREAD COUNT 2",
              c.call("XREAD", "COUNT", "2", "STREAMS", key, "0") == [[key, [e1, e2]]])

        key = rand_word()
        for i, ident in enumerate(["1-5", "2-0", "2-3", "3-1"]):
            c.call("XADD", key, ident, "f", str(i))
        check("local: XRANGE across milliseconds",
              c.call("XRANGE", key, "2-0", "2-9") == [["2-0", ["f", "1"]], ["2-3", ["f", "2"]]])

        key = rand_word()
        c.call("XADD", key, "0-1", "temperature", "96")
        check("streams-09 XREAD of one stream",
              c.call("XREAD", "streams", key, "0-0") == [[key, [["0-1", ["temperature", "96"]]]]])
        check("local: XREAD with nothing to read is a null array",
              c.call("XREAD", "streams", key, "0-1") is None)
        c.call("XADD", key, "0-2", "temperature", "97")
        check("local: XREAD from a middle ID reads only the entries above it",
              c.call("XREAD", "streams", key, "0-1") == [[key, [["0-2", ["temperature", "97"]]]]])

        a, b = rand_word(), rand_word()
        c.call("XADD", a, "0-1", "temperature", "95")
        c.call("XADD", b, "0-2", "humidity", "97")
        check("streams-10 XREAD of two streams",
              c.call("XREAD", "streams", a, b, "0-0", "0-1")
              == [[a, [["0-1", ["temperature", "95"]]]], [b, [["0-2", ["humidity", "97"]]]]])

        key = rand_word()
        c.call("XADD", key, "0-1", "temperature", "96")
        r = srv.client()
        r.send_raw(encode("XREAD", "block", "1000", "streams", key, "0-1"))
        time.sleep(0.3)
        c.call("XADD", key, "0-2", "temperature", "95")
        check("streams-11 XREAD BLOCK gets the new entry",
              r.try_read(3) == [[key, [["0-2", ["temperature", "95"]]]]])
        start = time.time()
        r.send_raw(encode("XREAD", "block", "1000", "streams", key, "0-2"))
        got = r.try_read(3)
        waited = time.time() - start
        check("streams-11 XREAD BLOCK 1000 ends with a null array after about 1 s",
              got is None and 0.9 <= waited <= 2.0, "%r after %.2f s" % (got, waited))

        key = rand_word()
        c.call("XADD", key, "0-1", "temperature", "96")
        r.send_raw(encode("XREAD", "block", "0", "streams", key, "0-1"))
        check("streams-12 XREAD BLOCK 0 waits", r.try_read(1) is TIMEOUT)
        c.call("XADD", key, "0-2", "temperature", "95")
        check("streams-12 XREAD BLOCK 0 gets the new entry",
              r.try_read(3) == [[key, [["0-2", ["temperature", "95"]]]]])

        key = rand_word()
        c.call("XADD", key, "0-1", "temperature", "96")
        r.send_raw(encode("XREAD", "block", "0", "streams", key, "$"))
        time.sleep(0.3)
        c.call("XADD", key, "0-2", "temperature", "95")
        check("streams-13 XREAD BLOCK with $ gets only the new entry",
              r.try_read(3) == [[key, [["0-2", ["temperature", "95"]]]]])
        r.send_raw(encode("XREAD", "block", "300", "streams", key, "$"))
        check("streams-13 XREAD BLOCK with $ and no new entry times out",
              r.try_read(3) is None)

        # Every reader of the key gets the entry, and a reader of two
        # streams gets only the stream that changed.
        key, other = rand_word(), rand_word()
        r2 = srv.client()
        r.send_raw(encode("XREAD", "BLOCK", "0", "STREAMS", key, "$"))
        r2.send_raw(encode("XREAD", "BLOCK", "0", "STREAMS", other, key, "$", "$"))
        time.sleep(0.3)
        c.call("XADD", key, "5-1", "f", "v")
        entry = [key, [["5-1", ["f", "v"]]]]
        got1, got2 = r.try_read(3), r2.try_read(3)
        check("local: XADD wakes every reader of the key",
              got1 == [entry] and got2 == [entry], "%r %r" % (got1, got2))

        # One write that holds XADD a and XADD b: the reader of a and b is
        # served by the first one only.
        served_a = 0
        for _ in range(20):
            ka, kb = rand_word(), rand_word()
            r.send_raw(encode("XREAD", "BLOCK", "0", "STREAMS", ka, kb, "$", "$"))
            time.sleep(0.05)
            c.raw().send_raw(encode("XADD", ka, "1-1", "f", "a") + encode("XADD", kb, "1-1", "f", "b"))
            c.raw().try_read(3)
            c.raw().try_read(3)
            if r.try_read(3) != [[ka, [["1-1", ["f", "a"]]]]]:
                break
            served_a += 1
        check("local: a reader that XADD served is not served again by the next XADD (20 rounds)",
              served_a == 20, "%d of 20" % served_a)

        # A reader whose ID is above the top waits until an entry above it
        # comes. A second reader, which the lower XADD serves, leaves the
        # queue without taking the other two out of it.
        key = rand_word()
        c.call("XADD", key, "1-0", "f", "v")
        r.send_raw(encode("XREAD", "BLOCK", "0", "STREAMS", key, "9-0"))
        r3 = srv.client()
        r3.send_raw(encode("XREAD", "BLOCK", "0", "STREAMS", key, "9-0"))
        time.sleep(0.1)
        r2.send_raw(encode("XREAD", "BLOCK", "0", "STREAMS", key, "$"))
        r4 = srv.client()
        r4.send_raw(encode("XREAD", "BLOCK", "0", "STREAMS", key, "$"))
        time.sleep(0.2)
        c.call("XADD", key, "2-0", "f", "v")
        low = [[key, [["2-0", ["f", "v"]]]]]
        got2, got4 = r2.try_read(3), r4.try_read(3)
        r4.close()
        check("local: XREAD BLOCK with $ gets the lower XADD (two readers)",
              got2 == low and got4 == low, "%r %r" % (got2, got4))
        r.sock.settimeout(0.5)
        try:
            early = r.sock.recv(65536)
        except socket.timeout:
            early = b""
        r.sock.settimeout(5)
        r.buf += early
        check("local: XREAD BLOCK above the top waits after a lower XADD (no byte comes)",
              early == b"", repr(early))
        c.call("XADD", key, "10-0", "f", "v")
        above = [[key, [["10-0", ["f", "v"]]]]]
        got1, got3 = r.try_read(3), r3.try_read(3)
        check("local: XREAD BLOCK above the top gets the entry above it",
              got1 == above and got3 == above, "%r %r" % (got1, got3))
        r3.close()

        # "$" names the top of the stream at the wait. When the key is
        # replaced while the reader waits, an entry below that top does not
        # wake it, and an entry above it does.
        key = rand_word()
        c.call("XADD", key, "5-1", "f", "v")
        r.send_raw(encode("XREAD", "BLOCK", "0", "STREAMS", key, "$"))
        time.sleep(0.1)
        c.call("SET", key, "x", "PX", "30")
        time.sleep(0.1)
        c.call("XADD", key, "1-1", "f", "low")
        check("local: XREAD BLOCK $ ignores an entry below the old top of a replaced key",
              r.try_read(0.5) is TIMEOUT)
        c.call("XADD", key, "9-1", "f", "high")
        check("local: XREAD BLOCK $ gets an entry above the old top of a replaced key",
              r.try_read(3) == [[key, [["9-1", ["f", "high"]]]]])

        # The commands that the client sends during the wait run after it,
        # in order.
        key = rand_word()
        r.send_raw(encode("XREAD", "BLOCK", "5000", "STREAMS", key, "$") + encode("PING") * 50)
        time.sleep(0.2)
        c.call("XADD", key, "1-1", "f", "v")
        got = r.try_read(3)
        pongs = 0
        while pongs < 50 and r.try_read(3) == "PONG":
            pongs += 1
        check("local: 50 commands sent during XREAD BLOCK run after it, in order",
              got == [[key, [["1-1", ["f", "v"]]]]] and pongs == 50, "%r, %d PONG" % (got, pongs))

        def clients_now():
            info = c.call("INFO", "clients")
            for line in info.split("\r\n"):
                if line.startswith("connected_clients:"):
                    return int(line.split(":")[1])
            return -1

        def clients_back_to(count):
            deadline = time.time() + 5
            now = clients_now()
            while time.time() < deadline and now != count:
                time.sleep(0.05)
                now = clients_now()
            return now

        r.close()
        r2.close()
        base = clients_now()
        gone = srv.client()
        gone.send_raw(encode("XREAD", "BLOCK", "0", "STREAMS", rand_word(), "$")
                      + encode("PING") * 100)
        time.sleep(0.2)
        gone.close()
        now = clients_back_to(base)
        check("local: XREAD BLOCK with no limit ends when its client goes",
              now == base, "connected_clients %d, before %d" % (now, base))

        key = rand_word()
        c.call("SET", key, "v")
        for args in (("XADD", key, "1-1", "f", "v"), ("XRANGE", key, "-", "+"),
                     ("XREAD", "STREAMS", key, "0")):
            got = c.call(*args)
            check("local: %s on a string key is WRONGTYPE" % args[0],
                  isinstance(got, Error) and got.startswith("WRONGTYPE"), repr(got))
        c.close()
    finally:
        srv.stop()
    stages_streams_large(prog)


class Asker:
    """A client whose call() never raises: when a reply does not come or
    cannot be read, it gives TIMEOUT or CLOSED and opens a new connection.
    So one broken reply fails its own case, and the cases after it still
    run."""

    def __init__(self, srv):
        self.srv = srv
        self.c = srv.client()

    def call(self, *args):
        try:
            if self.c is None:
                self.c = self.srv.client()
            self.c.send_raw(encode(*args))
            got = self.c.try_read(3)
        except (OSError, ValueError, EOFError):
            got = CLOSED
        if got is TIMEOUT or got is CLOSED or got is BAD_REPLY:
            if self.c is not None:
                self.c.close()
            self.c = None
        return got

    def raw(self):
        """The connection, for a case that sends raw bytes. It opens a new
        one when a failed call closed the last one."""
        if self.c is None:
            self.c = self.srv.client()
        return self.c

    def close(self):
        if self.c is not None:
            self.c.close()


def make_streams(c, keys):
    for i in range(0, len(keys), 4096):
        part = keys[i:i + 4096]
        c.send_raw(b"".join(encode("XADD", k, "1-1", "f", "v") for k in part))
        for _ in part:
            c.read()


def stages_streams_large(prog):
    """S1-S9, on a server of their own: S8 fills the heap with streams."""
    srv = Server(prog, [])
    try:
        check("streams (large): server starts", srv.up, srv.log_text())
        if not srv.up:
            return
        c = srv.client()
        c.call("XADD", "one", "1-1", "f", "v")
        got = [None]

        # S1-S4: the largest commands of the group. XADD keeps the fields as
        # one slice, the session writes a reply in steps and yields between
        # them, and XREAD parses its IDs before its one step, with yields.
        n = 524286
        big = srv.client()

        def xadd_big():
            big.send_raw(encode("XADD", "s1", "1-1", *(["abcd"] * (2 * n))))
            big.try_read(15)
            time.sleep(0.1)
        worst, wrong, _ = measure_while(srv, xadd_big)
        check("local: S1 XADD with 524286 field pairs stalls PING at most 0.2 s",
              worst <= 0.2 and not wrong, "worst %.3f s" % worst)
        got = [None]

        def xrange_big():
            big.send_raw(encode("XRANGE", "s1", "-", "+"))
            got[0] = big.try_read(15)
            time.sleep(0.1)
        worst, wrong, _ = measure_while(srv, xrange_big)
        check("local: S2 XRANGE of that entry gives every field",
              got[0] == [["1-1", ["abcd"] * (2 * n)]])
        check("local: S2 XRANGE of that entry stalls PING at most 0.15 s",
              worst <= 0.15 and not wrong, "worst %.3f s" % worst)
        keys = ["s3-%d" % i for i in range(n)]

        def xread_many():
            big.send_raw(encode("XREAD", "STREAMS", *keys, *(["0"] * n)))
            got[0] = big.try_read(15)
            time.sleep(0.1)
        worst, wrong, _ = measure_while(srv, xread_many)
        check("local: S3 XREAD of 524286 missing streams gives a null array", got[0] is None)
        check("local: S3 XREAD of 524286 streams stalls PING at most 0.5 s",
              worst <= 0.5 and not wrong, "worst %.3f s" % worst)

        def xread_block_many():
            big.send_raw(encode("XREAD", "BLOCK", "1000", "STREAMS", *keys, *(["0"] * (n - 1)), "0"))
            got[0] = big.try_read(15)
            time.sleep(0.3)
        worst, wrong, _ = measure_while(srv, xread_block_many)
        check("local: S4 XREAD BLOCK on 524286 streams times out with a null array",
              got[0] is None, repr(got[0])[:80])
        check("local: S4 XREAD BLOCK on 524286 streams stalls PING at most 1 s",
              worst <= 1.0 and not wrong, "worst %.3f s" % worst)
        big.close()


        # S5, S6: the options of XREAD and XRANGE are parsed before the
        # command, with yields.
        m = 524000

        def many_options(cmd):
            def run():
                c.send_raw(cmd)
                got[0] = c.try_read(10)
                time.sleep(0.1)
            return run
        worst, wrong, _ = measure_while(srv, many_options(
            encode("XREAD", *(["COUNT", "1"] * m), "STREAMS", "one", "0")))
        check("local: S5 XREAD with 524000 COUNT options stalls PING at most 0.2 s",
              worst <= 0.2 and not wrong and got[0] == [["one", [["1-1", ["f", "v"]]]]],
              "worst %.3f s, %r" % (worst, got[0]))
        worst, wrong, _ = measure_while(srv, many_options(
            encode("XRANGE", "one", "-", "+", *(["COUNT", "1"] * m))))
        check("local: S6 XRANGE with 524000 COUNT options stalls PING at most 0.2 s",
              worst <= 0.2 and not wrong and got[0] == [["1-1", ["f", "v"]]],
              "worst %.3f s, %r" % (worst, got[0]))

        # S7: 64 entries of almost 1 MB each. A step of the reply writer
        # holds about 1 MB, so it does not join the whole reply.
        field = "y" * (1048576 - 64)
        for i in range(64):
            c.call("XADD", "s7", "1-%d" % (i + 1), "f", field)
        size = [0]

        def xrange_s7():
            c.send_raw(encode("XRANGE", "s7", "-", "+"))
            c.sock.settimeout(10)
            want = 64 * (len(field) + 40)
            n = 0
            try:
                while n < want - 64 * 40:
                    chunk = c.sock.recv(1 << 20)
                    if not chunk:
                        break
                    n += len(chunk)
            except OSError:
                pass
            size[0] = n
            time.sleep(0.2)
        worst, wrong, _ = measure_while(srv, xrange_s7)
        c.close()
        check("local: S7 XRANGE of 64 entries of almost 1 MB stalls PING at most 0.5 s",
              worst <= 0.5 and not wrong and size[0] >= 64 * len(field),
              "worst %.3f s, %d bytes" % (worst, size[0]))

        # S9: one field of 64 MB goes to the socket alone, with no copy.
        c = srv.client()
        field = "z" * (64 << 20)
        c.call("XADD", "s9", "1-1", "f", field)

        def xrange_s9():
            c.send_raw(encode("XRANGE", "s9", "-", "+"))
            got[0] = c.try_read(15)
            time.sleep(0.2)
        worst, wrong, _ = measure_while(srv, xrange_s9)
        check("local: S9 XRANGE of a 64 MB field gives it",
              got[0] == [["1-1", ["f", field]]])
        check("local: S9 XRANGE of a 64 MB field stalls PING at most 0.25 s",
              worst <= 0.25 and not wrong, "worst %.3f s" % worst)
        got[0] = None
        field = None
        c.close()

        # S8, on a new server, so that the heap of S1-S9 does not add GC
        # pauses: XREAD of 131072 streams that have entries. The command
        # keeps only the bounds of each stream's entries; the session writes
        # them.
        srv.stop()
        srv = Server(prog, [])
        check("streams (S8): server starts", srv.up, srv.log_text())
        if not srv.up:
            return
        c = srv.client()
        keys = ["s8-%d" % i for i in range(131072)]
        make_streams(c, keys)

        def xread_s8():
            c.send_raw(encode("XREAD", "STREAMS", *keys, *(["0"] * len(keys))))
            got[0] = c.try_read(15)
            time.sleep(0.2)
        worst, wrong, _ = measure_while(srv, xread_s8)
        check("local: S8 XREAD of 131072 streams with entries gives each one",
              isinstance(got[0], list) and len(got[0]) == len(keys)
              and got[0][-1] == [keys[-1], [["1-1", ["f", "v"]]]])
        check("local: S8 XREAD of 131072 streams with entries stalls PING at most 0.45 s",
              worst <= 0.45 and not wrong, "worst %.3f s" % worst)
        c.close()
    finally:
        srv.stop()

def stages_transactions(prog):
    srv = Server(prog, [])
    try:
        check("transactions: server starts", srv.up, srv.log_text())
        if not srv.up:
            return
        c = Asker(srv)

        key = rand_word()
        c.call("SET", key, "41")
        check("transactions-01 INCR of a number", c.call("INCR", key) == 42)
        check("transactions-01 INCR again", c.call("INCR", key) == 43)
        check("transactions-02 INCR of a missing key", c.call("INCR", rand_word()) == 1)
        key = rand_word()
        c.call("SET", key, "xyz")
        check("transactions-03 INCR of a word",
              c.call("INCR", key) == "ERR value is not an integer or out of range")
        lkey = rand_word()
        c.call("RPUSH", lkey, "a")
        got = c.call("INCR", lkey)
        check("local: INCR of a list is WRONGTYPE",
              isinstance(got, Error) and got.startswith("WRONGTYPE"), repr(got))
        key = rand_word()
        c.call("SET", key, "999999999999999")
        check("local: INCR above 15 digits is an overflow error",
              c.call("INCR", key) == "ERR increment or decrement would overflow")
        key = rand_word()
        c.call("SET", key, "5", "PX", "300")
        c.call("INCR", key)
        time.sleep(0.5)
        check("local: INCR keeps the expiry", c.call("GET", key) is None)

        check("transactions-04 MULTI", c.call("MULTI") == "OK")
        check("transactions-06 empty EXEC", c.call("EXEC") == [])
        check("transactions-05 EXEC without MULTI",
              c.call("EXEC") == "ERR EXEC without MULTI")
        check("local: DISCARD without MULTI",
              c.call("DISCARD") == "ERR DISCARD without MULTI")

        key = rand_word()
        other = Asker(srv)
        c.call("MULTI")
        check("transactions-07 SET is queued", c.call("SET", key, "41") == "QUEUED")
        check("transactions-07 INCR is queued", c.call("INCR", key) == "QUEUED")
        check("transactions-07 another client sees nothing yet",
              other.call("GET", key) is None)
        check("transactions-08 EXEC gives each reply", c.call("EXEC") == ["OK", 42])
        check("transactions-08 the commands ran", other.call("GET", key) == "42")

        key = rand_word()
        c.call("MULTI")
        c.call("SET", key, "41")
        check("transactions-09 DISCARD", c.call("DISCARD") == "OK")
        check("transactions-09 the commands did not run", c.call("GET", key) is None)
        check("transactions-09 DISCARD again",
              c.call("DISCARD") == "ERR DISCARD without MULTI")

        a, b = rand_word(), rand_word()
        c.call("SET", a, "abc")
        c.call("MULTI")
        c.call("INCR", a)
        c.call("SET", b, "7")
        got = c.call("EXEC")
        check("transactions-10 a failed command does not stop the others",
              got == ["ERR value is not an integer or out of range", "OK"], repr(got))
        check("transactions-10 the next command ran", c.call("GET", b) == "7")

        key = rand_word()
        c.call("MULTI")
        other.call("MULTI")
        c.call("INCR", key)
        other.call("INCR", key)
        c.call("INCR", key)
        got1 = other.call("EXEC")
        got2 = c.call("EXEC")
        check("transactions-11 two transactions at once",
              got1 == [1] and got2 == [2, 3], "%r %r" % (got1, got2))

        c.call("MULTI")
        c.call("PING")
        check("local: MULTI in MULTI is an error",
              c.call("MULTI") == "ERR MULTI calls can not be nested")
        check("local: the transaction goes on after it", c.call("EXEC") == ["PONG"])
        c.call("MULTI")
        got = c.call("NOSUCH", "a")
        check("local: an unknown command in MULTI is an error",
              isinstance(got, Error) and got.startswith("ERR unknown command"), repr(got))
        check("local: EXEC after an unknown command is EXECABORT",
              c.call("EXEC") == "EXECABORT Transaction discarded because of previous errors.")
        c.call("MULTI")
        check("local: a wrong argument count in MULTI is an error",
              c.call("GET") == "ERR wrong number of arguments for 'get' command")
        check("local: EXEC after a wrong argument count is EXECABORT",
              c.call("EXEC") == "EXECABORT Transaction discarded because of previous errors.")
        c.call("MULTI")
        check("local: EXEC with an argument ends the transaction",
              c.call("EXEC", "x") == "EXECABORT Transaction discarded because of: "
              "wrong number of arguments for 'exec' command")
        check("local: no transaction after it", c.call("EXEC") == "ERR EXEC without MULTI")
        check("local: EXEC with an argument and no MULTI is EXECABORT too",
              c.call("EXEC", "x") == "EXECABORT Transaction discarded because of: "
              "wrong number of arguments for 'exec' command")

        # A command that would wait does not wait in EXEC.
        key, skey = rand_word(), rand_word()
        c.call("MULTI")
        c.call("BLPOP", key, "0")
        c.call("XREAD", "BLOCK", "0", "STREAMS", skey, "$")
        start = time.time()
        got = c.call("EXEC")
        check("local: BLPOP and XREAD BLOCK in EXEC give null arrays at once",
              got == [None, None] and time.time() - start < 1.0, repr(got))

        # Replies that the session writes in steps.
        lkey, skey = rand_word(), rand_word()
        c.call("MULTI")
        c.call("RPUSH", lkey, "a", "b")
        c.call("LRANGE", lkey, "0", "-1")
        c.call("XADD", skey, "1-1", "f", "v")
        c.call("XRANGE", skey, "-", "+")
        c.call("XREAD", "STREAMS", skey, "0")
        got = c.call("EXEC")
        check("local: EXEC with list and stream replies",
              got == [2, ["a", "b"], "1-1", [["1-1", ["f", "v"]]],
                      [[skey, [["1-1", ["f", "v"]]]]]], repr(got))

        # A client that waits acts before or after a transaction, never in
        # it: EXEC serves the waiters one time, after its last command.
        key = rand_word()
        waiter = srv.client()
        waiter.send_raw(encode("BLPOP", key, "0"))
        time.sleep(0.1)
        c.call("MULTI")
        c.call("RPUSH", key, "a")
        c.call("LPOP", key)
        got = c.call("EXEC")
        check("local: a push and a pop in EXEC are not seen by a BLPOP waiter",
              got == [1, "a"], repr(got))
        check("local: the waiter still waits after that EXEC", waiter.try_read(0.3) is TIMEOUT)
        c.call("MULTI")
        c.call("RPUSH", key, "a", "b")
        c.call("LRANGE", key, "0", "-1")
        got = c.call("EXEC")
        check("local: EXEC sees its own push whole while a client waits",
              got == [2, ["a", "b"]], repr(got))
        check("local: the waiter gets the first element after EXEC",
              waiter.try_read(3) == [key, "a"])
        waiter.close()
        skey = rand_word()
        c.call("XADD", skey, "1-1", "f", "v")
        reader = srv.client()
        reader.send_raw(encode("XREAD", "BLOCK", "0", "STREAMS", skey, "$"))
        time.sleep(0.1)
        c.call("MULTI")
        c.call("XADD", skey, "2-1", "f", "a")
        c.call("XADD", skey, "3-1", "f", "b")
        c.call("EXEC")
        got = reader.try_read(3)
        check("local: an XREAD BLOCK $ reader gets every entry of an EXEC",
              got == [[skey, [["2-1", ["f", "a"]], ["3-1", ["f", "b"]]]]], repr(got))
        reader.close()

        # A key that EXEC pushes to and then replaces wakes nobody.
        key = rand_word()
        waiter = srv.client()
        waiter.send_raw(encode("BLPOP", key, "1"))
        time.sleep(0.1)
        c.call("MULTI")
        c.call("RPUSH", key, "a")
        c.call("SET", key, "str")
        c.call("EXEC")
        check("local: a list that EXEC replaces does not serve its BLPOP waiter",
              waiter.try_read(3) is None)
        waiter.close()
        skey = rand_word()
        c.call("XADD", skey, "1-1", "f", "v")
        reader = srv.client()
        reader.send_raw(encode("XREAD", "BLOCK", "1000", "STREAMS", skey, "$"))
        time.sleep(0.1)
        c.call("MULTI")
        c.call("XADD", skey, "2-1", "f", "a")
        c.call("SET", skey, "str")
        c.call("EXEC")
        check("local: a stream that EXEC replaces does not serve its $ reader",
              reader.try_read(3) is None)
        reader.close()
        skey = rand_word()
        c.call("XADD", skey, "1-1", "f", "v")
        reader = srv.client()
        reader.send_raw(encode("XREAD", "COUNT", "1", "BLOCK", "0", "STREAMS", skey, "$"))
        time.sleep(0.1)
        c.call("MULTI")
        c.call("XADD", skey, "2-1", "f", "a")
        c.call("XADD", skey, "3-1", "f", "b")
        c.call("EXEC")
        check("local: COUNT 1 caps what a $ reader gets from an EXEC",
              reader.try_read(3) == [[skey, [["2-1", ["f", "a"]]]]])
        reader.close()

        # EXEC is atomic: a client that reads the key during a long EXEC
        # sees it before or after, never in between.
        key = rand_word()
        c.call("SET", key, "0")
        c.call("MULTI")
        for _ in range(5):
            c.raw().send_raw(encode("INCR", key) * 4000)
            for _ in range(4000):
                c.raw().try_read(3)
        seen = set()
        stop = threading.Event()

        def poll():
            p = srv.client()
            while not stop.is_set():
                seen.add(p.call("GET", key))
            p.close()
        th = threading.Thread(target=poll)
        th.start()
        time.sleep(0.1)
        got = c.call("EXEC")
        time.sleep(0.1)
        stop.set()
        th.join(10)
        check("local: EXEC of 20000 INCR gives each reply",
              isinstance(got, list) and len(got) == 20000 and got[-1] == 20000)
        check("local: another client sees the key before or after EXEC, never in between",
              seen <= {"0", "20000"} and "20000" in seen, repr(sorted(seen)[:5]))

        # The bound of a transaction (README rule 4).
        c.call("MULTI")
        for _ in range(16):
            c.raw().send_raw(encode("PING") * 4096)
            for _ in range(4096):
                c.raw().try_read(3)
        got = c.call("PING")
        check("local: the 65537th command of a transaction is refused",
              isinstance(got, Error) and "too many" in got, repr(got))
        check("local: EXEC after it is EXECABORT",
              c.call("EXEC") == "EXECABORT Transaction discarded because of previous errors.")
        c.call("MULTI")
        c.raw().send_raw(encode("RPUSH", "big", *(["a"] * 1048574)))
        c.raw().try_read(10)
        got = c.call("PING", "x")
        check("local: a transaction over 1048576 arguments is refused",
              isinstance(got, Error) and "too many" in got, repr(got))
        c.call("DISCARD")

        def clients_now():
            info = c.call("INFO", "clients")
            for line in info.split("\r\n"):
                if line.startswith("connected_clients:"):
                    return int(line.split(":")[1])
            return -1

        base = clients_now()
        gone = srv.client()
        gone.send_raw(encode("MULTI") + encode("SET", "g", "1") * 100)
        time.sleep(0.2)
        gone.close()
        deadline = time.time() + 5
        now = clients_now()
        while time.time() < deadline and now != base:
            time.sleep(0.05)
            now = clients_now()
        check("local: a client that goes during MULTI ends its tasks",
              now == base, "connected_clients %d, before %d" % (now, base))
        check("local: its queued commands did not run", c.call("GET", "g") is None)

        # T1, T2: EXEC of the largest transactions.
        def exec_stall(cmds):
            c.call("MULTI")
            for i in range(0, len(cmds), 4096):
                part = cmds[i:i + 4096]
                c.raw().send_raw(b"".join(part))
                for _ in part:
                    c.raw().try_read(10)
            out = [None]

            def run():
                c.raw().send_raw(encode("EXEC"))
                out[0] = c.raw().try_read(30)
                time.sleep(0.2)
            worst, wrong, _ = measure_while(srv, run)
            return worst, wrong, out[0]
        worst, wrong, got = exec_stall([encode("RPUSH", "t1", *(["a"] * 14))] * 65535)
        check("local: T1 EXEC of 65535 RPUSH of 14 elements stalls PING at most 0.6 s",
              worst <= 0.6 and not wrong and isinstance(got, list) and len(got) == 65535,
              "worst %.3f s" % worst)
        keys = ["t2-%d" % i for i in range(524286)]
        worst, wrong, got = exec_stall([encode("XREAD", "STREAMS", *keys, *(["0"] * len(keys)))])
        check("local: T2 EXEC of an XREAD of 524286 streams stalls PING at most 0.5 s",
              worst <= 0.5 and not wrong and got == [None], "worst %.3f s, %r" % (worst, got))
        c.close()
        other.close()
    finally:
        srv.stop()
    stages_transactions_large(prog)


def stages_transactions_large(prog):
    """The byte bound of a transaction, and T3-T5, on servers of their
    own."""
    srv = Server(prog, ["--client-query-buffer-limit", "100000"])
    try:
        check("transactions (bytes): server starts", srv.up, srv.log_text())
        if not srv.up:
            return
        c = srv.client()
        c.call("MULTI")
        first = c.call("SET", "k", "x" * 60000)
        got = c.call("SET", "k", "x" * 60000)
        check("local: a transaction over the query limit of bytes is refused",
              first == "QUEUED" and isinstance(got, Error) and "query limit of bytes" in got,
              repr(got))
        check("local: EXEC after it is EXECABORT (bytes)",
              c.call("EXEC") == "EXECABORT Transaction discarded because of previous errors.")
        c.close()
    finally:
        srv.stop()

    srv = Server(prog, [])
    try:
        check("transactions (large): server starts", srv.up, srv.log_text())
        if not srv.up:
            return
        c = srv.client()

        def exec_stall(cmds, drain=None):
            c.call("MULTI")
            for i in range(0, len(cmds), 4096):
                part = cmds[i:i + 4096]
                c.send_raw(b"".join(part))
                for _ in part:
                    c.try_read(10)
            got = [None]

            def run():
                c.send_raw(encode("EXEC"))
                if drain is None:
                    got[0] = c.try_read(30)
                else:
                    c.sock.settimeout(30)
                    n = 0
                    try:
                        while n < drain:
                            chunk = c.sock.recv(1 << 22)
                            if not chunk:
                                break
                            n += len(chunk)
                    except OSError:
                        pass
                    c.sock.settimeout(5)
                    got[0] = n
                time.sleep(0.2)
            worst, wrong, _ = measure_while(srv, run)
            return worst, wrong, got[0]

        def exec_stall_raw(cmds, read):
            c.call("MULTI")
            for i in range(0, len(cmds), 4096):
                part = cmds[i:i + 4096]
                c.send_raw(b"".join(part))
                for _ in part:
                    c.try_read(10)

            def run():
                c.send_raw(encode("EXEC"))
                read()
                time.sleep(0.2)
            return measure_while(srv, run)

        # T3: 200 readers wait above the top, and EXEC adds 10000 entries.
        # EXEC passes over the readers once, not once for each XADD.
        c.call("XADD", "t3", "1-1", "f", "v")
        readers = []
        for _ in range(200):
            r = srv.client()
            r.send_raw(encode("XREAD", "BLOCK", "0", "STREAMS", "t3", "999999999-0"))
            readers.append(r)
        time.sleep(0.5)
        worst, wrong, got = exec_stall(
            [encode("XADD", "t3", "%d-1" % (i + 2), "f", "v") for i in range(10000)])
        check("local: T3 EXEC of 10000 XADD with 200 readers above the top stalls PING at most 0.5 s",
              worst <= 0.5 and not wrong and isinstance(got, list) and len(got) == 10000,
              "worst %.3f s" % worst)
        for r in readers:
            r.close()

        # T4: GET gives its value with no copy, and the session writes a
        # long value alone.
        size = 64 << 20
        c.call("SET", "t4", "x" * size)
        want = 8 * (len("$%d\r\n" % size) + size + 2) + len("*8\r\n")
        worst, wrong, got = exec_stall([encode("GET", "t4")] * 8, drain=want)
        check("local: T4 EXEC of 8 GET of a 64 MB value stalls PING at most 0.3 s",
              worst <= 0.3 and not wrong and got == want, "worst %.3f s, %r bytes" % (worst, got))

        # T5: the session writes the replies of EXEC in steps.
        c.call("SET", "t5", "y" * 1024)
        want = 65535 * (len("$1024\r\n") + 1024 + 2) + len("*65535\r\n")
        worst, wrong, got = exec_stall([encode("GET", "t5")] * 65535, drain=want)
        check("local: T5 EXEC of 65535 GET of a 1 KB value stalls PING at most 0.5 s",
              worst <= 0.5 and not wrong and got == want, "worst %.3f s, %r bytes" % (worst, got))

        # T6: the replies of one EXEC copy at most 8388608 list elements.
        n = 1048573
        c.send_raw(encode("RPUSH", "t6", *(["abcd"] * n)))
        c.try_read(30)
        # The reply is read as bytes: 8 arrays of the list, then the
        # replies after them, which must be errors. With no budget the
        # ninth reply is a whole copy, and the case fails at its first line.
        array = b"*%d\r\n" % n + b"$4\r\nabcd\r\n" * n
        head = b"*202\r\n" + array * 8
        tail = [None]

        def read_t6():
            c.sock.settimeout(30)
            buf = b""
            try:
                # The tail after the eight arrays is about 15 KB.
                while len(buf) < len(head) + 65536:
                    chunk = c.sock.recv(1 << 22)
                    if not chunk:
                        break
                    buf += chunk
                    if len(buf) >= len(head) and buf[len(head):].count(b"\r\n") >= 194:
                        break
            except OSError:
                pass
            c.sock.settimeout(5)
            tail[0] = buf
        worst, wrong, _ = exec_stall_raw([encode("LRANGE", "t6", "0", "-1")] * 200
                                         + [encode("LPOP", "t6", "100"), encode("LPOP", "t6", "10")],
                                         read_t6)
        buf = tail[0] or b""
        lines = buf[len(head):].split(b"\r\n") if buf.startswith(head) else []
        refused = [x for x in lines[:192] if x.startswith(b"-ERR")]
        check("local: T6 EXEC of 200 LRANGE of 1048573 elements gives 8 and refuses 192",
              buf.startswith(head) and len(refused) == 192,
              "%d bytes, %d refused, next %r" % (len(buf), len(refused), lines[:1]))
        check("local: T6 an LPOP over the rest of the budget is refused, one under it runs",
              len(lines) > 193 and lines[192].startswith(b"-ERR") and lines[193] == b"*10",
              repr(lines[192:195]))
        c.close()
        c = srv.client()
        check("local: T6 that EXEC stalls PING at most 0.5 s",
              worst <= 0.5 and not wrong, "worst %.3f s" % worst)

        # T7: 5 readers each wait above the top on 65535 keys, and one EXEC
        # adds to each key. The pass after EXEC checks each reader on each
        # key once, as Redis does.
        keys = ["t7-%d" % i for i in range(65535)]
        waiting = []
        for _ in range(5):
            r = srv.client()
            r.send_raw(encode("XREAD", "BLOCK", "0", "STREAMS", *keys, *(["999999999-0"] * len(keys))))
            waiting.append(r)
        time.sleep(3)
        worst, wrong, got = exec_stall([encode("XADD", k, "1-1", "f", "v") for k in keys])
        check("local: T7 EXEC of 65535 XADD with 5 readers above the top on each key stalls PING at most 1 s",
              worst <= 1.0 and not wrong and isinstance(got, list) and len(got) == 65535,
              "worst %.3f s" % worst)
        for r in waiting:
            r.close()
        c.close()
    finally:
        srv.stop()


def stages_optimistic_locking(prog):
    srv = Server(prog, [])
    try:
        check("optimistic-locking: server starts", srv.up, srv.log_text())
        if not srv.up:
            return
        a = Asker(srv)
        b = Asker(srv)

        def tx_after(watch_keys, between, key="t"):
            """WATCH, run between(), then MULTI / SET key / EXEC. Returns
            the reply of EXEC."""
            a.call("WATCH", *watch_keys)
            between()
            a.call("MULTI")
            a.call("SET", key, "fromtx")
            return a.call("EXEC")

        check("optimistic-locking-01 WATCH", a.call("WATCH", rand_word()) == "OK")
        a.call("UNWATCH")
        a.call("MULTI")
        check("optimistic-locking-02 WATCH inside MULTI",
              a.call("WATCH", rand_word()) == "ERR WATCH inside MULTI is not allowed")
        check("local: the transaction goes on after WATCH in MULTI", a.call("EXEC") == [])

        foo, bar = rand_word(), rand_word()
        a.call("SET", foo, "100")
        a.call("SET", bar, "200")
        got = tx_after([foo, bar], lambda: b.call("SET", foo, "200"), key=bar)
        check("optimistic-locking-03 a write by another client makes EXEC fail",
              got is None, repr(got))
        check("optimistic-locking-03 the transaction did not run", a.call("GET", bar) == "200")

        got = tx_after([foo, bar], lambda: b.call("SET", bar, "300"), key=foo)
        check("optimistic-locking-04 a write to the second watched key makes EXEC fail",
              got is None, repr(got))

        missing = rand_word()
        got = tx_after([missing], lambda: b.call("SET", missing, "1"))
        check("optimistic-locking-05 a watched missing key that is set makes EXEC fail",
              got is None, repr(got))

        k = rand_word()
        a.call("SET", k, "1")

        def unwatch_then_write():
            a.call("UNWATCH")
            b.call("SET", k, "2")
        got = tx_after([k], unwatch_then_write)
        check("optimistic-locking-06 UNWATCH", got == ["OK"], repr(got))

        a.call("WATCH", k)
        a.call("MULTI")
        a.call("EXEC")
        b.call("SET", k, "3")
        a.call("MULTI")
        a.call("PING")
        check("optimistic-locking-07 EXEC forgets the watched keys", a.call("EXEC") == ["PONG"])

        a.call("WATCH", k)
        a.call("MULTI")
        a.call("DISCARD")
        b.call("SET", k, "4")
        a.call("MULTI")
        a.call("PING")
        check("optimistic-locking-08 DISCARD forgets the watched keys", a.call("EXEC") == ["PONG"])

        # Each kind of write makes EXEC fail; a command that changes
        # nothing does not.
        def writes(name, setup, write, fails):
            key = rand_word()
            for cmd in setup:
                b.call(*[key if x == "K" else x for x in cmd])
            got = tx_after([key], lambda: b.call(*[key if x == "K" else x for x in write]))
            want = None if fails else ["OK"]
            check("local: WATCH and %s" % name, got == want, repr(got))
        writes("SET of the same value", [["SET", "K", "1"]], ["SET", "K", "1"], True)
        writes("SET NX that fails", [["SET", "K", "1"]], ["SET", "K", "2", "NX"], False)
        writes("INCR", [["SET", "K", "1"]], ["INCR", "K"], True)
        writes("INCR of a missing key", [], ["INCR", "K"], True)
        writes("RPUSH", [["RPUSH", "K", "a"]], ["RPUSH", "K", "b"], True)
        writes("LPUSH", [], ["LPUSH", "K", "a"], True)
        writes("LPOP", [["RPUSH", "K", "a", "b"]], ["LPOP", "K"], True)
        writes("LPOP with a count", [["RPUSH", "K", "a", "b"]], ["LPOP", "K", "2"], True)
        writes("LPOP of a missing key", [], ["LPOP", "K"], False)
        writes("LPOP with a count of 0", [["RPUSH", "K", "a"]], ["LPOP", "K", "0"], False)
        writes("BLPOP that pops at once", [["RPUSH", "K", "a"]], ["BLPOP", "K", "1"], True)
        writes("XADD", [], ["XADD", "K", "1-1", "f", "v"], True)
        writes("XADD that fails", [["XADD", "K", "5-1", "f", "v"]], ["XADD", "K", "1-1", "f", "v"], False)
        writes("GET", [["SET", "K", "1"]], ["GET", "K"], False)

        key = rand_word()
        a.call("SET", key, "1")
        got = tx_after([key], lambda: a.call("SET", key, "2"))
        check("local: a write by the same client makes EXEC fail", got is None, repr(got))

        # A push that serves a waiting client writes the key too.
        key = rand_word()
        waiter = srv.client()
        waiter.send_raw(encode("BLPOP", key, "0"))
        time.sleep(0.1)

        def push_to_waiter():
            b.call("RPUSH", key, "x")
            waiter.try_read(3)
        got = tx_after([key], push_to_waiter)
        check("local: WATCH and a push that a waiting client takes", got is None, repr(got))
        waiter.close()

        # A watched key that expires makes EXEC fail, also when no command
        # reads it.
        key = rand_word()
        a.call("SET", key, "1", "PX", "100")
        got = tx_after([key], lambda: time.sleep(0.3), key=rand_word())
        check("local: a watched key that expires makes EXEC fail", got is None, repr(got))
        key = rand_word()
        a.call("SET", key, "1", "PX", "100")

        def expire_and_read():
            time.sleep(0.3)
            b.call("GET", key)
        got = tx_after([key], expire_and_read, key=rand_word())
        check("local: a watched key that expires and is read makes EXEC fail", got is None, repr(got))
        key = rand_word()
        a.call("SET", key, "1", "PX", "50")
        time.sleep(0.2)
        got = tx_after([key], lambda: None, key=rand_word())
        check("local: WATCH of a key that has expired already lets EXEC run",
              got == ["OK"], repr(got))
        key = rand_word()
        a.call("SET", key, "1", "PX", "5000")
        got = tx_after([key], lambda: None, key=rand_word())
        check("local: a watched key with an expiry that has not come lets EXEC run",
              got == ["OK"], repr(got))

        # A failed EXEC forgets the watched keys too.
        key = rand_word()
        tx_after([key], lambda: b.call("SET", key, "1"))
        b.call("SET", key, "2")
        a.call("MULTI")
        a.call("PING")
        check("local: a failed EXEC forgets the watched keys", a.call("EXEC") == ["PONG"])

        # EXECABORT comes before the check of the watched keys.
        key = rand_word()
        a.call("WATCH", key)
        b.call("SET", key, "1")
        a.call("MULTI")
        a.call("NOSUCH")
        check("local: EXECABORT comes before a broken WATCH",
              a.call("EXEC") == "EXECABORT Transaction discarded because of previous errors.")
        a.call("MULTI")
        check("local: UNWATCH in MULTI is queued", a.call("UNWATCH") == "QUEUED")
        check("local: and gives OK in EXEC", a.call("EXEC") == ["OK"])

        # Two clients watch one key; a write marks both.
        key = rand_word()
        c2 = Asker(srv)
        a.call("WATCH", key)
        c2.call("WATCH", key)
        b.call("SET", key, "1")
        a.call("MULTI")
        c2.call("MULTI")
        got1, got2 = a.call("EXEC"), c2.call("EXEC")
        check("local: a write marks every client that watches the key",
              got1 is None and got2 is None, "%r %r" % (got1, got2))
        c2.close()

        # A client that goes while it watches keys ends its tasks.
        def clients_now():
            info = a.call("INFO", "clients")
            for line in info.split("\r\n"):
                if line.startswith("connected_clients:"):
                    return int(line.split(":")[1])
            return -1
        base = clients_now()
        gone = srv.client()
        gone.send_raw(encode("WATCH", *["g%d" % i for i in range(5000)]))
        gone.try_read(5)
        gone.close()
        deadline = time.time() + 5
        now = clients_now()
        while time.time() < deadline and now != base:
            time.sleep(0.05)
            now = clients_now()
        check("local: a client that goes while it watches keys ends its tasks",
              now == base, "connected_clients %d, before %d" % (now, base))
        check("local: the server serves on after a client that watched 5000 keys goes",
              a.call("PING") == "PONG")
        a.close()
        b.close()
    finally:
        srv.stop()

    # W1, W2: the most keys that a client may watch, on a server of their own.
    srv = Server(prog, [])
    try:
        check("optimistic-locking (large): server starts", srv.up, srv.log_text())
        if not srv.up:
            return
        c = srv.client()
        keys = ["w%d" % i for i in range(1048575)]
        got = [None]

        def run(cmd):
            def go():
                c.send_raw(cmd)
                got[0] = c.try_read(30)
                time.sleep(0.2)
            return go
        worst, wrong, _ = measure_while(srv, run(encode("WATCH", *keys)))
        check("local: W1 WATCH of 1048575 keys stalls PING at most 0.3 s",
              worst <= 0.3 and not wrong and got[0] == "OK", "worst %.3f s, %r" % (worst, got[0]))
        second = srv.client()

        def second_watch():
            second.send_raw(encode("WATCH", *keys))
            got[0] = second.try_read(30)
            time.sleep(0.2)
        worst, wrong, _ = measure_while(srv, second_watch)
        # W3 holds rule 6 only (no mutation), so its limit is the 1 s of
        # the rule: a slower host must not fail it.
        check("local: W3 a second client that watches the same 1048575 keys stalls PING at most 1 s",
              worst <= 1.0 and not wrong and got[0] == "OK", "worst %.3f s, %r" % (worst, got[0]))
        second.close()
        check("local: a client may watch 1048576 keys", c.call("WATCH", "x0") == "OK")
        got2 = c.call("WATCH", "x1")
        check("local: a client may not watch more than 1048576 keys",
              isinstance(got2, Error) and "too many" in got2, repr(got2))
        c.call("MULTI")
        c.call("PING")
        worst, wrong, _ = measure_while(srv, run(encode("EXEC")))
        check("local: W2 EXEC that forgets 1048575 watched keys stalls PING at most 0.15 s",
              worst <= 0.15 and not wrong and got[0] == ["PONG"], "worst %.3f s, %r" % (worst, got[0]))
        check("local: after it the client may watch again", c.call("WATCH", "x1") == "OK")
        c.call("UNWATCH")

        # W4: 100 clients watch one key, and one EXEC writes it 65535 times.
        # The first write marks them all; the next ones cost no more.
        hot = []
        for _ in range(100):
            h = srv.client()
            h.call("WATCH", "hot")
            hot.append(h)
        c.call("MULTI")
        for i in range(0, 65535, 4096):
            n = min(4096, 65535 - i)
            c.send_raw(encode("INCR", "hot") * n)
            for _ in range(n):
                c.try_read(10)
        worst, wrong, _ = measure_while(srv, run(encode("EXEC")))
        check("local: W4 EXEC of 65535 INCR of a key that 100 clients watch stalls PING at most 0.5 s",
              worst <= 0.5 and not wrong and isinstance(got[0], list) and got[0][-1] == 65535,
              "worst %.3f s" % worst)
        h = hot[0]
        h.call("MULTI")
        h.call("PING")
        check("local: W4 the watchers are marked", h.call("EXEC") is None)
        for h in hot:
            h.close()
        c.close()
    finally:
        srv.stop()


def rdb_length(n):
    if n < 64:
        return bytes([n])
    if n < 16384:
        return bytes([0x40 | (n >> 8), n & 0xFF])
    return b"\x80" + n.to_bytes(4, "big")


def rdb_string(b):
    if isinstance(b, str):
        b = b.encode("latin-1")
    return rdb_length(len(b)) + b


def write_rdb(path, entries, version=b"0011", extra=b""):
    """Writes an RDB file as the Codecrafters tester does: the header,
    auxiliary fields, database 0 with its sizes, then each (key, value,
    expiry in ms or None, "ms" or "s") as a string with its expiry, and
    the end byte with a checksum of zeros."""
    out = [b"REDIS" + version]
    out.append(b"\xfa" + rdb_string("redis-ver") + rdb_string("7.2.0"))
    out.append(b"\xfa" + rdb_string("redis-bits") + b"\xc0\x40")
    out.append(b"\xfe\x00")
    out.append(b"\xfb" + rdb_length(len(entries))
               + rdb_length(sum(1 for e in entries if e[2] is not None)))
    for key, value, expiry, unit in entries:
        if expiry is not None:
            if unit == "s":
                out.append(b"\xfd" + int(expiry // 1000).to_bytes(4, "little"))
            else:
                out.append(b"\xfc" + int(expiry).to_bytes(8, "little"))
        out.append(b"\x00" + (key if isinstance(key, bytes) and key[:1] in (b"\xc0", b"\xc1", b"\xc2")
                              else rdb_string(key)))
        out.append(value if isinstance(value, bytes) and value[:1] in (b"\xc0", b"\xc1", b"\xc2", b"\xc3", b"\x81")
                   else rdb_string(value))
    out.append(extra)
    out.append(b"\xff" + b"\x00" * 8)
    with open(path, "wb") as f:
        f.write(b"".join(out))


def stages_rdb(prog):
    d = tempfile.mkdtemp(prefix="loxpp-rdb-")
    try:
        srv = Server(prog, ["--dir", d, "--dbfilename", "none.rdb"])
        try:
            check("persistence-rdb: server starts with no RDB file", srv.up, srv.log_text())
            if not srv.up:
                return
            c = srv.client()
            check("persistence-rdb-01 CONFIG GET dir", c.call("CONFIG", "GET", "dir") == ["dir", d])
            check("persistence-rdb-01 CONFIG GET dbfilename",
                  c.call("CONFIG", "GET", "dbfilename") == ["dbfilename", "none.rdb"])
            got = c.call("CONFIG", "GET", "DbFileName", "DIR", "dir")
            # Redis gives no fixed order, so the pairs are compared as a set.
            pairs = sorted(zip(got[0::2], got[1::2])) if isinstance(got, list) else got
            check("local: CONFIG GET of two names gives each once, as sent",
                  pairs == [("DIR", d), ("DbFileName", "none.rdb")], repr(got))
            check("local: a missing RDB file is an empty keyspace", c.call("KEYS", "*") == [])
            c.close()
        finally:
            srv.stop()

        def with_rdb(name, entries, **kw):
            path = os.path.join(d, name)
            write_rdb(path, entries, **kw)
            return Server(prog, ["--dir", d, "--dbfilename", name])

        def started(srv, names):
            """True when srv is up. Else each named case of the file fails
            by its name, so a load that fails does not hide them."""
            if srv.up:
                return True
            for name in names:
                check(name, False, "the server did not start: %s" % srv.log_text()[-200:])
            return False

        key = rand_word()
        srv = with_rdb("one.rdb", [(key, "orange", None, None)])
        try:
            if not started(srv, ["persistence-rdb-02 KEYS * gives the key",
                                 "persistence-rdb-03 GET gives the value"]):
                raise StopIteration
            c = srv.client()
            check("persistence-rdb-02 KEYS * gives the key", c.call("KEYS", "*") == [key])
            check("persistence-rdb-03 GET gives the value", c.call("GET", key) == "orange")
            c.close()
        except StopIteration:
            pass
        finally:
            srv.stop()

        keys = [rand_word() for _ in range(4)]
        values = [rand_word() for _ in range(4)]
        srv = with_rdb("many.rdb", [(k, v, None, None) for k, v in zip(keys, values)])
        try:
            if not started(srv, ["persistence-rdb-04 KEYS * gives every key",
                                 "persistence-rdb-05 GET gives every value"]):
                raise StopIteration
            c = srv.client()
            check("persistence-rdb-04 KEYS * gives every key", sorted(c.call("KEYS", "*")) == sorted(keys))
            check("persistence-rdb-05 GET gives every value",
                  [c.call("GET", k) for k in keys] == values)
            c.close()
        except StopIteration:
            pass
        finally:
            srv.stop()

        now = int(time.time() * 1000)
        live, dead, live_s, plain, neg = (rand_word() for _ in range(5))
        neg_s, top_ms = rand_word(), rand_word()
        srv = with_rdb("expiry.rdb", [(live, "a", now + 600000, "ms"), (dead, "b", now - 600000, "ms"),
                                      (plain, "p", None, None), (live_s, "c", now + 600000, "s"),
                                      (neg, "n", 2 ** 64 - 1, "ms"),
                                      (neg_s, "ns", 0xFFFFFFFF * 1000, "s"),
                                      (top_ms, "t", 2 ** 63 - 1, "ms")])
        try:
            if not started(srv, ["persistence-rdb-06 a key with a future expiry in ms",
                                 "persistence-rdb-06 a key that has expired",
                                 "local: a key with a future expiry in seconds",
                                 "local: a key with no expiry after an expired key",
                                 "local: a negative expiry in ms has passed",
                                 "local: a negative expiry in seconds has passed",
                                 "local: the greatest expiry in ms has not passed",
                                 "local: KEYS skips the expired key"]):
                raise StopIteration
            c = srv.client()
            check("persistence-rdb-06 a key with a future expiry in ms", c.call("GET", live) == "a")
            check("persistence-rdb-06 a key that has expired", c.call("GET", dead) is None)
            check("local: a key with a future expiry in seconds", c.call("GET", live_s) == "c")
            check("local: a key with no expiry after an expired key", c.call("GET", plain) == "p")
            check("local: a negative expiry in ms has passed", c.call("GET", neg) is None)
            check("local: a negative expiry in seconds has passed", c.call("GET", neg_s) is None)
            check("local: the greatest expiry in ms has not passed", c.call("GET", top_ms) == "t")
            check("local: KEYS skips the expired key",
                  sorted(c.call("KEYS", "*")) == sorted([live, live_s, plain, top_ms]))
            c.close()
        except StopIteration:
            pass
        finally:
            srv.stop()

        # Integer strings and the long length forms.
        long_value = "x" * 10000
        srv = with_rdb("forms.rdb", [("i8", b"\xc0\xf6", None, None), ("i16", b"\xc1\x30\xf8", None, None),
                                     ("i32", b"\xc2\x00\x94\x35\x77", None, None),
                                     (b"\xc0\x07", "int key", None, None),
                                     ("long", long_value, None, None),
                                     # LZF: a run of three bytes, then a back
                                     # reference of 6 bytes at offset 3.
                                     ("lzf", b"\xc3\x06\x09\x02abc\x80\x02", None, None),
                                     ("huge", "y" * 70000, None, None),
                                     ("len64", b"\x81" + (5).to_bytes(8, "big") + b"hello", None, None)])
        names = ["local: RDB integer strings of 1, 2 and 4 bytes", "local: an RDB key stored as an integer",
                 "local: an RDB string compressed with LZF", "local: RDB lengths of 14 and 32 bits",
                 "local: an RDB length of 64 bits"]
        if not srv.up:
            # A load that fails fails each case of the file, by its name.
            for name in names:
                check(name, False, "the server did not start: %s" % srv.log_text()[-200:])
            srv.stop()
            srv = None
        try:
            c = srv.client() if srv is not None else None
            check("local: RDB integer strings of 1, 2 and 4 bytes",
                  [c.call("GET", k) for k in ("i8", "i16", "i32")] == ["-10", "-2000", "2000000000"],
                  repr([c.call("GET", k) for k in ("i8", "i16", "i32")]))
            check("local: an RDB key stored as an integer", c.call("GET", "7") == "int key")
            check("local: an RDB string compressed with LZF", c.call("GET", "lzf") == "abcabcabc",
                  repr(c.call("GET", "lzf")))
            check("local: RDB lengths of 14 and 32 bits",
                  c.call("GET", "long") == long_value and c.call("GET", "huge") == "y" * 70000)
            check("local: an RDB length of 64 bits", c.call("GET", "len64") == "hello")
            c.close()
        except AttributeError:
            pass
        finally:
            if srv is not None:
                srv.stop()

        # A file that this server cannot load stops the start, with a message.
        def refuses(name, data, text):
            path = os.path.join(d, name)
            with open(path, "wb") as f:
                f.write(data)
            bad = Server(prog, ["--dir", d, "--dbfilename", name])
            try:
                deadline = time.time() + 5
                while bad.proc.poll() is None and time.time() < deadline:
                    time.sleep(0.05)
                code = bad.proc.poll()
                log = bad.log_text()
                check("local: %s stops the start" % text,
                      not bad.up and code == 1 and "cannot load the RDB file" in log,
                      "up %r, code %r, log %r" % (bad.up, code, log[-200:]))
            finally:
                bad.stop()
        good = os.path.join(d, "good.rdb")
        write_rdb(good, [("k", "v", None, None)])
        data = open(good, "rb").read()
        refuses("magic.rdb", b"XEDIS" + data[5:], "a file with no REDIS header")
        refuses("short.rdb", data[:-12], "a file that ends early")
        refuses("list.rdb", data[:data.index(b"\x00\x01k")] + b"\x01\x01k\x01v" + b"\xff" + b"\x00" * 8,
                "a value kind that this server has not")
        refuses("db1.rdb", data.replace(b"\xfe\x00", b"\xfe\x01"), "a key in database 1")
        refuses("version.rdb", b"REDISxxxx" + data[9:], "a version that is not a number")
        refuses("version0.rdb", b"REDIS0000" + data[9:], "version 0")
        at = data.index(b"\x00\x01k")
        for name, op, text in (("lru.rdb", b"\xf8\x05", "an LRU idle time"),
                               ("lru14.rdb", b"\xf8\x40\x05", "an LRU idle time of 14 bits"),
                               ("lfu.rdb", b"\xf9\x07", "an LFU count")):
            path = os.path.join(d, name)
            with open(path, "wb") as f:
                f.write(data[:at] + op + data[at:])
            srv = Server(prog, ["--dir", d, "--dbfilename", name])
            try:
                ok = srv.up and srv.client().call("GET", "k") == "v"
                check("local: an RDB file with %s loads" % text, ok, srv.log_text()[-200:])
            finally:
                srv.stop()
    finally:
        shutil.rmtree(d, ignore_errors=True)

    # K1, K2: KEYS on a large keyspace and with a slow pattern.
    srv = Server(prog, [])
    try:
        check("persistence-rdb (large): server starts", srv.up, srv.log_text())
        if not srv.up:
            return
        c = srv.client()
        n = 524288
        for i in range(0, n, 4096):
            c.send_raw(b"".join(encode("SET", "k%d" % j, "v") for j in range(i, i + 4096)))
            for _ in range(4096):
                c.read()
        got = [None]

        def keys_all():
            c.send_raw(encode("KEYS", "k*"))
            got[0] = c.try_read(60)
            time.sleep(0.2)
        worst, wrong, _ = measure_while(srv, keys_all)
        check("local: K1 KEYS k* of 524288 keys gives each one",
              isinstance(got[0], list) and len(got[0]) == n)
        # K1 holds rule 6 only (no mutation), so its limit is the 1 s of
        # the rule.
        check("local: K1 KEYS k* of 524288 keys stalls PING at most 1 s",
              worst <= 1.0 and not wrong, "worst %.3f s" % worst)
        # K4: a pattern that fails at the first byte does one step of glob
        # work for each key, so the filter loop must yield by itself.
        def keys_none():
            c.send_raw(encode("KEYS", "nomatch"))
            got[0] = c.try_read(60)
            time.sleep(0.2)
        worst, wrong, _ = measure_while(srv, keys_none)
        check("local: K4 KEYS nomatch of 524288 keys stalls PING at most 0.1 s",
              worst <= 0.1 and not wrong and got[0] == [], "worst %.3f s" % worst)
        # In EXEC, each KEYS spends twice the size of the keyspace from the
        # element budget of 8388608: eight run, the ninth is refused.
        c.call("MULTI")
        for _ in range(9):
            c.call("KEYS", "nomatch*")
        got[0] = c.call("EXEC")
        check("local: KEYS in EXEC spends the element budget",
              isinstance(got[0], list) and got[0][:8] == [[]] * 8 and isinstance(got[0][8], Error),
              repr(got[0])[:120])
        c.call("SET", "brief", "v", "PX", "100")
        time.sleep(0.3)
        got = c.call("KEYS", "brief*")
        check("local: KEYS skips a key that expired after the load", got == [], repr(got))
        for k in ("foo", "fob", "bar", "a*b", "axb", "f", "b", "_", "x]", "x\\", "", "\xff"):
            c.call("SET", k, "1")
        got = c.call("KEYS", "**")
        check("local: KEYS ** does not give the empty key",
              isinstance(got, list) and "" not in got and len(got) > 0, repr(got)[:60])
        check("local: KEYS of an empty pattern gives the empty key", c.call("KEYS", "") == [""])
        patterns = {"fo?": ["fob", "foo"], "[ab]*": ["a*b", "axb", "b", "bar"],
                    "[^fkx_]*": ["a*b", "axb", "b", "bar", "\xff"], "a\\*b": ["a*b"],
                    "?o?": ["fob", "foo"], "[a-b]?r": ["bar"], "[fb": ["b", "f"],
                    "[a-]": ["_"], "x[\\]]": ["x]"], "x[\\\\]": ["x\\"],
                    # Redis compares range ends as signed bytes: 0xff is -1,
                    # so this range is -1 to 97.
                    "[a-\xff]": ["_", "\xff"]}
        for pattern, want in patterns.items():
            got = c.call("KEYS", pattern)
            check("local: KEYS %s" % pattern, isinstance(got, list) and sorted(got) == want,
                  repr(got)[:80])
        # A key of 100000 bytes, and a pattern of a star and a run of 100
        # bytes that never matches at the end: about 10^7 steps of glob work.
        c.call("SET", "a" * 100000, "v")

        def keys_slow():
            c.send_raw(encode("KEYS", "*" + "a" * 100 + "b"))
            got[0] = c.try_read(120)
            time.sleep(0.2)
        worst, wrong, _ = measure_while(srv, keys_slow)
        check("local: K2 KEYS with a slow pattern gives no key", got[0] == [], repr(got[0])[:60])
        check("local: K2 KEYS with a slow pattern stalls PING at most 0.3 s",
              worst <= 0.3 and not wrong, "worst %.3f s" % worst)

        c.close()
    finally:
        srv.stop()

    # K3, on a server of its own with one key: a long set, read again at
    # each byte of the key. Each byte of the set counts as a step of glob
    # work.
    srv = Server(prog, [])
    try:
        check("persistence-rdb (K3): server starts", srv.up, srv.log_text())
        if not srv.up:
            return
        c = srv.client()
        got = [None]
        c.call("SET", "b" * 20000, "v")

        def keys_set():
            c.send_raw(encode("KEYS", "*[" + "a" * 500 + "]z"))
            got[0] = c.try_read(120)
            time.sleep(0.2)
        worst, wrong, _ = measure_while(srv, keys_set)
        check("local: K3 KEYS with a long set gives no key", got[0] == [], repr(got[0])[:60])
        check("local: K3 KEYS with a long set stalls PING at most 0.3 s",
              worst <= 0.3 and not wrong, "worst %.3f s" % worst)

        # K5: 256 keys of one byte, and a pattern of "?" and 262144 stars:
        # each key ends at once, and the rest of the pattern is stars.
        for b in range(256):
            c.call("SET", chr(b), "v")

        def keys_stars():
            c.send_raw(encode("KEYS", "?" + "*" * 262144))
            got[0] = c.try_read(120)
            time.sleep(0.2)
        worst, wrong, _ = measure_while(srv, keys_stars)
        check("local: K5 KEYS ? and 262144 stars gives the 256 keys of one byte and the K3 key",
              isinstance(got[0], list) and len(got[0]) == 257, repr(got[0])[:60])
        check("local: K5 KEYS ? and 262144 stars stalls PING at most 0.3 s",
              worst <= 0.3 and not wrong, "worst %.3f s" % worst)
        c.close()
    finally:
        srv.stop()


class SafeLink(Client):
    """A link of the fake master: a send to a replica that closed the link
    does nothing, so the cases after it fail by their names."""

    def send_raw(self, raw):
        try:
            self.sock.sendall(raw)
        except OSError:
            pass


class NullLink:
    """The link of a replica that did not connect: each read times out and
    each send does nothing, so the cases that need it fail by their names."""

    class _Sock:
        def settimeout(self, t):
            pass

        def recv(self, n):
            raise socket.timeout()

    def __init__(self):
        self.sock = NullLink._Sock()
        self.buf = b""

    def try_read(self, timeout=2):
        return TIMEOUT

    def read(self):
        raise EOFError("no link")

    def send_raw(self, raw):
        pass

    def close(self):
        pass


class FakeMaster:
    """A master played by the test: it listens, takes one replica, and
    records the handshake commands that it gets."""

    def __init__(self):
        self.lsock = socket.socket()
        self.lsock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.lsock.bind(("127.0.0.1", 0))
        self.lsock.listen(4)
        self.port = self.lsock.getsockname()[1]
        self.link = None

    def accept(self, timeout=5):
        """The link of the replica, or a NullLink when it does not connect
        in time."""
        self.lsock.settimeout(timeout)
        try:
            conn, _ = self.lsock.accept()
        except OSError:
            return NullLink()
        conn.settimeout(5)
        link = SafeLink.__new__(SafeLink)
        link.sock = conn
        link.buf = b""
        self.link = link
        return link

    def close(self):
        if self.link is not None:
            self.link.close()
        self.lsock.close()


def ack_until(link, want, seconds=3):
    """Reads REPLCONF ACK replies until one gives the offset want, or the
    time ends. A replica also sends an ACK of its own every second, so the
    answer to a GETACK can come after one of those. Returns the last
    reply."""
    deadline = time.time() + seconds
    got = None
    while time.time() < deadline:
        got = link.try_read(max(0.1, deadline - time.time()))
        if got == ["REPLCONF", "ACK", str(want)] or got is TIMEOUT or got is CLOSED:
            return got
    return got


ACK_RE = re.compile(rb"\*3\r\n\$8\r\nREPLCONF\r\n\$3\r\nACK\r\n\$\d+\r\n\d+\r\n")


def fake_handshake(link, rdb=b"REDIS0011\xff" + b"\x00" * 8, offset=0):
    """Plays the master side of the handshake on link. Returns the commands
    that the replica sent."""
    got = []
    for reply in (b"+PONG\r\n", b"+OK\r\n", b"+OK\r\n"):
        got.append(link.try_read(3))
        link.send_raw(reply)
    got.append(link.try_read(3))
    link.send_raw(b"+FULLRESYNC 75cd7bc10c49047e0d163660f3b90625b1af31dc %d\r\n" % offset)
    link.send_raw(b"$%d\r\n" % len(rdb) + rdb)
    return got


def replica_handshake(port):
    """A replica played by the test: does the handshake with the server at
    port. Returns (client, the FULLRESYNC line, the RDB bytes)."""
    r = Client(port)
    r.call("PING")
    r.call("REPLCONF", "listening-port", "6380")
    r.call("REPLCONF", "capa", "psync2")
    r.send_raw(encode("PSYNC", "?", "-1"))
    full = r._line().decode("latin-1")
    header = r._line()
    rdb = r._exact(int(header[1:]))
    return r, full, rdb


def stages_replication(prog):
    srv = Server(prog, [])
    try:
        check("replication: server starts", srv.up, srv.log_text())
        if not srv.up:
            return
        c = Asker(srv)
        check("replication-01 the server listens on --port", c.call("PING") == "PONG")
        info = c.call("INFO", "replication")
        check("replication-02 INFO replication of a master gives role:master",
              isinstance(info, str) and "role:master" in info, repr(info)[:80])
        lines = dict(l.split(":", 1) for l in info.split("\r\n") if ":" in l)
        replid = lines.get("master_replid", "")
        check("replication-04 master_replid has 40 letters and digits",
              len(replid) == 40 and replid.isalnum(), repr(replid))
        check("replication-04 master_repl_offset is 0", lines.get("master_repl_offset") == "0")

        # The master side of the handshake, with a replica played here.
        r = Client(srv.port)
        check("replication-08 PING gives PONG", r.call("PING") == "PONG")
        check("replication-08 REPLCONF listening-port gives OK",
              r.call("REPLCONF", "listening-port", "6380") == "OK")
        check("replication-08 REPLCONF capa gives OK", r.call("REPLCONF", "capa", "psync2") == "OK")
        r.send_raw(encode("PSYNC", "?", "-1"))
        full = r._line().decode("latin-1")
        parts = full.split()
        check("replication-09 PSYNC gives FULLRESYNC with the ID and offset 0",
              len(parts) == 3 and parts[0] == "+FULLRESYNC" and parts[1] == replid and parts[2] == "0",
              repr(full))
        header = r._line()
        rdb = r._exact(int(header[1:]))
        check("replication-10 then an RDB file, with no CRLF after it",
              rdb.startswith(b"REDIS") and rdb.endswith(b"\xff" + b"\x00" * 8) and r.buf == b"",
              repr(rdb[:20]))

        # Propagation to one replica and to three.
        r2, _, _ = replica_handshake(srv.port)
        r3, _, _ = replica_handshake(srv.port)
        start = time.time()
        got = c.call("WAIT", "3", "500")
        check("replication-17 WAIT with no write since the replicas came gives their number at once",
              got == 3 and time.time() - start < 0.4, repr(got))
        c.call("SET", "foo", "123")
        c.call("SET", "bar", "456")
        c.call("SET", "baz", "789")
        want = [["SET", "foo", "123"], ["SET", "bar", "456"], ["SET", "baz", "789"]]
        got1 = [r.try_read(1) for _ in range(3)]
        check("replication-11 the writes reach the replica, in order", got1 == want, repr(got1))
        got2 = [r2.try_read(1) for _ in range(3)]
        got3 = [r3.try_read(1) for _ in range(3)]
        check("replication-12 the writes reach every replica", got2 == want and got3 == want,
              "%r %r" % (got2, got3))
        c.call("GET", "foo")
        r.sock.settimeout(0.3)
        try:
            extra = r.sock.recv(4096)
        except socket.timeout:
            extra = b""
        r.sock.settimeout(5)
        check("local: a read is not propagated", extra == b"", repr(extra))

        # The effective form of each write.
        c.call("XADD", "s", "*", "f", "v")
        got = r.try_read(1)
        check("local: XADD * is propagated with the ID that the master gave",
              isinstance(got, list) and got[:2] == ["XADD", "s"] and got[2] != "*" and got[3:] == ["f", "v"],
              repr(got))
        r2.try_read(1)
        r3.try_read(1)
        c.call("RPUSH", "l", "a", "b")
        c.call("BLPOP", "l", "0")
        got = [r.try_read(1) for _ in range(2)]
        check("local: the pop of BLPOP is propagated as LPOP",
              got == [["RPUSH", "l", "a", "b"], ["LPOP", "l"]], repr(got))
        c.call("MULTI")
        c.call("SET", "t", "1")
        c.call("INCR", "t")
        c.call("EXEC")
        got = [r.try_read(1) for _ in range(4)]
        check("local: a transaction is propagated in MULTI and EXEC",
              got == [["MULTI"], ["SET", "t", "1"], ["INCR", "t"], ["EXEC"]], repr(got))
        for x in (r2, r3):
            for _ in range(6):
                x.try_read(1)
        c.call("RPUSH", "pl", "a", "b", "c", "d")
        c.call("LPUSH", "pl", "z")
        c.call("LPOP", "pl")
        c.call("LPOP", "pl", "2")
        c.call("INCR", "pi")
        c.call("INCR", "pi")
        got = [r.try_read(1) for _ in range(6)]
        check("local: LPUSH, LPOP, LPOP with a count and INCR are propagated as they came",
              got == [["RPUSH", "pl", "a", "b", "c", "d"], ["LPUSH", "pl", "z"], ["LPOP", "pl"],
                      ["LPOP", "pl", "2"], ["INCR", "pi"], ["INCR", "pi"]], repr(got))
        c.call("SET", "brief", "v", "PX", "100")
        r.try_read(1)
        time.sleep(0.3)
        c.call("GET", "brief")
        got = r.try_read(1)
        check("local: a lazy expiry on the master is propagated as DEL", got == ["DEL", "brief"], repr(got))
        waiter = srv.client()
        waiter.send_raw(encode("BLPOP", "wl", "0"))
        time.sleep(0.2)
        c.call("RPUSH", "wl", "v")
        waiter.try_read(1)
        got = [r.try_read(1) for _ in range(2)]
        check("local: a push that serves a waiting BLPOP is propagated before its LPOP",
              got == [["RPUSH", "wl", "v"], ["LPOP", "wl"]], repr(got))
        waiter.send_raw(encode("BLPOP", "wl", "0"))
        time.sleep(0.2)
        c.call("MULTI")
        c.call("RPUSH", "wl", "w")
        c.call("SET", "wx", "1")
        c.call("EXEC")
        waiter.try_read(1)
        got = [r.try_read(1) for _ in range(5)]
        check("local: the pop of a waiter that EXEC served comes after its EXEC",
              got == [["MULTI"], ["RPUSH", "wl", "w"], ["SET", "wx", "1"], ["EXEC"], ["LPOP", "wl"]], repr(got))
        c.call("MULTI")
        c.call("SET", "wy", "1")
        c.call("GET", "wy")
        c.call("EXEC")
        got = r.try_read(1)
        check("local: a transaction with one write is propagated alone, as Redis 7.0 does",
              got == ["SET", "wy", "1"], repr(got))
        waiter.close()
        c.call("SET", "dk", "1")
        r.try_read(1)
        got = c.call("DEL", "dk", "nosuch")
        check("local: DEL counts the keys that it removes", got == 1, repr(got))
        got = r.try_read(1)
        check("local: DEL is propagated", got == ["DEL", "dk", "nosuch"], repr(got))
        check("local: DEL of missing keys gives 0", c.call("DEL", "nosuch") == 0)
        c.call("SET", "de", "v", "PX", "50")
        r.try_read(1)
        time.sleep(0.2)
        check("local: DEL of an expired key gives 0", c.call("DEL", "de") == 0)
        got = r.try_read(1)
        check("local: DEL of an expired key is still propagated", got == ["DEL", "de"], repr(got))
        c.call("SET", "dw", "1")
        r.try_read(1)
        watcher = Asker(srv)
        watcher.call("WATCH", "dw")
        c.call("DEL", "dw")
        r.try_read(1)
        watcher.call("MULTI")
        watcher.call("PING")
        check("local: DEL marks a client that watches the key", watcher.call("EXEC") is None)
        watcher.close()
        r.send_raw(encode("PSYNC", "?", "-1"))
        again = r.try_read(3)
        header = r._line()
        r._exact(int(header[1:]))
        info = c.call("INFO", "replication")
        check("local: a second PSYNC on one link is still one replica",
              isinstance(again, str) and again.startswith("FULLRESYNC") and "connected_slaves:3" in info,
              "%r %r" % (again, info[:80]))
        for x in (r2, r3):
            # Drains what the replica has got so far.
            for _ in range(1000):
                if x.try_read(0.5) in (TIMEOUT, CLOSED, BAD_REPLY):
                    break

        # WAIT.
        alone = Server(prog, [])
        try:
            a = alone.client()
            check("replication-16 WAIT with no replica gives 0 at once", a.call("WAIT", "0", "60000") == 0)
            a.close()
        finally:
            alone.stop()
        c.call("SET", "w", "1")
        for x in (r, r2, r3):
            x.try_read(1)
        # The replicas played here acknowledge the offset of the master
        # before WAIT, which is what WAIT waits for.
        info = c.call("INFO", "replication")
        target = dict(l.split(":", 1) for l in info.split("\r\n") if ":" in l)["master_repl_offset"]
        start = time.time()
        c.raw().send_raw(encode("WAIT", "2", "2000"))
        acks = 0
        for x in (r, r2):
            got = x.try_read(1)
            if got == ["REPLCONF", "GETACK", "*"]:
                x.send_raw(encode("REPLCONF", "ACK", target))
                acks += 1
        reply = c.raw().try_read(3)
        check("replication-18 WAIT gives the replicas that acknowledged",
              acks == 2 and reply == 2 and time.time() - start < 1.5, "%r %r" % (acks, reply))
        r3.try_read(1)
        c.call("SET", "w2", "1")
        for x in (r, r2, r3):
            x.try_read(1)
        start = time.time()
        reply = c.call("WAIT", "3", "500")
        waited = time.time() - start
        check("replication-18 WAIT ends at its time limit with the count so far",
              reply == 0 and 0.4 <= waited <= 2.5, "%r after %.2f s" % (reply, waited))
        for x in (r, r2, r3):
            x.close()
        time.sleep(0.2)
        info = c.call("INFO", "replication")
        check("local: replicas that go leave connected_slaves", "connected_slaves:0" in info, repr(info)[:120])
        c.close()
    finally:
        srv.stop()

    # The replica side, with a master played here.
    m = FakeMaster()
    rep = Server(prog, ["--replicaof", "localhost %d" % m.port])
    try:
        check("replication: replica starts", rep.up, rep.log_text())
        if not rep.up:
            return
        link = m.accept()
        got = fake_handshake(link)
        check("replication-05 the replica sends PING", got[0] == ["PING"], repr(got))
        check("replication-06 the replica sends REPLCONF listening-port and capa",
              got[1] == ["REPLCONF", "listening-port", str(rep.port)] and got[2] == ["REPLCONF", "capa", "psync2"],
              repr(got))
        check("replication-07 the replica sends PSYNC ? -1", got[3] == ["PSYNC", "?", "-1"], repr(got))
        c = Asker(rep)
        info = c.call("INFO", "replication")
        check("replication-03 INFO replication of a replica gives role:slave",
              isinstance(info, str) and "role:slave" in info, repr(info)[:80])
        link.send_raw(encode("SET", "foo", "123") + encode("SET", "bar", "456") + encode("SET", "baz", "789"))
        time.sleep(0.3)
        check("replication-13 the replica applies the commands of the master",
              [c.call("GET", k) for k in ("foo", "bar", "baz")] == ["123", "456", "789"])
        link.sock.settimeout(0.3)
        try:
            extra = link.sock.recv(4096)
        except socket.timeout:
            extra = b""
        link.sock.settimeout(5)
        check("local: the replica sends no reply to the master", ACK_RE.sub(b"", extra) == b"",
              repr(extra))
        offset = len(encode("SET", "foo", "123") + encode("SET", "bar", "456") + encode("SET", "baz", "789"))
        link.send_raw(encode("REPLCONF", "GETACK", "*"))
        got = ack_until(link, offset)
        check("replication-15 REPLCONF GETACK gives the offset of the commands so far",
              got == ["REPLCONF", "ACK", str(offset)], repr(got))
        offset += len(encode("REPLCONF", "GETACK", "*"))
        link.send_raw(encode("PING"))
        link.send_raw(encode("REPLCONF", "GETACK", "*"))
        got = ack_until(link, offset + len(encode("PING")))
        check("replication-15 the GETACK and a PING count in the offset",
              got == ["REPLCONF", "ACK", str(offset + len(encode("PING")))], repr(got))
        got = c.call("SET", "x", "1")
        check("local: a replica refuses a write from a client",
              isinstance(got, Error) and got.startswith("READONLY"), repr(got))
        link.send_raw(encode("MULTI") + encode("SET", "t1", "a") + encode("SET", "t2", "b") + encode("EXEC"))
        time.sleep(0.3)
        check("local: the replica applies a transaction of the master",
              [c.call("GET", "t1"), c.call("GET", "t2")] == ["a", "b"])
        c.call("MULTI")
        got = c.call("SET", "x", "1")
        check("local: a replica refuses a write in MULTI", isinstance(got, Error) and got.startswith("READONLY"),
              repr(got))
        c.call("DISCARD")
        got = c.call("DEL", "foo")
        check("local: a replica refuses DEL from a client", isinstance(got, Error) and got.startswith("READONLY"),
              repr(got))
        # An expired key stays on the replica for the commands of its
        # master, until the DEL of the master: SET XX after the expiry finds
        # it, as on a redis-server replica.
        link.send_raw(encode("SET", "ex", "5", "PX", "100"))
        time.sleep(0.3)
        check("local: a client of the replica reads an expired key as missing", c.call("GET", "ex") is None)
        link.send_raw(encode("SET", "ex", "7", "XX"))
        time.sleep(0.2)
        check("local: a command of the master sees an expired key on the replica", c.call("GET", "ex") == "7")
        # A key that has expired at WATCH: its DEL from the master changes
        # nothing that the client saw, as on a redis-server 7.0 replica. A
        # write of it does; Redis 7.0 marks no client then (see the README).
        link.send_raw(encode("SET", "wx", "5", "PX", "100") + encode("SET", "wy", "5", "PX", "100"))
        time.sleep(0.3)
        c.call("WATCH", "wx")
        link.send_raw(encode("DEL", "wx"))
        time.sleep(0.2)
        c.call("MULTI")
        c.call("GET", "wx")
        got = c.call("EXEC")
        check("local: the DEL of a key that had expired at WATCH does not fail EXEC", got == [None], repr(got))
        c.call("WATCH", "wy")
        link.send_raw(encode("SET", "wy", "6"))
        time.sleep(0.2)
        c.call("MULTI")
        c.call("GET", "wy")
        got = c.call("EXEC")
        check("local: a write of a key that had expired at WATCH fails EXEC", got is None, repr(got))
        # After such a DEL, the client still watches the key: a later write
        # fails EXEC. Both forms of an entry in watchers: one client, and
        # two clients.
        c2 = Asker(rep)
        link.send_raw(encode("SET", "wq", "5", "PX", "100") + encode("SET", "wz", "5", "PX", "100"))
        time.sleep(0.3)
        c.call("WATCH", "wq")
        link.send_raw(encode("DEL", "wq") + encode("SET", "wq", "8"))
        time.sleep(0.2)
        c.call("MULTI")
        got = c.call("EXEC")
        check("local: after the DEL of a key that had expired at WATCH, one watcher still sees a write",
              got is None, repr(got))
        c.call("WATCH", "wz")
        c2.call("WATCH", "wz")
        link.send_raw(encode("DEL", "wz") + encode("SET", "wz", "8"))
        time.sleep(0.2)
        c.call("MULTI")
        c2.call("MULTI")
        got = [c.call("EXEC"), c2.call("EXEC")]
        check("local: after the DEL of a key that had expired at WATCH, two watchers still see a write",
              got == [None, None], repr(got))
        c2.close()
        # A transaction of the master: no client of the replica sees its
        # writes before its EXEC.
        link.send_raw(encode("MULTI") + encode("SET", "ma", "1"))
        time.sleep(0.3)
        check("local: a client of the replica sees no write of a transaction before its EXEC",
              c.call("GET", "ma") is None)
        link.send_raw(encode("EXEC"))
        time.sleep(0.2)
        check("local: after EXEC the client sees the write", c.call("GET", "ma") == "1")
        # The replica sends an ACK of its own every second.
        link.sock.settimeout(1.6)
        try:
            beat = link.sock.recv(4096)
        except socket.timeout:
            beat = b""
        link.sock.settimeout(5)
        check("local: the replica sends REPLCONF ACK every second", ACK_RE.search(beat) is not None, repr(beat))
        # A client that watches a key sees a full resync as a write.
        c.call("WATCH", "foo")
        # When the link breaks, the replica connects again and syncs.
        link.close()
        m.link = None
        link = m.accept(timeout=5)
        # The new RDB holds a key that has expired. A replica keeps it for
        # the commands of its master, as a redis-server replica does.
        d = tempfile.mkdtemp(prefix="loxpp-repl-rdb-")
        path = os.path.join(d, "exp.rdb")
        write_rdb(path, [("rk", "5", 1000, "ms")])
        got = fake_handshake(link, rdb=open(path, "rb").read())
        shutil.rmtree(d, ignore_errors=True)
        check("local: the replica connects again after the link breaks",
              got[3] == ["PSYNC", "?", "-1"], repr(got))
        time.sleep(0.3)
        link.send_raw(encode("SET", "rk", "7", "XX"))
        time.sleep(0.2)
        check("local: a replica keeps an expired key of the RDB for its master", c.call("GET", "rk") == "7")
        c.call("MULTI")
        c.call("PING")
        check("local: a full resync makes EXEC of a client that watches a key fail", c.call("EXEC") is None)
        check("local: a full resync replaces the keyspace", c.call("GET", "foo") is None)
        c.close()
    finally:
        rep.stop()
        m.close()

    # The bounds and the liveness of replication, on a server of their own.
    srv = Server(prog, [])
    try:
        c = Asker(srv)

        def slaves():
            info = c.call("INFO", "replication")
            for line in info.split("\r\n"):
                if line.startswith("connected_slaves:"):
                    return int(line.split(":")[1])
            return -1

        def clients_now():
            info = c.call("INFO", "clients")
            for line in info.split("\r\n"):
                if line.startswith("connected_clients:"):
                    return int(line.split(":")[1])
            return -1

        # RP1: a replica that does not read is dropped over the hard limit
        # of 256 MB, so the log does not grow without bound (README rule 4).
        lazy, _, _ = replica_handshake(srv.port)
        big = "x" * (1 << 20)
        for i in range(300):
            c.call("SET", "k", big + str(i))
        time.sleep(0.3)
        check("local: RP1 a replica that does not read is dropped over the hard limit",
              slaves() == 0, "connected_slaves %d" % slaves())
        lazy.close()
        time.sleep(0.2)

        # An entry that is appended while the replica session suspends in a
        # write reaches the replica with no later write.
        slow, _, _ = replica_handshake(srv.port)
        c.call("SET", "huge", "h" * (32 << 20))
        time.sleep(0.3)
        c.call("SET", "z", "1")
        tail = encode("SET", "z", "1")
        slow.sock.settimeout(3)
        data = b""
        deadline = time.time() + 6
        try:
            while time.time() < deadline and not data.endswith(tail):
                chunk = slow.sock.recv(1 << 20)
                if not chunk:
                    break
                data += chunk[-len(tail) * 2:] if len(data) > 1 << 20 else chunk
                if len(data) > 1 << 21:
                    data = data[-len(tail) * 2:]
        except OSError:
            pass
        check("local: a write made while the replica session waits in a write reaches the replica",
              data.endswith(tail), repr(data[-60:]))
        slow.close()
        time.sleep(0.2)

        # RP2: propagation of an RPUSH of 1048573 elements to a replica
        # that reads (rule 6 only).
        reader, _, _ = replica_handshake(srv.port)
        stop = threading.Event()
        got_bytes = [0]

        def drain():
            reader.sock.settimeout(0.5)
            while not stop.is_set():
                try:
                    chunk = reader.sock.recv(1 << 20)
                    if not chunk:
                        break
                    got_bytes[0] += len(chunk)
                except OSError:
                    pass
        th = threading.Thread(target=drain)
        th.start()
        n = 1048573
        cmd = encode("RPUSH", "rp2", *(["abcd"] * n))
        out = [None]

        def push():
            c.raw().send_raw(cmd)
            out[0] = c.raw().try_read(60)
            time.sleep(0.5)
        worst, wrong, _ = measure_while(srv, push)
        deadline = time.time() + 10
        while got_bytes[0] < len(cmd) and time.time() < deadline:
            time.sleep(0.05)
        stop.set()
        th.join(10)
        check("local: RP2 an RPUSH of 1048573 elements reaches the replica whole",
              out[0] == n and got_bytes[0] >= len(cmd), "%r, %d of %d bytes" % (out[0], got_bytes[0], len(cmd)))
        check("local: RP2 that RPUSH with a replica stalls PING at most 1 s",
              worst <= 1.0 and not wrong, "worst %.3f s" % worst)
        reader.close()
        time.sleep(0.3)

        # A client that goes during WAIT ends its tasks.
        keep, _, _ = replica_handshake(srv.port)
        c.call("SET", "w", "1")
        base = clients_now()
        gone = srv.client()
        gone.send_raw(encode("WAIT", "1", "0") + encode("PING") * 100)
        time.sleep(0.2)
        gone.close()
        deadline = time.time() + 5
        now = clients_now()
        while time.time() < deadline and now != base:
            time.sleep(0.05)
            now = clients_now()
        check("local: a client that goes during WAIT ends its tasks",
              now == base, "connected_clients %d, before %d" % (now, base))
        keep.close()
        c.close()
    finally:
        srv.stop()

    # RP3: the soft limit, with a soft time of 3 s, so that a slow host
    # writes the 80 MB in time. A replica over 64 MB
    # stays while the soft time runs (one large write must not drop it),
    # and goes when it is still over after that time.
    srv = Server(prog, ["--replica-soft-seconds", "3"])
    try:
        c = Asker(srv)

        def slaves3():
            info = c.call("INFO", "replication")
            for line in info.split("\r\n"):
                if line.startswith("connected_slaves:"):
                    return int(line.split(":")[1])
            return -1
        lazy, _, _ = replica_handshake(srv.port)
        big = "y" * (1 << 20)
        for i in range(80):
            c.call("SET", "s", big + str(i))
        check("local: RP3 a replica over the soft limit stays while the soft time runs",
              slaves3() == 1, "connected_slaves %d" % slaves3())
        time.sleep(3.5)
        c.call("SET", "s", "after")
        time.sleep(0.2)
        check("local: RP3 a replica that is still over the soft limit after its time is dropped",
              slaves3() == 0, "connected_slaves %d" % slaves3())
        lazy.close()
        c.close()
    finally:
        srv.stop()

    # GETACK with no command first (stage 14), on a new replica.
    m = FakeMaster()
    rep = Server(prog, ["--replicaof", "localhost %d" % m.port])
    try:
        link = m.accept()
        fake_handshake(link)
        link.send_raw(encode("REPLCONF", "GETACK", "*"))
        got = ack_until(link, 0)
        check("replication-14 REPLCONF GETACK with no command first gives ACK 0",
              got == ["REPLCONF", "ACK", "0"], repr(got))
    finally:
        rep.stop()
        m.close()

    # The offset of a replica goes on from the offset of FULLRESYNC.
    m = FakeMaster()
    rep = Server(prog, ["--replicaof", "localhost %d" % m.port])
    try:
        link = m.accept()
        fake_handshake(link, offset=1000)
        link.send_raw(encode("REPLCONF", "GETACK", "*"))
        got = ack_until(link, 1000)
        check("local: the offset of a replica starts at the offset of FULLRESYNC",
              got == ["REPLCONF", "ACK", "1000"], repr(got))
    finally:
        rep.stop()
        m.close()

    # A full resync of 524288 keys on a live replica: the load yields, and
    # the clients see the old keyspace until it ends (rule 6).
    d = tempfile.mkdtemp(prefix="loxpp-resync-")
    m = FakeMaster()
    rep = Server(prog, ["--replicaof", "localhost %d" % m.port])
    try:
        path = os.path.join(d, "big.rdb")
        write_rdb(path, [("r%d" % i, "v", None, None) for i in range(524288)])
        big = open(path, "rb").read()
        link = m.accept()
        out = [None]

        def resync():
            fake_handshake(link, rdb=big)
            time.sleep(1.5)
        worst, wrong, _ = measure_while(rep, resync)
        c = Asker(rep)
        deadline = time.time() + 10
        loaded = c.call("GET", "r524287")
        while loaded != "v" and time.time() < deadline:
            time.sleep(0.1)
            loaded = c.call("GET", "r524287")
        check("local: a full resync of 524288 keys loads them", loaded == "v")
        check("local: a full resync of 524288 keys stalls PING at most 1 s",
              worst <= 1.0 and not wrong, "worst %.3f s" % worst)
        c.close()
    finally:
        rep.stop()
        m.close()
        shutil.rmtree(d, ignore_errors=True)


def read_bytes(path):
    try:
        with open(path, "rb") as f:
            return f.read()
    except OSError:
        return None


def write_bytes(path, data):
    with open(path, "wb") as f:
        f.write(data)


def stages_aof(prog):
    """The persistence-aof group, and the local cases of the AOF."""
    d = tempfile.mkdtemp(prefix="loxpp-aof-")
    try:
        stages_aof_in(prog, d)
    finally:
        shutil.rmtree(d, ignore_errors=True)


def aof_started(srv, names):
    """True when srv is up. Else each named case fails by its name."""
    if srv.up:
        return True
    for name in names:
        check(name, False, "the server did not start: %s" % srv.log_text()[-300:])
    return False


def aof_exits(prog, args, name, cwd=None):
    """Checks that the server refuses to start: it exits with no listener
    and a message."""
    srv = Server(prog, args, cwd=cwd)
    try:
        code = srv.proc.wait(timeout=10) if not srv.up else None
        check(name, not srv.up and code not in (None, 0) and "redis.lox:" in srv.log_text(),
              "up=%s code=%r log=%r" % (srv.up, code, srv.log_text()[-200:]))
    except subprocess.TimeoutExpired:
        check(name, False, "the server did not exit")
    finally:
        srv.stop()


def stages_aof_in(prog, d):
    # Stage 1: the defaults, with the start directory as dir.
    home = os.path.join(d, "home")
    os.mkdir(home)
    names01 = ["persistence-aof-01 CONFIG GET dir gives the start directory",
               "persistence-aof-01 CONFIG GET appendonly gives no",
               "persistence-aof-01 CONFIG GET appenddirname gives appendonlydir",
               "persistence-aof-01 CONFIG GET appendfilename gives appendonly.aof",
               "persistence-aof-01 CONFIG GET appendfsync gives everysec",
               "local: with appendonly no, no AOF directory is made"]
    srv = Server(prog, [], cwd=home)
    try:
        if aof_started(srv, names01):
            c = Asker(srv)
            check(names01[0], c.call("CONFIG", "GET", "dir") == ["dir", os.path.realpath(home)],
                  repr(c.call("CONFIG", "GET", "dir")))
            check(names01[1], c.call("CONFIG", "GET", "appendonly") == ["appendonly", "no"])
            check(names01[2], c.call("CONFIG", "GET", "appenddirname") == ["appenddirname", "appendonlydir"])
            check(names01[3], c.call("CONFIG", "GET", "appendfilename") == ["appendfilename", "appendonly.aof"])
            check(names01[4], c.call("CONFIG", "GET", "appendfsync") == ["appendfsync", "everysec"])
            c.call("SET", "k", "v")
            check(names01[5], os.listdir(home) == [], repr(os.listdir(home)))
            c.close()
    finally:
        srv.stop()

    # Stage 2: the flags. Stages 3-5: the directory, the empty file and the
    # manifest, made at start.
    base = os.path.join(d, "s2")
    os.mkdir(base)
    adir, aname = "ad" + rand_word(4), "af" + rand_word(4) + ".aof"
    names = ["persistence-aof-02 CONFIG GET gives each flag",
             "persistence-aof-03 the AOF directory is made at start",
             "persistence-aof-04 an empty incremental file is made at start",
             "persistence-aof-05 the manifest names the incremental file"]
    srv = Server(prog, ["--dir", base, "--appendonly", "yes", "--appenddirname", adir,
                        "--appendfilename", aname, "--appendfsync", "everysec"])
    try:
        if aof_started(srv, names):
            path = os.path.join(base, adir)
            # Read before any command: the server makes them at start.
            isdir = os.path.isdir(path)
            incr = read_bytes(os.path.join(path, aname + ".1.incr.aof"))
            manifest = read_bytes(os.path.join(path, aname + ".manifest"))
            c = Asker(srv)
            got = [c.call("CONFIG", "GET", n) for n in
                   ("dir", "appendonly", "appenddirname", "appendfilename", "appendfsync")]
            check(names[0], got == [["dir", base], ["appendonly", "yes"], ["appenddirname", adir],
                                    ["appendfilename", aname], ["appendfsync", "everysec"]], repr(got))
            check(names[1], isdir)
            check(names[2], incr == b"", repr(incr))
            check(names[3], manifest == ("file %s.1.incr.aof seq 1 type i\n" % aname).encode(), repr(manifest))
            c.close()
    finally:
        srv.stop()
    srv = Server(prog, ["--appendfsync", "always"], cwd=home)
    try:
        if aof_started(srv, ["persistence-aof-02 a flag that is not given keeps its default"]):
            c = Asker(srv)
            got = [c.call("CONFIG", "GET", n) for n in ("appendonly", "appendfsync")]
            check("persistence-aof-02 a flag that is not given keeps its default",
                  got == [["appendonly", "no"], ["appendfsync", "always"]], repr(got))
            c.close()
    finally:
        srv.stop()
    # A directory that is there already is not an error.
    srv = Server(prog, ["--dir", base, "--appendonly", "yes", "--appenddirname", adir,
                        "--appendfilename", aname])
    check("persistence-aof-03 an AOF directory that is there already is not an error", srv.up,
          srv.log_text()[-200:])
    srv.stop()
    srv = Server(prog, ["--dir", base, "--appendonly", "yes"])
    try:
        check("local: the default AOF names are used",
              srv.up and read_bytes(os.path.join(base, "appendonlydir", "appendonly.aof.manifest"))
              == b"file appendonly.aof.1.incr.aof seq 1 type i\n")
    finally:
        srv.stop()

    # Stages 6-8: the writes go to the file that the manifest names, before
    # the reply.
    def prepared(tag):
        base = os.path.join(d, tag)
        adir, aname, rname = "ad" + rand_word(4), "af" + rand_word(4) + ".aof", rand_word(6)
        os.makedirs(os.path.join(base, adir))
        write_bytes(os.path.join(base, adir, aname + ".manifest"),
                    ("file %s.1.incr.aof seq 1 type i\n" % rname).encode())
        write_bytes(os.path.join(base, adir, rname + ".1.incr.aof"), b"")
        return base, adir, aname, os.path.join(base, adir, rname + ".1.incr.aof")

    base, adir, aname, rpath = prepared("s6")
    flags = ["--dir", base, "--appendonly", "yes", "--appenddirname", adir, "--appendfilename", aname,
             "--appendfsync", "always"]
    names = ["persistence-aof-06 SET goes to the file that the manifest names before the reply",
             "persistence-aof-06 the default file is not used",
             "persistence-aof-07 two writes go to the file in order",
             "persistence-aof-08 only the writes go to the file"]
    srv = Server(prog, flags)
    try:
        if aof_started(srv, names):
            c = Asker(srv)
            k1, v1, k2, v2 = rand_word(), rand_word(), rand_word(), rand_word()
            c.call("SET", k1, v1)
            got = read_bytes(rpath)
            check(names[0], got == encode("SET", k1, v1), repr(got))
            check(names[1], not os.path.exists(os.path.join(base, adir, aname + ".1.incr.aof")))
            c.call("SET", k2, v2)
            got = read_bytes(rpath)
            check(names[2], got == encode("SET", k1, v1) + encode("SET", k2, v2), repr(got))
            c.call("GET", k1)
            c.call("PING")
            c.call("ECHO", "hello")
            c.call("CONFIG", "GET", "appendfsync")
            k3 = rand_word()
            c.call("SET", k3, "3")
            got = read_bytes(rpath)
            check(names[3], got == encode("SET", k1, v1) + encode("SET", k2, v2) + encode("SET", k3, "3"),
                  repr(got))
            c.close()
    finally:
        srv.stop()

    # Each kind of write goes to the file in the form that it took effect in.
    base, adir, aname, rpath = prepared("forms")
    flags = ["--dir", base, "--appendonly", "yes", "--appenddirname", adir, "--appendfilename", aname,
             "--appendfsync", "everysec"]
    srv = Server(prog, flags)
    forms = ["local: with everysec too, a write is in the file before its reply",
             "local: SET with PX goes to the file with an absolute PXAT",
             "local: INCR, RPUSH, LPUSH, LPOP and DEL go to the file",
             "local: XADD * goes to the file with its ID",
             "local: a served BLPOP goes to the file as LPOP",
             "local: a transaction goes to the file in MULTI and EXEC",
             "local: a command that fails or writes nothing does not go to the file",
             "local: a lazy expiry goes to the file as DEL"]
    try:
        if aof_started(srv, forms):
            c = Asker(srv)
            c.call("SET", "a", "1")
            check(forms[0], read_bytes(rpath) == encode("SET", "a", "1"), repr(read_bytes(rpath)))
            mark = len(read_bytes(rpath))
            before = int(time.time() * 1000)
            c.call("SET", "p", "v", "PX", "100000")
            after = int(time.time() * 1000)
            tail = read_bytes(rpath)[mark:]
            ok = False
            m = re.match(rb"\*5\r\n\$3\r\nSET\r\n\$1\r\np\r\n\$1\r\nv\r\n\$4\r\nPXAT\r\n\$13\r\n(\d{13})\r\n$", tail)
            if m:
                at = int(m.group(1))
                ok = before + 100000 - 1 <= at <= after + 100000
            check(forms[1], ok, repr(tail))
            mark = len(read_bytes(rpath))
            c.call("INCR", "n")
            c.call("RPUSH", "l", "x", "y")
            c.call("LPUSH", "l", "w")
            c.call("LPOP", "l")
            c.call("DEL", "a", "missing")
            want = (encode("INCR", "n") + encode("RPUSH", "l", "x", "y") + encode("LPUSH", "l", "w")
                    + encode("LPOP", "l") + encode("DEL", "a", "missing"))
            got = read_bytes(rpath)[mark:]
            check(forms[2], got == want, repr(got))
            mark = len(read_bytes(rpath))
            xid = c.call("XADD", "s", "*", "f", "v")
            got = read_bytes(rpath)[mark:]
            check(forms[3], isinstance(xid, str) and got == encode("XADD", "s", xid, "f", "v"), repr(got))
            mark = len(read_bytes(rpath))
            b = Asker(srv)
            b.c.send_raw(encode("BLPOP", "q", "0"))
            time.sleep(0.2)
            c.call("RPUSH", "q", "e")
            served = b.c.try_read(2)
            got = read_bytes(rpath)[mark:]
            check(forms[4], served == ["q", "e"] and got == encode("RPUSH", "q", "e") + encode("LPOP", "q"),
                  repr((served, got)))
            b.close()
            mark = len(read_bytes(rpath))
            c.call("MULTI")
            c.call("SET", "t", "1")
            c.call("GET", "t")
            c.call("INCR", "t")
            c.call("EXEC")
            got = read_bytes(rpath)[mark:]
            check(forms[5], got == encode("MULTI") + encode("SET", "t", "1") + encode("INCR", "t") + encode("EXEC"),
                  repr(got))
            mark = len(read_bytes(rpath))
            c.call("MULTI")
            c.call("GET", "t")
            c.call("SET", "t1", "1")
            c.call("EXEC")
            got = read_bytes(rpath)[mark:]
            check("local: a transaction with one write goes to the file alone, as Redis 7.0 writes it",
                  got == encode("SET", "t1", "1"), repr(got))
            mark = len(read_bytes(rpath))
            c.call("INCR", "l")
            c.call("SET", "t", "2", "NX")
            c.call("SET", "x", "1", "BAD")
            c.call("LPOP", "missing")
            c.call("DEL", "missing")
            c.call("MULTI")
            c.call("GET", "t")
            c.call("EXEC")
            got = read_bytes(rpath)[mark:]
            check(forms[6], got == b"", repr(got))
            c.call("SET", "soon", "v", "PX", "50")
            time.sleep(0.15)
            mark = len(read_bytes(rpath))
            c.call("GET", "soon")
            got = read_bytes(rpath)[mark:]
            check(forms[7], got == encode("DEL", "soon"), repr(got))
            c.close()
    finally:
        srv.stop()

    # WAIT sends GETACK to the replicas; it does not go to the file.
    base, adir, aname, rpath = prepared("wait")
    srv = Server(prog, ["--dir", base, "--appendonly", "yes", "--appenddirname", adir, "--appendfilename", aname])
    try:
        if aof_started(srv, ["local: the GETACK of WAIT does not go to the file"]):
            r, _, _ = replica_handshake(srv.port)
            c = Asker(srv)
            c.call("SET", "w", "1")
            c.call("WAIT", "1", "100")
            c.call("SET", "w", "2")
            got = read_bytes(rpath)
            check("local: the GETACK of WAIT does not go to the file",
                  got == encode("SET", "w", "1") + encode("SET", "w", "2"), repr(got))
            r.close()
            c.close()
    finally:
        srv.stop()

    # Stages 9-10: the replay at start, from the file that the manifest names.
    def with_aof(tag, data, extra_files=None, manifest=None):
        base = os.path.join(d, tag)
        adir, aname, rname = "ad" + rand_word(4), "af" + rand_word(4) + ".aof", rand_word(6)
        os.makedirs(os.path.join(base, adir))
        if manifest is None:
            manifest = "file %s.1.incr.aof seq 1 type i\n" % rname
        write_bytes(os.path.join(base, adir, aname + ".manifest"), manifest.encode())
        if data is not None:
            write_bytes(os.path.join(base, adir, rname + ".1.incr.aof"), data)
        for name, content in (extra_files or {}).items():
            write_bytes(os.path.join(base, adir, name), content)
        flags = ["--dir", base, "--appendonly", "yes", "--appenddirname", adir, "--appendfilename", aname]
        return flags, os.path.join(base, adir), rname + ".1.incr.aof"

    k, v = rand_word(), rand_word()
    flags, _, _ = with_aof("s9", encode("SET", k, v))
    srv = Server(prog, flags)
    try:
        if aof_started(srv, ["persistence-aof-09 GET gives the value of the replayed SET"]):
            c = Asker(srv)
            check("persistence-aof-09 GET gives the value of the replayed SET", c.call("GET", k) == v)
            c.close()
    finally:
        srv.stop()
    keys = [rand_word() for _ in range(5)]
    vals = [rand_word() for _ in range(5)]
    flags, _, _ = with_aof("s10", b"".join(encode("SET", a, b) for a, b in zip(keys, vals)))
    srv = Server(prog, flags)
    try:
        if aof_started(srv, ["persistence-aof-10 GET gives the value of each replayed SET"]):
            c = Asker(srv)
            got = [c.call("GET", a) for a in keys]
            check("persistence-aof-10 GET gives the value of each replayed SET", got == vals, repr(got))
            c.close()
    finally:
        srv.stop()

    # A round trip: the writes of one run come back in the next, and the
    # next run appends to the same file.
    base, adir, aname, rpath = prepared("trip")
    flags = ["--dir", base, "--appendonly", "yes", "--appenddirname", adir, "--appendfilename", aname]
    srv = Server(prog, flags)
    trip = ["local: a restart gives back strings, lists, streams and counters",
            "local: a restart gives back a transaction",
            "local: a restart keeps an absolute expiry",
            "local: a key that expired before the restart stays gone",
            "local: the next run appends to the same file",
            "local: a restart keeps an expiry whose absolute time has 16 digits"]
    try:
        if aof_started(srv, trip):
            c = Asker(srv)
            c.call("SET", "s", "v")
            c.call("RPUSH", "l", "a", "b", "c")
            c.call("LPOP", "l")
            c.call("XADD", "x", "1-1", "f", "v")
            c.call("INCR", "n")
            c.call("INCR", "n")
            c.call("MULTI")
            c.call("SET", "m1", "1")
            c.call("SET", "m2", "2")
            c.call("EXEC")
            c.call("SET", "e", "v", "PX", "100000")
            c.call("SET", "gone", "v", "PX", "100")
            c.call("SET", "far", "v", "EX", "999999999999")
            c.call("SET", "farms", "v", "PX", "999999999999999")
            c.call("SET", "farat", "v", "EXAT", "9999999999999")
            c.close()
    finally:
        srv.stop()
    time.sleep(0.2)
    size = len(read_bytes(rpath) or b"")
    srv = Server(prog, flags)
    try:
        if aof_started(srv, trip):
            c = Asker(srv)
            got = [c.call("GET", "s"), c.call("LRANGE", "l", "0", "-1"), c.call("XRANGE", "x", "-", "+"),
                   c.call("GET", "n")]
            check(trip[0], got == ["v", ["b", "c"], [["1-1", ["f", "v"]]], "2"], repr(got))
            check(trip[1], [c.call("GET", "m1"), c.call("GET", "m2")] == ["1", "2"])
            check(trip[2], c.call("GET", "e") == "v")
            check(trip[3], c.call("GET", "gone") is None and c.call("KEYS", "gone") == [])
            check("local: a restart keeps an expiry whose absolute time has 16 digits",
                  [c.call("GET", "far"), c.call("GET", "farms"), c.call("GET", "farat")] == ["v", "v", "v"])
            c.call("SET", "after", "1")
            got = read_bytes(rpath)
            check(trip[4], got is not None and len(got) > size and got.endswith(encode("SET", "after", "1"))
                  and got.startswith(encode("SET", "s", "v")), repr(got[-60:] if got else got))
            c.close()
    finally:
        srv.stop()

    # The replay: the forms that Redis writes, and files that end early.
    replay = [
        ("local: the replay accepts SELECT 0 and annotation lines",
         encode("SELECT", "0") + b"#TS:1700000000\r\n" + encode("SET", "r", "1"), {}, None,
         lambda c: c.call("GET", "r") == "1"),
        ("local: during the replay an expired key is still there for the next command",
         encode("SET", "r", "5", "PXAT", "1000") + encode("SET", "r", "7", "XX"), {}, None,
         lambda c: c.call("GET", "r") == "7"),
        ("local: the replay applies a transaction",
         encode("MULTI") + encode("SET", "r", "1") + encode("INCR", "r") + encode("EXEC"), {}, None,
         lambda c: c.call("GET", "r") == "2"),
        ("local: the replay loads a base RDB file and then the incremental files in order",
         encode("SET", "r", "2"), {"b.rdb": None, "i0.aof": encode("SET", "r", "1") + encode("SET", "q", "1")},
         "file b.rdb seq 1 type b\nfile i0.aof seq 1 type i\nfile {R} seq 2 type i\n",
         lambda c: [c.call("GET", "base"), c.call("GET", "r"), c.call("GET", "q")] == ["b", "2", "1"]),
        ("local: an RDB base keeps an expired key for the commands after it",
         encode("SET", "base", "7", "XX"), {"b.rdb": "expired"},
         "file b.rdb seq 1 type b\nfile {R} seq 2 type i\n",
         lambda c: c.call("GET", "base") == "7"),
        ("local: the manifest keys have no case, as Redis reads them",
         encode("SET", "r", "1"), {}, "FILE {R} SEQ 1 TYPE i\n",
         lambda c: c.call("GET", "r") == "1"),
        ("local: the replay goes on after EXEC with no MULTI",
         encode("EXEC") + encode("SET", "r", "1"), {}, None,
         lambda c: c.call("GET", "r") == "1"),
        ("local: the replay loads a base AOF file and skips a history file",
         encode("INCR", "r"), {"b.aof": encode("SET", "r", "10"), "h.aof": encode("SET", "r", "99")},
         "file h.aof seq 1 type h\nfile b.aof seq 2 type b\nfile {R} seq 3 type i\n",
         lambda c: c.call("GET", "r") == "11"),
    ]
    for name, data, extra, manifest, test in replay:
        files = {}
        for fname, content in extra.items():
            if content is None or content == "expired":
                p = os.path.join(d, "tmp.rdb")
                write_rdb(p, [("base", "b", 1000 if content else None, "ms" if content else None)])
                content = read_bytes(p)
            files[fname] = content
        rname = rand_word(6)
        man = manifest.replace("{R}", rname + ".1.incr.aof") if manifest else None
        tag = "rp" + rand_word(4)
        base = os.path.join(d, tag)
        adir, aname = "ad", "af.aof"
        os.makedirs(os.path.join(base, adir))
        write_bytes(os.path.join(base, adir, aname + ".manifest"),
                    (man or "file %s.1.incr.aof seq 1 type i\n" % rname).encode())
        write_bytes(os.path.join(base, adir, rname + ".1.incr.aof"), data)
        for fname, content in files.items():
            write_bytes(os.path.join(base, adir, fname), content)
        srv = Server(prog, ["--dir", base, "--appendonly", "yes", "--appenddirname", adir,
                            "--appendfilename", aname])
        try:
            if aof_started(srv, [name]):
                c = Asker(srv)
                check(name, test(c))
                c.close()
        finally:
            srv.stop()

    # A last file that ends inside a command, or inside MULTI, is cut back
    # to its last whole command, and the server starts.
    for name, data, keep in [
            ("local: a last file that ends inside a command is cut back",
             encode("SET", "r", "1") + encode("SET", "r", "2")[:-3], encode("SET", "r", "1")),
            ("local: a last file that ends inside MULTI is cut back to before MULTI",
             encode("SET", "r", "1") + encode("MULTI") + encode("SET", "r", "2"), encode("SET", "r", "1")),
            ("local: a last file that ends inside a header line is cut back",
             encode("SET", "r", "1") + b"*3\r\n$3", encode("SET", "r", "1")),
            ("local: an annotation that ends the file with no newline is kept",
             encode("SET", "r", "1") + b"#TS:1700000000", encode("SET", "r", "1") + b"#TS:1700000000")]:
        flags, path, rfile = with_aof("cut" + rand_word(4), data)
        srv = Server(prog, flags)
        try:
            if aof_started(srv, [name]):
                c = Asker(srv)
                got = [c.call("GET", "r"), read_bytes(os.path.join(path, rfile))]
                c.call("SET", "z", "1")
                got.append(read_bytes(os.path.join(path, rfile)))
                check(name, got == ["1", keep, keep + encode("SET", "z", "1")], repr(got))
                c.close()
        finally:
            srv.stop()

    # The files that the server refuses at start.
    for name, data, extra, manifest in [
            ("local: the replay refuses a file that is not RESP", encode("SET", "r", "1") + b"SET r 2\r\n", {}, None),
            ("local: the replay refuses an unknown command", encode("FOO", "r"), {}, None),
            ("local: the replay refuses SELECT of another database", encode("SELECT", "1"), {}, None),
            ("local: a file that ends inside a command and is not the last is refused",
             encode("SET", "r", "1"), {"i0.aof": encode("SET", "q", "1")[:-2]},
             "file i0.aof seq 1 type i\nfile {R} seq 2 type i\n"),
            ("local: a manifest that names a file in another directory is refused",
             encode("SET", "r", "1"), {"../x.aof": encode("SET", "r", "2")}, "file ../x.aof seq 1 type i\n"),
            ("local: a manifest line with an unknown type is refused", encode("SET", "r", "1"), {},
             "file {R} seq 1 type z\n"),
            ("local: a manifest line with a word that has no value is refused", encode("SET", "r", "1"), {},
             "file {R} seq 1 type i x\n"),
            ("local: a manifest that names a missing file is refused", encode("SET", "r", "1"), {},
             "file nothere.aof seq 1 type i\n"),
            ("local: a manifest line with no seq is refused", encode("SET", "r", "1"), {},
             "file {R} type b\n"),
            ("local: an empty manifest is refused", encode("SET", "r", "1"), {}, ""),
            ("local: a manifest with a blank line is refused", encode("SET", "r", "1"), {},
             "file {R} seq 1 type i\n\n"),
            ("local: a manifest with two base files is refused", encode("SET", "r", "1"),
             {"b1.aof": encode("SET", "q", "1"), "b2.aof": encode("SET", "q", "2")},
             "file b1.aof seq 1 type b\nfile b2.aof seq 1 type b\nfile {R} seq 1 type i\n"),
            ("local: incremental files out of the order of their seq are refused", encode("SET", "r", "1"),
             {"i2.aof": encode("SET", "q", "1")}, "file i2.aof seq 2 type i\nfile {R} seq 1 type i\n"),
            ("local: a manifest seq of 0 is refused", encode("SET", "r", "1"), {}, "file {R} seq 0 type b\n"),
            ("local: two incremental files with the same seq are refused", encode("SET", "r", "1"),
             {"i1.aof": encode("SET", "q", "1")}, "file i1.aof seq 1 type i\nfile {R} seq 1 type i\n")]:
        rname = rand_word(6)
        base = os.path.join(d, "bad" + rand_word(4))
        os.makedirs(os.path.join(base, "ad"))
        man = ("file {R} seq 1 type i\n" if manifest is None else manifest).replace("{R}", rname + ".1.incr.aof")
        write_bytes(os.path.join(base, "ad", "af.aof.manifest"), man.encode())
        write_bytes(os.path.join(base, "ad", rname + ".1.incr.aof"), data)
        for fname, content in extra.items():
            write_bytes(os.path.join(base, "ad", fname), content)
        aof_exits(prog, ["--dir", base, "--appendonly", "yes", "--appenddirname", "ad",
                         "--appendfilename", "af.aof"], name)

    # A manifest with comments only names no file: the server starts, as
    # Redis does, and makes a first incremental file.
    base = os.path.join(d, "comments")
    os.makedirs(os.path.join(base, "ad"))
    write_bytes(os.path.join(base, "ad", "af.aof.manifest"), b"# nothing yet\n")
    srv = Server(prog, ["--dir", base, "--appendonly", "yes", "--appenddirname", "ad", "--appendfilename", "af.aof"])
    try:
        name = "local: a manifest with comments only gets a first incremental file"
        if aof_started(srv, [name]):
            c = Asker(srv)
            c.call("SET", "r", "1")
            got = [read_bytes(os.path.join(base, "ad", "af.aof.manifest")),
                   read_bytes(os.path.join(base, "ad", "af.aof.1.incr.aof"))]
            check(name, got == [b"# nothing yet\nfile af.aof.1.incr.aof seq 1 type i\n", encode("SET", "r", "1")],
                  repr(got))
            c.close()
    finally:
        srv.stop()

    # A replica that stops reading keeps its entries in the log while the
    # AOF drops the ones that the file has: when it reads again, it gets
    # every write.
    base = os.path.join(d, "lag")
    os.mkdir(base)
    srv = Server(prog, ["--dir", base, "--appendonly", "yes"])
    try:
        name = "local: with the AOF on, a replica that stops reading still gets every write"
        if aof_started(srv, [name]):
            r, _, _ = replica_handshake(srv.port)
            c = Asker(srv)
            payload = "v" * (1 << 20)
            for i in range(40):
                c.call("SET", "k%d" % i, payload)
            for i in range(2000):
                c.c.send_raw(encode("INCR", "n"))
            for i in range(2000):
                c.c.try_read(5)
            want = [["SET", "k%d" % i, payload] for i in range(40)] + [["INCR", "n"]] * 2000
            got = []
            for _ in range(len(want)):
                x = r.try_read(5)
                if x in (TIMEOUT, CLOSED, BAD_REPLY):
                    got.append(x)
                    break
                got.append(x)
            check(name, got == want, "%d entries, first bad %r" % (
                len(got), next((repr(g)[:60] for g, w in zip(got, want) if g != w), None)))
            r.close()
            c.close()
    finally:
        srv.stop()

    # A replica that reads at once, and an AOF that is behind it: the log
    # keeps the entries that the file has not got.
    base, adir, aname, rpath = prepared("ahead")
    srv = Server(prog, ["--dir", base, "--appendonly", "yes", "--appenddirname", adir,
                        "--appendfilename", aname])
    try:
        name = "local: a replica ahead of the AOF does not make the log drop what the file needs"
        if aof_started(srv, [name]):
            r, _, _ = replica_handshake(srv.port)
            stop = threading.Event()

            def drain():
                r.sock.settimeout(0.2)
                while not stop.is_set():
                    try:
                        if not r.sock.recv(1 << 20):
                            return
                    except socket.timeout:
                        pass
                    except OSError:
                        return
            th = threading.Thread(target=drain)
            th.start()
            clients = [Client(srv.port) for _ in range(4)]
            for j, cl in enumerate(clients):
                try:
                    cl.send_raw(b"".join(encode("SET", "a%d-%d" % (j, i), "v") for i in range(6000)))
                except OSError:
                    # The server went down: the check below fails by name.
                    pass
            for cl in clients:
                for _ in range(6000):
                    if cl.try_read(10) in (TIMEOUT, CLOSED, BAD_REPLY):
                        break
                cl.close()
            stop.set()
            th.join()
            r.close()
            got = (read_bytes(rpath) or b"").count(b"*3\r\n$3\r\nSET\r\n")
            check(name, srv.proc.poll() is None and got == 24000, "up=%s, %d SET in the file" % (
                srv.proc.poll() is None, got))
    finally:
        srv.stop()

    # A manifest with no incremental file gets one.
    base = os.path.join(d, "noincr")
    os.makedirs(os.path.join(base, "ad"))
    write_bytes(os.path.join(base, "ad", "af.aof.manifest"), b"file b.aof seq 1 type b\n")
    write_bytes(os.path.join(base, "ad", "b.aof"), encode("SET", "r", "1"))
    srv = Server(prog, ["--dir", base, "--appendonly", "yes", "--appenddirname", "ad", "--appendfilename", "af.aof"])
    try:
        name = "local: a manifest with no incremental file gets one, and the writes go to it"
        if aof_started(srv, [name]):
            c = Asker(srv)
            first = c.call("GET", "r")
            c.call("SET", "r", "2")
            got = [first, read_bytes(os.path.join(base, "ad", "af.aof.manifest")),
                   read_bytes(os.path.join(base, "ad", "af.aof.1.incr.aof"))]
            check(name, got == ["1", b"file b.aof seq 1 type b\nfile af.aof.1.incr.aof seq 1 type i\n",
                                encode("SET", "r", "2")], repr(got))
            c.close()
    finally:
        srv.stop()

    # The values of the flags are read with no case, as Redis reads them.
    base = os.path.join(d, "upper")
    os.mkdir(base)
    srv = Server(prog, ["--dir", base, "--appendonly", "YES", "--appendfsync", "Always"])
    try:
        name = "local: the values of --appendonly and --appendfsync have no case"
        if aof_started(srv, [name]):
            c = Asker(srv)
            got = [c.call("CONFIG", "GET", "appendonly"), c.call("CONFIG", "GET", "appendfsync"),
                   os.path.isdir(os.path.join(base, "appendonlydir"))]
            check(name, got == [["appendonly", "yes"], ["appendfsync", "always"], True], repr(got))
            c.close()
    finally:
        srv.stop()

    # The log drops the entries that the file has: 40 writes of 16 MB to
    # one key leave one value live.
    base = os.path.join(d, "trim")
    os.mkdir(base)
    srv = Server(prog, ["--dir", base, "--appendonly", "yes"])
    try:
        name = "local: AOF3 the log does not keep the entries that the file has"
        if aof_started(srv, [name]):
            c = Asker(srv)
            for i in range(40):
                c.c.sock.settimeout(30)
                c.call("SET", "r", ("%02d" % i) * (8 << 20))
            rss = rss_kib(srv.proc.pid)
            print("       measured: RSS %d KiB after 40 writes of 16 MB" % rss)
            check(name, rss < 300 * 1024, "RSS %d KiB" % rss)
            c.close()
    finally:
        srv.stop()

    # With appendonly yes, the RDB file is not loaded.
    base = os.path.join(d, "rdbtoo")
    os.mkdir(base)
    write_rdb(os.path.join(base, "dump.rdb"), [("rdbkey", "v", None, None)])
    srv = Server(prog, ["--dir", base, "--appendonly", "yes"])
    try:
        name = "local: with appendonly yes, the RDB file is not loaded"
        if aof_started(srv, [name]):
            c = Asker(srv)
            check(name, c.call("GET", "rdbkey") is None)
            c.close()
    finally:
        srv.stop()

    # A write that the file cannot take stops the server, as Redis does with
    # appendfsync always: part of a command may be in the file.
    base, adir, aname, rpath = prepared("werr")
    srv = Server(prog, ["--dir", base, "--appendonly", "yes", "--appenddirname", adir,
                        "--appendfilename", aname])
    try:
        name = "local: a write that the AOF file cannot take stops the server"
        if aof_started(srv, [name]):
            shutil.rmtree(os.path.join(base, adir))
            c = Asker(srv)
            got = c.call("SET", "k", "v")
            try:
                code = srv.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                code = None
            check(name, got is CLOSED and code == 1 and "redis.lox: cannot write the AOF file" in srv.log_text(),
                  repr((got, code, srv.log_text()[-200:])))
            c.close()
    finally:
        srv.stop()

    # Flags that the server refuses.
    for name, args in [
            ("local: --appendonly needs yes or no", ["--appendonly", "maybe"]),
            ("local: --appendfsync needs always, everysec or no", ["--appendfsync", "sometimes"]),
            ("local: --appendfilename needs a name with no '/'", ["--appendfilename", "a/b"]),
            ("local: --appenddirname needs a name with no '/'", ["--appenddirname", ".."]),
            ("local: appendonly yes on a replica is refused",
             ["--appendonly", "yes", "--replicaof", "localhost 1"])]:
        aof_exits(prog, args, name, cwd=home)

    # Rule 6: the writer writes a long value alone, and a long command in
    # steps, so the other clients go on.
    base, adir, aname, rpath = prepared("big")
    srv = Server(prog, ["--dir", base, "--appendonly", "yes", "--appenddirname", adir,
                        "--appendfilename", aname])
    try:
        names = ["local: AOF1 SET of 64 MB with the AOF stalls PING at most 1 s",
                 "local: the reply of a write comes after the file has it",
                 "local: the reply of EXEC comes after the file has the transaction",
                 "local: a served BLPOP replies after the file has its pop",
                 "local: AOF2 RPUSH of 1048573 elements with the AOF stalls PING at most 1 s",
                 "local: AOF2 the file has every element"]
        if aof_started(srv, names):
            c = Asker(srv)
            big = "y" * (64 << 20)

            def set_big():
                c.call("SET", "big", big)
            worst, wrong, _ = measure_while(srv, set_big)
            check(names[0], worst <= 1.0 and not wrong and read_bytes(rpath) == encode("SET", "big", big),
                  "worst %.3f s" % worst)
            c.call("DEL", "big")
            # A reply comes after the file has the entries of its command.
            # 200000 elements take the writer about 200 steps, so a reply
            # that does not wait comes before the file has them, and the
            # file is read at once.
            many = ["m%d" % i for i in range(200000)]
            def slow_call(*args):
                # A long limit: a loaded host can take more than the 3 s of
                # Asker.call, and a call that gives up would read the file
                # while the writer still writes.
                c.raw().send_raw(encode(*args))
                return c.raw().try_read(30)
            mark = len(read_bytes(rpath))
            slow_call("RPUSH", "W", *many)
            got = read_bytes(rpath)[mark:]
            check("local: the reply of a write comes after the file has it",
                  got == encode("RPUSH", "W", *many), "%d bytes" % len(got))
            mark = len(read_bytes(rpath))
            slow_call("MULTI")
            slow_call("RPUSH", "W2", *many)
            slow_call("SET", "W3", "1")
            slow_call("EXEC")
            got = read_bytes(rpath)[mark:]
            check("local: the reply of EXEC comes after the file has the transaction",
                  got == encode("MULTI") + encode("RPUSH", "W2", *many) + encode("SET", "W3", "1") + encode("EXEC"),
                  "%d bytes" % len(got))
            b = Asker(srv)
            b.c.send_raw(encode("BLPOP", "Q", "0"))
            time.sleep(0.2)
            mark = len(read_bytes(rpath))
            c.c.send_raw(encode("RPUSH", "Q", *many))
            served = b.c.try_read(10)
            got = read_bytes(rpath)[mark:]
            c.c.try_read(10)
            check("local: a served BLPOP replies after the file has its pop",
                  served == ["Q", "m0"] and got == encode("RPUSH", "Q", *many) + encode("LPOP", "Q"),
                  "%r, %d bytes" % (served, len(got)))
            b.close()
            c.call("DEL", "W", "W2", "W3", "Q")
            mark = len(read_bytes(rpath))
            n = 1048573
            elems = ["e%d" % i for i in range(n)]

            def push():
                c.c.sock.settimeout(60)
                c.c.send_raw(encode("RPUSH", "L", *elems))
                c.c.read()
                c.c.sock.settimeout(5)
            worst, wrong, _ = measure_while(srv, push)
            check(names[4], worst <= 1.0 and not wrong, "worst %.3f s" % worst)
            got = read_bytes(rpath)[mark:]
            check(names[5], got == encode("RPUSH", "L", *elems), "%d bytes" % len(got))
            c.close()
    finally:
        srv.stop()


def can_not_execute(reply, name):
    return isinstance(reply, Error) and reply.lower().startswith(("err can't execute '%s'" % name).lower())


def stages_pubsub(prog):
    srv = Server(prog, [])
    try:
        check("pub-sub: server starts", srv.up, srv.log_text())
        if not srv.up:
            return
        stages_pubsub_on(srv)
    finally:
        srv.stop()
    stages_pubsub_limits(prog)
    stages_pubsub_replication(prog)


def stages_pubsub_on(srv):
    a = Asker(srv)
    check("pub-sub-01 SUBSCRIBE gives subscribe, the channel and 1",
          a.call("SUBSCRIBE", "foo") == ["subscribe", "foo", 1])
    check("pub-sub-02 a second channel counts 2", a.call("SUBSCRIBE", "bar") == ["subscribe", "bar", 2])
    check("pub-sub-02 the same channel again keeps the count", a.call("SUBSCRIBE", "bar") == ["subscribe", "bar", 2])
    b = Asker(srv)
    check("pub-sub-02 the count is for each client", b.call("SUBSCRIBE", "bar") == ["subscribe", "bar", 1])
    for name, cmd in (("set", ("SET", "k", "v")), ("get", ("GET", "k")), ("echo", ("ECHO", "hey"))):
        got = a.call(*cmd)
        check("pub-sub-03 subscribed mode refuses %s" % name.upper(), can_not_execute(got, name), repr(got))
    check("pub-sub-03 subscribed mode allows SUBSCRIBE", a.call("SUBSCRIBE", "baz") == ["subscribe", "baz", 3])
    check("pub-sub-04 PING in subscribed mode gives pong and an empty string", a.call("PING") == ["pong", ""])
    plain = Asker(srv)
    check("pub-sub-04 PING of a client that is not subscribed gives PONG", plain.call("PING") == "PONG")

    # Stages 5-6: the count and the delivery.
    c1, c2, c3 = Asker(srv), Asker(srv), Asker(srv)
    c1.call("SUBSCRIBE", "p-foo")
    c2.call("SUBSCRIBE", "p-foo")
    c3.call("SUBSCRIBE", "p-bar")
    check("pub-sub-05 PUBLISH gives the number of subscribers", plain.call("PUBLISH", "p-foo", "hello") == 2)
    check("pub-sub-05 PUBLISH to another channel", plain.call("PUBLISH", "p-bar", "world") == 1)
    check("pub-sub-05 PUBLISH to a channel with no subscriber gives 0", plain.call("PUBLISH", "p-none", "x") == 0)
    got = [c1.c.try_read(2), c2.c.try_read(2), c3.c.try_read(2)]
    check("pub-sub-06 each subscriber gets the message of its channel",
          got == [["message", "p-foo", "hello"], ["message", "p-foo", "hello"], ["message", "p-bar", "world"]], repr(got))
    got = [c1.c.try_read(0.3), c3.c.try_read(0.3)]
    check("pub-sub-06 a client gets no message of another channel", got == [TIMEOUT, TIMEOUT], repr(got))

    # Stage 7: UNSUBSCRIBE.
    u1, u2 = Asker(srv), Asker(srv)
    u1.call("SUBSCRIBE", "u-foo")
    u1.call("SUBSCRIBE", "u-baz")
    u2.call("SUBSCRIBE", "u-foo")
    u2.call("SUBSCRIBE", "u-bar")
    plain.call("PUBLISH", "u-foo", "before-unsubscribe")
    got = [u1.c.try_read(2), u2.c.try_read(2)]
    check("pub-sub-07 before UNSUBSCRIBE both subscribers get the message",
          got == [["message", "u-foo", "before-unsubscribe"]] * 2, repr(got))
    check("pub-sub-07 UNSUBSCRIBE gives unsubscribe, the channel and the count left",
          u1.call("UNSUBSCRIBE", "u-foo") == ["unsubscribe", "u-foo", 1])
    check("pub-sub-07 PUBLISH after UNSUBSCRIBE counts the others", plain.call("PUBLISH", "u-foo", "after-unsubscribe") == 1)
    got = [u1.c.try_read(0.3), u2.c.try_read(2)]
    check("pub-sub-07 after UNSUBSCRIBE only the other subscriber gets the message",
          got == [TIMEOUT, ["message", "u-foo", "after-unsubscribe"]], repr(got))
    check("pub-sub-07 UNSUBSCRIBE of a channel that the client has not gives the same count",
          u1.call("UNSUBSCRIBE", "u-nope") == ["unsubscribe", "u-nope", 1])

    # Local cases.
    n = Asker(srv)
    check("local: UNSUBSCRIBE with no channel and no subscription gives a null channel and 0",
          n.call("UNSUBSCRIBE") == ["unsubscribe", None, 0])
    n.c.send_raw(encode("SUBSCRIBE", "x1", "x2", "x1"))
    got = [n.c.try_read(2) for _ in range(3)]
    check("local: SUBSCRIBE of three channels gives one reply each",
          got == [["subscribe", "x1", 1], ["subscribe", "x2", 2], ["subscribe", "x1", 2]], repr(got))
    check("local: PING with an argument in subscribed mode gives pong and the argument",
          n.call("PING", "hi") == ["pong", "hi"])
    got = n.call("PING", "a", "b")
    check("local: PING with two arguments in subscribed mode is an arity error",
          isinstance(got, Error) and "wrong number of arguments" in got, repr(got))
    got = n.call("NOSUCH")
    check("local: an unknown command in subscribed mode is an unknown command",
          isinstance(got, Error) and got.startswith("ERR unknown command"), repr(got))
    got = n.call("GET")
    check("local: a wrong number of arguments in subscribed mode comes first",
          isinstance(got, Error) and "wrong number of arguments for 'get'" in got, repr(got))
    for name, cmd in (("multi", ("MULTI",)), ("wait", ("WAIT", "1", "1")), ("watch", ("WATCH", "k")),
                      ("publish", ("PUBLISH", "x", "y"))):
        got = n.call(*cmd)
        check("local: subscribed mode refuses %s" % name.upper(), can_not_execute(got, name), repr(got))
    n.c.send_raw(encode("UNSUBSCRIBE"))
    got = [n.c.try_read(2) for _ in range(2)]
    check("local: UNSUBSCRIBE with no channel leaves each channel",
          sorted(g[1] for g in got if isinstance(g, list)) == ["x1", "x2"]
          and sorted(g[2] for g in got if isinstance(g, list)) == [0, 1], repr(got))
    check("local: with no channel left the client leaves subscribed mode", n.call("PING") == "PONG")
    check("local: a client that left subscribed mode runs any command", n.call("ECHO", "back") == "back")

    t = Asker(srv)
    t.call("MULTI")
    got = t.call("SUBSCRIBE", "q")
    check("local: SUBSCRIBE in MULTI is refused", isinstance(got, Error), repr(got))
    got = t.call("EXEC")
    check("local: a refused SUBSCRIBE aborts the transaction",
          isinstance(got, Error) and got.startswith("EXECABORT"), repr(got))
    s = Asker(srv)
    s.call("SUBSCRIBE", "tx-ch")
    t.call("MULTI")
    t.call("PUBLISH", "tx-ch", "m1")
    t.call("SET", "tx-k", "v")
    got = t.call("EXEC")
    check("local: PUBLISH in EXEC gives the count", got == [1, "OK"], repr(got))
    check("local: PUBLISH in EXEC delivers", s.c.try_read(2) == ["message", "tx-ch", "m1"])
    for i in range(100):
        plain.c.send_raw(encode("PUBLISH", "tx-ch", "n%d" % i))
    for i in range(100):
        plain.c.try_read(2)
    got = [s.c.try_read(2) for _ in range(100)]
    check("local: the messages come in the order of PUBLISH",
          got == [["message", "tx-ch", "n%d" % i] for i in range(100)], repr(got[:3]))
    s.close()
    deadline = time.time() + 5
    left = plain.call("PUBLISH", "tx-ch", "gone")
    while left != 0 and time.time() < deadline:
        time.sleep(0.05)
        left = plain.call("PUBLISH", "tx-ch", "gone")
    check("local: a subscriber that goes leaves its channels", left == 0, repr(left))
    z = Asker(srv)
    got = z.call("SUBSCRIBE")
    check("local: SUBSCRIBE with no channel is an arity error",
          isinstance(got, Error) and "wrong number of arguments for 'subscribe'" in got, repr(got))
    z.call("SUBSCRIBE", "zz")
    for name, cmd, want in (
            ("WAIT with one argument", ("WAIT", "1"), "ERR wrong number of arguments for 'wait'"),
            ("PSYNC with one argument", ("PSYNC", "?"), "ERR wrong number of arguments for 'psync'"),
            ("EXEC", ("EXEC",), "EXECABORT Transaction discarded because of: Can't execute 'exec'"),
            ("CONFIG GET", ("CONFIG", "GET", "dir"), "ERR Can't execute 'config|get'"),
            ("CONFIG FOO", ("CONFIG", "FOO"), "ERR unknown subcommand 'FOO'"),
            ("CONFIG GET with no name", ("CONFIG", "GET"), "ERR wrong number of arguments for 'config|get'"),
            ("EXEC with an argument", ("EXEC", "x"), "EXECABORT Transaction discarded because of: wrong number"),
            ("PSYNC with three arguments", ("PSYNC", "a", "b", "c"), "ERR Can't execute 'psync'")):
        got = z.call(*cmd)
        check("local: in subscribed mode %s gives the error of Redis" % name,
              isinstance(got, Error) and got.startswith(want), repr(got))
    z.close()

    # A message that a PUBLISH sent before the session ran a command goes
    # before the reply of that command, also when the reader has already
    # sent the command: the session of a subscriber that does not read is
    # behind its reader.
    late = Client(srv.port)
    late.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
    late.call("SUBSCRIBE", "late-ch")
    pub = Asker(srv)
    big = "b" * (1 << 20)
    for _ in range(8):
        pub.call("PUBLISH", "late-ch", big)
    late.send_raw(encode("UNSUBSCRIBE", "late-ch") + encode("PING"))
    time.sleep(0.3)
    counted = pub.call("PUBLISH", "late-ch", "late")
    # A large buffer again for the read: 8 MB through 4 KB takes minutes.
    late.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 22)
    late.sock.settimeout(10)
    stream = []
    try:
        while True:
            x = late.read()
            stream.append(x)
            if x == "PONG" or (isinstance(x, list) and x and x[0] == "pong"):
                break
    except (OSError, EOFError, ValueError) as e:
        stream.append(repr(e))
    tail = [x if not (isinstance(x, list) and len(x) == 3 and x[2] == big) else "big" for x in stream]
    want = ["big"] * 8 + ([["message", "late-ch", "late"]] if counted == 1 else []) + [["unsubscribe", "late-ch", 0], "PONG"]
    check("local: a message that came before UNSUBSCRIBE ran goes before its reply", tail == want,
          "counted %r, got %r" % (counted, tail))
    late.close()
    pub.close()

    # The same with PING in subscribed mode: the message goes before the
    # pong.
    late = Client(srv.port)
    late.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
    late.call("SUBSCRIBE", "late-ping")
    pub = Asker(srv)
    for _ in range(8):
        pub.call("PUBLISH", "late-ping", big)
    late.send_raw(encode("PING"))
    time.sleep(0.3)
    counted = pub.call("PUBLISH", "late-ping", "late")
    late.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 22)
    late.sock.settimeout(10)
    stream = []
    try:
        while True:
            x = late.read()
            stream.append(x)
            if isinstance(x, list) and x and x[0] == "pong":
                break
    except (OSError, EOFError, ValueError) as e:
        stream.append(repr(e))
    tail = [x if not (isinstance(x, list) and len(x) == 3 and x[2] == big) else "big" for x in stream]
    want = ["big"] * 8 + ([["message", "late-ping", "late"]] if counted == 1 else []) + [["pong", ""]]
    check("local: a message that came before PING ran goes before the pong", tail == want,
          "counted %r, got %r" % (counted, tail))
    late.close()
    pub.close()

    # UNSUBSCRIBE of 3000 long channels to a client that does not read: its
    # session is held in the loop, and a message to the last channel, which
    # it has not left yet, goes before the reply that leaves it.
    det = Client(srv.port)
    det.sock.settimeout(30)
    names = ["%04d" % i + "n" * 3996 for i in range(3000)]
    det.send_raw(encode("SUBSCRIBE", *names))
    for _ in range(3000):
        det.read()
    det.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
    det.send_raw(encode("UNSUBSCRIBE", *names))
    time.sleep(0.5)
    pub = Asker(srv)
    counted = pub.call("PUBLISH", names[-1], "late")
    det.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 22)
    seq = []
    try:
        det.sock.settimeout(5)
        while len(seq) < 3:
            x = det.read()
            if isinstance(x, list) and x[0] == "message":
                seq.append("message")
            elif isinstance(x, list) and x[1] == names[-1]:
                seq.append("last reply")
                break
    except (OSError, EOFError, ValueError) as e:
        seq.append(repr(e))
    want = (["message"] if counted == 1 else []) + ["last reply"]
    check("local: a message counted before UNSUBSCRIBE left its channel goes before that reply",
          seq == want, "counted %r, got %r" % (counted, seq))
    det.close()
    pub.close()

    # A publisher that keeps a backlog does not keep the command of a
    # subscriber from running: the session writes only the messages that
    # were there when the command came.
    slow = Client(srv.port)
    slow.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 65536)
    slow.sock.settimeout(30)
    slow.call("SUBSCRIBE", "starve")
    pub = Client(srv.port)
    pub.sock.settimeout(30)
    msg = "m" * 65536
    state = {"read": 0, "pub": 0, "pong": None, "stop": False}
    for _ in range(100):
        pub.call("PUBLISH", "starve", msg)
        state["pub"] += 1
    t0 = time.time()
    slow.send_raw(encode("PING"))

    def slow_reader():
        try:
            while not state["stop"]:
                x = slow.read()
                if isinstance(x, list) and x[0] == "pong":
                    state["pong"] = time.time() - t0
                    return
                state["read"] += 1
                time.sleep(0.03)
        except (OSError, EOFError, ValueError):
            return
    th = threading.Thread(target=slow_reader)
    th.start()
    while time.time() - t0 < 20 and state["pong"] is None:
        if state["pub"] - state["read"] < 100:
            pub.call("PUBLISH", "starve", msg)
            state["pub"] += 1
        else:
            time.sleep(0.002)
    state["stop"] = True
    th.join(timeout=40)
    check("local: a publisher that keeps a backlog does not stop the PING of a subscriber",
          state["pong"] is not None, "read %d of %d" % (state["read"], state["pub"]))
    slow.close()
    pub.close()

    # UNSUBSCRIBE while a publisher keeps a backlog on the channel: it
    # leaves the channel first, then writes the messages that are there, so
    # no message of the channel comes after the reply that leaves it.
    slow = Client(srv.port)
    slow.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 65536)
    slow.sock.settimeout(30)
    slow.call("SUBSCRIBE", "backlog")
    pub = Client(srv.port)
    pub.sock.settimeout(30)
    st = {"read": 0, "pub": 0, "reply": None, "after": 0, "zero": False}
    for _ in range(100):
        pub.call("PUBLISH", "backlog", msg)
        st["pub"] += 1
    t0 = time.time()
    slow.send_raw(encode("UNSUBSCRIBE", "backlog"))

    def unsub_reader():
        try:
            while True:
                x = slow.read()
                if isinstance(x, list) and x[0] == "unsubscribe":
                    st["reply"] = st["read"]
                    slow.sock.settimeout(3)
                    continue
                if st["reply"] is not None:
                    st["after"] += 1
                else:
                    st["read"] += 1
                    time.sleep(0.03)
        except (OSError, EOFError, ValueError):
            return
    th = threading.Thread(target=unsub_reader)
    th.start()
    while time.time() - t0 < 25 and st["reply"] is None:
        if st["zero"] or st["pub"] - st["read"] >= 100:
            time.sleep(0.002)
            continue
        if pub.call("PUBLISH", "backlog", msg) == 0:
            st["zero"] = True
        st["pub"] += 1
    th.join(timeout=40)
    check("local: no message of a channel comes after the reply of UNSUBSCRIBE under a backlog",
          st["reply"] is not None and st["after"] == 0,
          "reply after %r messages, %d messages after it" % (st["reply"], st["after"]))
    slow.close()
    pub.close()
    for x in (a, b, plain, c1, c2, c3, u1, u2, n, t):
        x.close()


def pubsub_count(srv, channel):
    c = Asker(srv)
    try:
        return c.call("PUBLISH", channel, "probe")
    finally:
        c.close()


def wait_count(srv, channel, want, seconds=5):
    deadline = time.time() + seconds
    got = pubsub_count(srv, channel)
    while got != want and time.time() < deadline:
        time.sleep(0.1)
        got = pubsub_count(srv, channel)
    return got


def stages_pubsub_limits(prog):
    # PS1: a subscriber that never reads is closed over the hard limit.
    srv = Server(prog, [])
    try:
        names = ["local: PS1 a subscriber that never reads is closed over the hard limit of 32 MB",
                 "local: PS1 the other clients go on"]
        if aof_started(srv, names):
            lazy = Client(srv.port)
            lazy.call("SUBSCRIBE", "big")
            lazy.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
            p = Asker(srv)
            payload = "z" * (1 << 20)
            for _ in range(40):
                p.call("PUBLISH", "big", payload)
            check(names[0], wait_count(srv, "big", 0) == 0)
            check(names[1], p.call("PING") == "PONG")
            lazy.close()
            p.close()
    finally:
        srv.stop()

    # PS5: a subscriber that reads its messages gives back their cost, so
    # 40 MB over time do not close it.
    srv = Server(prog, [])
    try:
        name = "local: PS5 a subscriber that reads its messages is not closed"
        if aof_started(srv, [name]):
            reader = Client(srv.port)
            reader.call("SUBSCRIBE", "read")
            p = Asker(srv)
            payload = "r" * (1 << 20)
            got = 0
            for _ in range(40):
                p.call("PUBLISH", "read", payload)
                if reader.try_read(5) == ["message", "read", payload]:
                    got += 1
            check(name, got == 40 and pubsub_count(srv, "read") == 1, "%d messages" % got)
            reader.close()
            p.close()
    finally:
        srv.stop()

    # PS6: an empty message costs its memory in the mailbox, so 600000 of
    # them close a subscriber that does not read (the socket buffers take
    # the first 100000 or more).
    srv = Server(prog, [])
    try:
        name = "local: PS6 600000 empty messages close a subscriber that does not read"
        if aof_started(srv, [name]):
            lazy = Client(srv.port)
            lazy.call("SUBSCRIBE", "e")
            lazy.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
            p = Client(srv.port)
            p.sock.settimeout(30)
            for _ in range(60):
                p.send_raw(encode("PUBLISH", "e", "") * 10000)
                for _ in range(10000):
                    p.read()
            check(name, wait_count(srv, "e", 0) == 0)
            lazy.close()
            p.close()
    finally:
        srv.stop()

    # PS7: a subscriber that read its messages starts its soft time again.
    srv = Server(prog, ["--pubsub-soft-seconds", "2"])
    try:
        name = "local: PS7 a subscriber that read its messages starts the soft time again"
        if aof_started(srv, [name]):
            sub = Client(srv.port)
            sub.call("SUBSCRIBE", "again")
            p = Asker(srv)
            payload = "a" * (1 << 20)
            for _ in range(12):
                p.call("PUBLISH", "again", payload)
            sub.sock.settimeout(10)
            for _ in range(12):
                sub.read()
            time.sleep(2.5)
            for _ in range(12):
                p.call("PUBLISH", "again", payload)
            check(name, pubsub_count(srv, "again") == 1)
            sub.close()
            p.close()
    finally:
        srv.stop()

    # PS8: a channel with no subscriber left is dropped: 5 rounds of a
    # client that joins 200000 new channels and goes; the memory grows much
    # less than without the drop.
    srv = Server(prog, [])
    try:
        name = "local: PS8 the channels that no client has are dropped"
        if aof_started(srv, [name]):
            rss = []
            for cyc in range(5):
                c = Client(srv.port)
                c.sock.settimeout(60)
                c.send_raw(encode("SUBSCRIBE", *["c%d-%d" % (cyc, i) for i in range(200000)]))
                for _ in range(200000):
                    c.read()
                c.close()
                wait_count(srv, "c%d-199999" % cyc, 0, seconds=20)
                time.sleep(0.5)
                rss.append(rss_kib(srv.proc.pid) // 1024)
            print("       measured: RSS in MB after each round %r" % rss)
            # Without the drop of empty channels the growth from round 2 to
            # round 5 is 380-470 MB; with it, 150-210 MB (the GC keeps some
            # garbage; the baseline heap moves the GC points).
            check(name, rss[4] - rss[1] < 300, repr(rss))
    finally:
        srv.stop()

    # PS2: the soft limit, with a soft time of 2 s.
    srv = Server(prog, ["--pubsub-soft-seconds", "2"])
    try:
        names = ["local: PS2 a subscriber over the soft limit stays while the soft time runs",
                 "local: PS2 a subscriber still over the soft limit after its time is closed"]
        if aof_started(srv, names):
            lazy = Client(srv.port)
            lazy.call("SUBSCRIBE", "soft")
            p = Asker(srv)
            payload = "z" * (1 << 20)
            for _ in range(12):
                p.call("PUBLISH", "soft", payload)
            check(names[0], pubsub_count(srv, "soft") == 1)
            time.sleep(2.5)
            p.call("PUBLISH", "soft", payload)
            check(names[1], wait_count(srv, "soft", 0) == 0)
            lazy.close()
            p.close()
    finally:
        srv.stop()

    # Rule 6: SUBSCRIBE of 1048575 channels, and the end of that client.
    srv = Server(prog, [])
    try:
        names = ["local: PS3 SUBSCRIBE of 1048575 channels stalls PING at most 1 s",
                 "local: PS3 the last reply counts every channel",
                 "local: PS3 SUBSCRIBE joins the channels in steps",
                 "local: PS4 the end of a client with 1048575 channels stalls PING at most 1 s",
                 "local: PS4 the end of the client leaves the channels in steps"]
        if aof_started(srv, names):
            sub = Client(srv.port)
            n = 1048575
            last = [None]

            def subscribe():
                sub.sock.settimeout(60)
                sub.send_raw(encode("SUBSCRIBE", *["ch%d" % i for i in range(n)]))
                # The messages of the sampler come between the replies.
                while True:
                    x = sub.read()
                    if isinstance(x, list) and x[0] == "subscribe":
                        last[0] = x
                        if x[1] == "ch%d" % (n - 1):
                            break
            # A pair of PUBLISH, to the first and the last channel, that
            # gives (1, 0) or (0, 1) saw the channels half joined or half
            # left: only a loop that yields between them shows that.
            pairs = set()
            stop = threading.Event()

            def sample():
                q = Client(srv.port)
                q.sock.settimeout(30)
                while not stop.is_set():
                    q.send_raw(encode("PUBLISH", "ch0", "x") + encode("PUBLISH", "ch%d" % (n - 1), "x"))
                    pairs.add((q.read(), q.read()))
                q.close()
            th = threading.Thread(target=sample)
            th.start()
            worst, wrong, _ = measure_while(srv, subscribe)
            stop.set()
            th.join()
            check(names[0], worst <= 1.0 and not wrong, "worst %.3f s" % worst)
            check(names[1], last[0] == ["subscribe", "ch%d" % (n - 1), n], repr(last[0]))
            check(names[2], (1, 0) in pairs, repr(sorted(pairs)))
            pairs.clear()
            stop.clear()
            th = threading.Thread(target=sample)
            th.start()

            def leave():
                sub.close()
                wait_count(srv, "ch%d" % (n - 1), 0, seconds=20)
            worst, wrong, _ = measure_while(srv, leave)
            stop.set()
            th.join()
            check(names[3], worst <= 1.0 and not wrong and pubsub_count(srv, "ch0") == 0, "worst %.3f s" % worst)
            check(names[4], (0, 1) in pairs, repr(sorted(pairs)))
    finally:
        srv.stop()


def stages_pubsub_replication(prog):
    # A master propagates PUBLISH to its replicas, and its AOF does not get
    # it, as in Redis.
    d = tempfile.mkdtemp(prefix="loxpp-pubsub-")
    srv = Server(prog, ["--dir", d, "--appendonly", "yes"])
    try:
        names = ["local: PUBLISH is propagated to the replicas",
                 "local: PUBLISH does not go to the AOF"]
        if aof_started(srv, names):
            r, _, _ = replica_handshake(srv.port)
            c = Asker(srv)
            c.call("PUBLISH", "rc", "m")
            c.call("SET", "after", "1")
            got = [r.try_read(2), r.try_read(2)]
            check(names[0], got == [["PUBLISH", "rc", "m"], ["SET", "after", "1"]], repr(got))
            data = read_bytes(os.path.join(d, "appendonlydir", "appendonly.aof.1.incr.aof"))
            check(names[1], data == encode("SET", "after", "1"), repr(data))
            r.close()
            c.close()
    finally:
        srv.stop()
        shutil.rmtree(d, ignore_errors=True)
    # A replica delivers a PUBLISH of its master to its subscribers.
    m = FakeMaster()
    rep = Server(prog, ["--replicaof", "localhost %d" % m.port])
    try:
        name = "local: a replica delivers a PUBLISH of its master"
        if aof_started(rep, [name]):
            link = m.accept()
            fake_handshake(link)
            s = Asker(rep)
            s.call("SUBSCRIBE", "rc")
            link.send_raw(encode("PUBLISH", "rc", "from-master"))
            check(name, s.c.try_read(2) == ["message", "rc", "from-master"])
            got = s.call("SET", "k", "v")
            check("local: on a replica, READONLY comes before the subscribed mode",
                  isinstance(got, Error) and got.startswith("READONLY"), repr(got))
            s.close()
    finally:
        rep.stop()
        m.close()


def stages_zset(prog):
    srv = Server(prog, [])
    try:
        check("sorted-sets: server starts", srv.up, srv.log_text())
        if not srv.up:
            return
        stages_zset_on(srv)
    finally:
        srv.stop()
    stages_zset_limits(prog)
    stages_zset_replication(prog)


def stages_zset_on(srv):
    c = Asker(srv)
    check("sorted-sets-01 ZADD of a new member gives 1", c.call("ZADD", "z1", "10.0", "zset_member") == 1)
    got = [c.call("ZADD", "z2", "20.0", "m1"), c.call("ZADD", "z2", "30.1", "m2"), c.call("ZADD", "z2", "40.2", "m3"),
           c.call("ZADD", "z2", "50.3", "m4")]
    check("sorted-sets-02 ZADD of each new member gives 1", got == [1, 1, 1, 1], repr(got))
    check("sorted-sets-02 ZADD of an existing member gives 0", c.call("ZADD", "z2", "100.0", "m1") == 0)
    for score, m in (("100.0", "foo"), ("100.0", "bar"), ("20.0", "baz"), ("30.1", "caz"), ("40.2", "paz")):
        c.call("ZADD", "z3", score, m)
    got = [c.call("ZRANK", "z3", m) for m in ("caz", "baz", "foo", "bar")]
    check("sorted-sets-03 ZRANK gives the rank by score, then member", got == [1, 0, 4, 3], repr(got))
    check("sorted-sets-03 ZRANK of a missing member gives a null bulk", c.call("ZRANK", "z3", "missing_member") is None)
    check("sorted-sets-03 ZRANK of a missing key gives a null bulk", c.call("ZRANK", "missing_key", "member") is None)
    check("sorted-sets-04 ZRANGE 2 4 gives the members", c.call("ZRANGE", "z3", "2", "4") == ["paz", "bar", "foo"])
    check("sorted-sets-04 ZRANGE past the end stops at the end", c.call("ZRANGE", "z3", "3", "10") == ["bar", "foo"])
    check("sorted-sets-04 ZRANGE with start after stop is empty", c.call("ZRANGE", "z3", "3", "1") == [])
    check("sorted-sets-04 ZRANGE with start past the end is empty", c.call("ZRANGE", "z3", "5", "9") == [])
    check("sorted-sets-04 ZRANGE of a missing key is empty", c.call("ZRANGE", "missing_key", "0", "1") == [])
    for score, m in (("20.0", "foo"), ("30.1", "bar"), ("40.2", "baz"), ("25.0", "paz"), ("25.0", "caz")):
        c.call("ZADD", "z5", score, m)
    check("sorted-sets-05 ZRANGE 2 -1", c.call("ZRANGE", "z5", "2", "-1") == ["paz", "bar", "baz"])
    check("sorted-sets-05 ZRANGE -2 -1", c.call("ZRANGE", "z5", "-2", "-1") == ["bar", "baz"])
    check("sorted-sets-05 ZRANGE with a start before the first is from the first",
          c.call("ZRANGE", "z5", "-10", "1") == ["foo", "caz"])
    check("sorted-sets-06 ZCARD gives the number of members", c.call("ZCARD", "z2") == 4)
    check("sorted-sets-06 ZCARD of a missing key gives 0", c.call("ZCARD", "missing_key") == 0)
    check("sorted-sets-07 ZSCORE gives the score", c.call("ZSCORE", "z2", "m2") == "30.1")
    c.call("ZADD", "z2", "100.99", "m2")
    check("sorted-sets-07 ZSCORE gives the new score", c.call("ZSCORE", "z2", "m2") == "100.99")
    check("sorted-sets-07 ZSCORE of a missing member gives a null bulk", c.call("ZSCORE", "z2", "m100") is None)
    check("sorted-sets-07 ZSCORE of a missing key gives a null bulk", c.call("ZSCORE", "missing_key", "m") is None)
    for score, m in (("80.5", "foo"), ("50.3", "baz"), ("80.5", "bar")):
        c.call("ZADD", "z8", score, m)
    check("sorted-sets-08 ZREM of a member gives 1", c.call("ZREM", "z8", "baz") == 1)
    check("sorted-sets-08 ZRANGE after ZREM", c.call("ZRANGE", "z8", "0", "-1") == ["bar", "foo"])
    check("sorted-sets-08 ZREM of a missing member gives 0", c.call("ZREM", "z8", "missing_member") == 0)

    # Local cases.
    check("local: ZADD of several pairs counts the new members", c.call("ZADD", "zl", "1", "a", "2", "b", "1", "a") == 2)
    check("local: the last score of a member in one ZADD wins", c.call("ZADD", "zl2", "3", "x", "2", "x") == 1
          and c.call("ZSCORE", "zl2", "x") == "2")
    c.call("ZADD", "zl", "5", "a")
    check("local: a new score moves the member", c.call("ZRANGE", "zl", "0", "-1") == ["b", "a"]
          and c.call("ZRANK", "zl", "a") == 1)
    check("local: ZRANGE WITHSCORES gives each member and its score",
          c.call("ZRANGE", "zl", "0", "-1", "WITHSCORES") == ["b", "2", "a", "5"])
    check("local: ZRANGE takes WITHSCORES twice, as Redis does",
          c.call("ZRANGE", "zl", "0", "-1", "WITHSCORES", "withscores") == ["b", "2", "a", "5"])
    got = c.call("ZADD", "zl", "1" + "0" * 64, "long")
    check("local: a score of 65 bytes is refused", got == NOT_FLOAT_REPLY, repr(got))
    t0 = time.time()
    got = [c.call("ZADD", "zl", "1e-999999", "x"), c.call("ZADD", "zl", "1e999999", "x")]
    check("local: ZADD of an exponent of 6 digits replies within 1 s",
          got == [NOT_FLOAT_REPLY] * 2 and time.time() - t0 < 1.0, "%r in %.2f s" % (got, time.time() - t0))
    args = []
    for i in range(64):
        args += ["1e-99999", "y%d" % i]
    t0 = time.time()
    got = c.call("ZADD", "zl", *args)
    check("local: ZADD of 64 scores 1e-99999 replies within 1 s",
          got == NOT_FLOAT_REPLY and time.time() - t0 < 1.0, "%r in %.2f s" % (got, time.time() - t0))
    for cmd, want in ((("ZADD", "zl", "1"), "wrong number of arguments for 'zadd'"),
                      (("ZADD", "zl", "1", "a", "2"), "syntax error"),
                      (("ZADD", "zl", "abc", "a"), "value is not a valid float"),
                      (("ZADD", "zl", "nan", "a"), "value is not a valid float"),
                      (("ZADD", "zl", "1e400", "a"), "value is not a valid float"),
                      (("ZADD", "zl", "1.8e308", "a"), "value is not a valid float"),
                      (("ZADD", "zl", "2e-324", "a"), "value is not a valid float"),
                      (("ZADD", "zl", "1x", "a"), "value is not a valid float"),
                      (("ZRANGE", "zl", "a", "b"), "value is not an integer"),
                      (("ZRANGE", "zl", "0", "1", "FOO"), "syntax error"),
                      (("ZRANGE", "zl", "0"), "wrong number of arguments for 'zrange'"),
                      (("ZREM", "zl"), "wrong number of arguments for 'zrem'"),
                      (("ZRANK", "zl"), "wrong number of arguments for 'zrank'"),
                      (("ZSCORE", "zl"), "wrong number of arguments for 'zscore'"),
                      (("ZCARD",), "wrong number of arguments for 'zcard'")):
        got = c.call(*cmd)
        check("local: %s gives the error of Redis" % " ".join(cmd), isinstance(got, Error) and want in got, repr(got))
    check("local: a bad score changes nothing", c.call("ZADD", "zl", "9", "new", "abc", "x") == NOT_FLOAT_REPLY
          and c.call("ZSCORE", "zl", "new") is None)
    scores = [("1e3", "1000"), ("-inf", "-inf"), ("+inf", "inf"), ("Infinity", "inf"), (".5", "0.5"), ("1.", "1"),
              ("-0.0", "0"), ("1.5e300", "1.5e+300"), ("00012.50", "12.5")]
    got = []
    for i, (text, _) in enumerate(scores):
        c.call("ZADD", "zf", text, "f%d" % i)
        got.append(c.call("ZSCORE", "zf", "f%d" % i))
    check("local: ZADD reads the score forms of strtod", got == [w for _, w in scores], repr(got))
    c.call("SET", "str", "v")
    for cmd in (("ZADD", "str", "1", "a"), ("ZREM", "str", "a"), ("ZCARD", "str"), ("ZSCORE", "str", "a"),
                ("ZRANK", "str", "a"), ("ZRANGE", "str", "0", "1")):
        got = c.call(*cmd)
        check("local: %s of a string gives WRONGTYPE" % cmd[0], isinstance(got, Error) and got.startswith("WRONGTYPE"),
              repr(got))
    check("local: TYPE of a sorted set gives zset", c.call("TYPE", "zl") == "zset")
    got = c.call("LPUSH", "zl", "x")
    check("local: LPUSH of a sorted set gives WRONGTYPE", isinstance(got, Error) and got.startswith("WRONGTYPE"), repr(got))
    check("local: ZREM counts each member once", c.call("ZREM", "zl", "a", "a", "nosuch") == 1)
    check("local: ZREM of the last member removes the key", c.call("ZREM", "zl", "b") == 1
          and c.call("TYPE", "zl") == "none" and c.call("ZCARD", "zl") == 0)
    # Ranks on a set of 2000 members, with ties: each rank and each range
    # agrees with a sort in Python.
    members = [("%d" % (i % 37), "m%04d" % i) for i in range(2000)]
    for i in range(0, 2000, 500):
        args = []
        for score, m in members[i:i + 500]:
            args += [score, m]
        c.call("ZADD", "big", *args)
    order = [m for s, m in sorted(members, key=lambda p: (int(p[0]), p[1]))]
    got = [c.call("ZRANK", "big", order[k]) for k in range(0, 2000, 97)]
    check("local: ZRANK on 2000 members agrees with a sort", got == list(range(0, 2000, 97)), repr(got[:5]))
    check("local: ZRANGE on 2000 members agrees with a sort", c.call("ZRANGE", "big", "0", "-1") == order
          and c.call("ZRANGE", "big", "1500", "1510") == order[1500:1511])
    for m in order[::3]:
        c.c.send_raw(encode("ZREM", "big", m))
    for _ in order[::3]:
        c.c.try_read(5)
    rest = [m for k, m in enumerate(order) if k % 3 != 0]
    check("local: ranks and ranges stay right after ZREM",
          c.call("ZRANGE", "big", "0", "-1") == rest and c.call("ZRANK", "big", rest[777]) == 777
          and c.call("ZCARD", "big") == len(rest))
    # WATCH: a ZADD that changes the set marks the client; one that changes
    # nothing does not.
    w = Asker(srv)
    c.call("ZADD", "wz", "1", "a")
    w.call("WATCH", "wz")
    c.call("ZADD", "wz", "1", "a")
    w.call("MULTI")
    w.call("PING")
    check("local: a ZADD that changes nothing does not fail EXEC", w.call("EXEC") == ["PONG"])
    w.call("WATCH", "wz")
    c.call("ZADD", "wz", "2", "a")
    w.call("MULTI")
    w.call("PING")
    check("local: a ZADD that changes a score fails EXEC", w.call("EXEC") is None)
    w.call("WATCH", "wz")
    c.call("ZREM", "wz", "a")
    w.call("MULTI")
    w.call("PING")
    check("local: ZREM fails EXEC", w.call("EXEC") is None)
    w.close()
    c.close()


NOT_FLOAT_REPLY = Error("ERR value is not a valid float")


def stages_zset_limits(prog):
    srv = Server(prog, [])
    try:
        names = ["local: ZADD of more than 16384 members is refused",
                 "local: ZREM of more than 16384 members is refused",
                 "local: EXEC refuses sorted set work over 32768",
                 "local: Z1 ZADD of 16384 members to a set of 524288 stalls PING at most 1 s",
                 "local: Z2 ZRANGE of 524288 members stalls PING at most 1 s",
                 "local: Z3 EXEC of two ZADD of 16384 members stalls PING at most 1 s",
                 "local: EXEC refuses a ZRANGE over the element budget",
                 "local: a new score costs two units of the sorted set work of EXEC",
                 "local: a member twice in one ZADD costs a delete and an insert",
                 "local: a pair that changes nothing costs one unit of the sorted set work of EXEC",
                 "local: Z6 ZRANGE with 1048572 options stalls PING at most 0.5 s",
                 "local: Z4 ZADD of 16384 scores of 64 bytes stalls PING at most 1 s"]
        if aof_started(srv, names):
            c = Asker(srv)
            c.c.sock.settimeout(60)
            args = []
            for i in range(16385):
                args += ["1", "x%d" % i]
            got = c.call("ZADD", "lim", *args)
            check(names[0], isinstance(got, Error) and "16384" in got and c.call("ZCARD", "lim") == 0, repr(got))
            got = c.call("ZREM", "lim", *["x%d" % i for i in range(16385)])
            check(names[1], isinstance(got, Error) and "16384" in got, repr(got))
            third = []
            for i in range(15000):
                third += ["1", "e%d" % i]
            c.call("MULTI")
            c.call("ZADD", "e1", *third)
            c.call("ZADD", "e2", *third)
            c.call("ZADD", "e3", *third)
            c.raw().sock.settimeout(60)
            c.raw().send_raw(encode("EXEC"))
            got = c.raw().try_read(60)
            check(names[2], isinstance(got, list) and got[:2] == [15000, 15000] and isinstance(got[2], Error)
                  and "transaction" in got[2] and c.call("ZCARD", "e3") == 0, repr(got)[:120])
            # A set of 524288 members, from 32 ZADD of 16384. A build that
            # takes over 60 s (a skip list of one level is a linked list)
            # fails Z1 by name, before the suite runs out of time.
            built = time.time()
            for b in range(32):
                if time.time() - built > 60:
                    break
                part = []
                for i in range(16384):
                    part += ["%d" % ((b * 16384 + i) % 1000), "s%07d" % (b * 16384 + i)]
                c.raw().send_raw(encode("ZADD", "Z", *part))
                c.raw().try_read(60)
            more = []
            for i in range(16384):
                more += ["%d" % (i % 997), "t%07d" % i]

            if time.time() - built > 60:
                for nm in names[3:]:
                    check(nm, False, "the set of 524288 members took over 60 s to build")
                c.close()
                return

            def zadd():
                c.raw().send_raw(encode("ZADD", "Z", *more))
                c.raw().try_read(60)
            worst, wrong, _ = measure_while(srv, zadd)
            check(names[3], worst <= 1.0 and not wrong and c.call("ZCARD", "Z") == 524288 + 16384,
                  "worst %.3f s" % worst)
            c.call("DEL", "e1", "e2")
            c.call("ZREM", "Z", *["t%07d" % i for i in range(16384)])
            got = [None]

            def zrange():
                c.raw().send_raw(encode("ZRANGE", "Z", "0", "-1"))
                got[0] = c.raw().try_read(60)
            worst, wrong, _ = measure_while(srv, zrange)
            check(names[4], worst <= 1.0 and not wrong and isinstance(got[0], list) and len(got[0]) == 524288,
                  "worst %.3f s" % worst)
            # The element budget of EXEC: 8 ZRANGE WITHSCORES of 524288
            # members fill it, and the ninth is refused.
            x = Client(srv.port)
            x.sock.settimeout(120)
            # An empty range first: it must spend nothing, and give back
            # nothing either.
            x.send_raw(encode("MULTI") + encode("ZRANGE", "Z", "524287", "0") * 3
                       + encode("ZRANGE", "Z", "0", "-1", "WITHSCORES") * 9 + encode("EXEC"))
            for _ in range(13):
                x.read()
            reply = x.read()
            x.close()
            check("local: EXEC refuses a ZRANGE over the element budget",
                  isinstance(reply, list) and len(reply) == 12 and reply[:3] == [[], [], []]
                  and all(len(r) == 1048576 for r in reply[3:11])
                  and isinstance(reply[11], Error), repr(reply[11])[:80] if isinstance(reply, list) else repr(reply)[:80])

            def exec_two():
                x = Client(srv.port)
                x.sock.settimeout(60)
                x.send_raw(encode("MULTI") + encode("ZADD", "Y1", *more) + encode("ZADD", "Y2", *more) + encode("EXEC"))
                for _ in range(4):
                    x.read()
                x.close()
            worst, wrong, _ = measure_while(srv, exec_two)
            check(names[5], worst <= 1.0 and not wrong, "worst %.3f s" % worst)
            # A new score is a delete and an insert: 16384 new members, then
            # new scores for them, are 49152 units, over 32768.
            fresh = []
            moved = []
            for i in range(16384):
                fresh += ["1", "w%d" % i]
                moved += ["2", "w%d" % i]
            x = Client(srv.port)
            x.sock.settimeout(60)
            x.send_raw(encode("MULTI") + encode("ZADD", "W", *fresh) + encode("ZADD", "W", *moved) + encode("EXEC"))
            for _ in range(3):
                x.read()
            reply = x.read()
            x.close()
            check("local: a new score costs two units of the sorted set work of EXEC",
                  isinstance(reply, list) and reply[0] == 16384 and isinstance(reply[1], Error), repr(reply)[:100])
            # A member that comes again in one ZADD with a new score is a
            # delete and an insert too: 8192 members twice are 24576 units,
            # and 8193 new members more pass 32768.
            twice = []
            for i in range(8192):
                twice += ["1", "d%d" % i]
            for i in range(8192):
                twice += ["2", "d%d" % i]
            extra = []
            for i in range(8193):
                extra += ["1", "f%d" % i]
            x = Client(srv.port)
            x.sock.settimeout(60)
            x.send_raw(encode("MULTI") + encode("ZADD", "D", *twice) + encode("ZADD", "F", *extra) + encode("EXEC"))
            for _ in range(3):
                x.read()
            reply = x.read()
            x.close()
            check("local: a member twice in one ZADD costs a delete and an insert",
                  isinstance(reply, list) and reply[0] == 8192 and isinstance(reply[1], Error), repr(reply)[:100])
            # A pair that changes nothing is one unit too: 3 ZADD of the
            # same 16384 pairs pass 32768.
            x = Client(srv.port)
            x.sock.settimeout(60)
            x.call("ZADD", "U", *fresh)
            x.send_raw(encode("MULTI") + encode("ZADD", "U", *fresh) * 3 + encode("EXEC"))
            for _ in range(4):
                x.read()
            reply = x.read()
            x.close()
            check("local: a pair that changes nothing costs one unit of the sorted set work of EXEC",
                  isinstance(reply, list) and reply[:2] == [0, 0] and isinstance(reply[2], Error), repr(reply)[:100])
            # Z6: ZRANGE with 1048572 options parses them in steps.
            def zrange_options():
                x = Client(srv.port)
                x.sock.settimeout(60)
                x.call("ZRANGE", "D", "0", "1", *(["WITHSCORES"] * 1048572))
                x.close()
            worst, wrong, _ = measure_while(srv, zrange_options)
            check("local: Z6 ZRANGE with 1048572 options stalls PING at most 0.5 s",
                  worst <= 0.5 and not wrong, "worst %.3f s" % worst)
            # Z4: 16384 scores of 64 bytes, the longest, on this master.
            long_args = []
            for i in range(16384):
                long_args += ["0." + "0" * 52 + "%04d" % (i % 10000) + "e-256", "v%d" % i]

            def zadd_long():
                x = Client(srv.port)
                x.sock.settimeout(60)
                x.call("ZADD", "V", *long_args)
                x.close()
            worst, wrong, _ = measure_while(srv, zadd_long)
            check("local: Z4 ZADD of 16384 scores of 64 bytes stalls PING at most 1 s",
                  worst <= 1.0 and not wrong and c.call("ZCARD", "V") == 16384, "worst %.3f s" % worst)
            c.close()
    finally:
        srv.stop()


def stages_zset_replication(prog):
    d = tempfile.mkdtemp(prefix="loxpp-zset-")
    srv = Server(prog, ["--dir", d, "--appendonly", "yes"])
    try:
        names = ["local: ZADD and ZREM that change the set are propagated, others not",
                 "local: a restart from the AOF gives back the sorted set"]
        if aof_started(srv, names):
            r, _, _ = replica_handshake(srv.port)
            c = Asker(srv)
            c.call("ZADD", "rz", "1", "a", "2", "b")
            c.call("ZADD", "rz", "1", "a")
            c.call("ZREM", "rz", "nosuch")
            c.call("ZREM", "rz", "b")
            c.call("SET", "end", "1")
            got = [r.try_read(2) for _ in range(3)]
            check(names[0], got == [["ZADD", "rz", "1", "a", "2", "b"], ["ZREM", "rz", "b"], ["SET", "end", "1"]],
                  repr(got))
            r.close()
            c.close()
    finally:
        srv.stop()
    srv = Server(prog, ["--dir", d, "--appendonly", "yes"])
    try:
        if aof_started(srv, [names[1]]):
            c = Asker(srv)
            check(names[1], c.call("ZRANGE", "rz", "0", "-1", "WITHSCORES") == ["a", "1"])
            c.close()
    finally:
        srv.stop()
        shutil.rmtree(d, ignore_errors=True)
    m = FakeMaster()
    rep = Server(prog, ["--replicaof", "localhost %d" % m.port])
    try:
        names = ["local: a replica refuses ZADD and ZREM from a client",
                 "local: a replica applies ZADD and ZREM of its master",
                 "local: Z4 a replica that applies ZADD of 16384 scores of 64 bytes stalls PING at most 1 s",
                 "local: Z5 a replica parses a ZADD of its master in steps"]
        if aof_started(rep, names):
            link = m.accept()
            fake_handshake(link)
            c = Asker(rep)
            got = [c.call("ZADD", "q", "1", "a"), c.call("ZREM", "q", "a")]
            check(names[0], all(isinstance(g, Error) and g.startswith("READONLY") for g in got), repr(got))
            link.send_raw(encode("ZADD", "q", "1", "a", "2", "b") + encode("ZREM", "q", "a"))
            time.sleep(0.3)
            check(names[1], c.call("ZRANGE", "q", "0", "-1") == ["b"])
            long_args = []
            for i in range(16384):
                long_args += ["0." + "0" * 52 + "%04d" % (i % 10000) + "e-256", "v%d" % i]

            def apply_long():
                link.send_raw(encode("ZADD", "V", *long_args))
                deadline = time.time() + 30
                while time.time() < deadline and c.call("ZCARD", "V") != 16384:
                    time.sleep(0.05)
            worst, wrong, _ = measure_while(rep, apply_long)
            check("local: Z4 a replica that applies ZADD of 16384 scores of 64 bytes stalls PING at most 1 s",
                  worst <= 1.0 and not wrong and c.call("ZCARD", "V") == 16384, "worst %.3f s" % worst)

            # The same ZADD again changes nothing, so only its parse is work:
            # with a yield every 1024 pairs it stalls little (rule 2).
            cheap = []
            for i in range(16384):
                cheap += ["0." + "0" * 40 + "%021d" % (i + 1), "c%d" % i]
            link.send_raw(encode("ZADD", "V3", *cheap))
            deadline = time.time() + 30
            while time.time() < deadline and c.call("ZCARD", "V3") != 16384:
                time.sleep(0.05)

            def apply_again():
                link.send_raw(encode("ZADD", "V3", *cheap) + encode("ZADD", "V2", "1", "done"))
                deadline = time.time() + 30
                while time.time() < deadline and c.call("ZCARD", "V2") != 1:
                    time.sleep(0.05)
            worst, wrong, _ = measure_while(rep, apply_again)
            check("local: Z5 a replica parses a ZADD of its master in steps",
                  worst <= 0.15 and not wrong and c.call("ZCARD", "V2") == 1, "worst %.3f s" % worst)
            c.close()
    finally:
        rep.stop()
        m.close()


def stages_geo(prog):
    srv = Server(prog, [])
    try:
        check("geospatial: server starts", srv.up, srv.log_text())
        if not srv.up:
            return
        stages_geo_on(srv)
    finally:
        srv.stop()
    stages_geo_limits(prog)
    stages_geo_replication(prog)


def stages_geo_on(srv):
    c = Asker(srv)
    check("geospatial-01 GEOADD of a new location gives 1", c.call("GEOADD", "g1", "11.5030378", "48.164271", "Munich") == 1)
    got = c.call("GEOADD", "g2", "200", "100", "foo")
    check("geospatial-02 GEOADD of an invalid pair gives an error that names both",
          isinstance(got, Error) and got.startswith("ERR") and "longitude" in got and "latitude" in got, repr(got))
    got = c.call("GEOADD", "g2", "10", "86", "foo")
    check("geospatial-02 an invalid latitude names the latitude",
          isinstance(got, Error) and got.startswith("ERR") and "latitude" in got, repr(got))
    got = c.call("GEOADD", "g2", "181", "10", "foo")
    check("geospatial-02 an invalid longitude names the longitude",
          isinstance(got, Error) and got.startswith("ERR") and "longitude" in got, repr(got))
    check("geospatial-03 GEOADD adds to a sorted set", c.call("GEOADD", "g3", "2.2944692", "48.8584625", "Paris") == 1
          and c.call("ZRANGE", "g3", "0", "-1") == ["Paris"])
    c.call("GEOADD", "g4", "2.2944692", "48.8584625", "Paris")
    c.call("GEOADD", "g4", "-0.127758", "51.507351", "London")
    check("geospatial-04 the score is the geohash", c.call("ZSCORE", "g4", "Paris") == "3663832614298053"
          and c.call("ZSCORE", "g4", "London") == "2163557714754256")
    c.call("GEOADD", "g5", "-0.0884948", "51.506479", "London")
    c.call("GEOADD", "g5", "11.5030378", "48.164271", "Munich")
    got = c.call("GEOPOS", "g5", "London", "Munich")
    check("geospatial-05 GEOPOS gives a pair of floats for each location",
          isinstance(got, list) and len(got) == 2 and all(isinstance(p, list) and len(p) == 2 for p in got)
          and all(abs(float(a) - b) < 1e-5 for a, b in zip(got[0] + got[1], [-0.0884948, 51.506479, 11.5030378, 48.164271])),
          repr(got))
    check("geospatial-05 GEOPOS of a missing location gives a null array", c.call("GEOPOS", "g5", "missing_location") == [None])
    check("geospatial-05 GEOPOS of a missing key gives a null array for each", c.call("GEOPOS", "missing_key", "a", "b") == [None, None])
    for score, m in (("3663832614298053", "Foo"), ("3876464048901851", "Bar"), ("3468915414364476", "Baz"),
                     ("3781709020344510", "Caz")):
        c.call("ZADD", "g6", score, m)
    check("geospatial-06 GEOPOS decodes the score", c.call("GEOPOS", "g6", "Foo") == [["2.294471561908722", "48.85846255040141"]],
          repr(c.call("GEOPOS", "g6", "Foo")))
    c.call("GEOADD", "g7", "11.5030378", "48.164271", "Munich")
    c.call("GEOADD", "g7", "2.2944692", "48.8584625", "Paris")
    check("geospatial-07 GEODIST gives the distance in meters with 4 decimals",
          c.call("GEODIST", "g7", "Munich", "Paris") == "682477.7582")
    c.call("GEOADD", "g8", "11.5030378", "48.164271", "Munich")
    c.call("GEOADD", "g8", "2.2944692", "48.8584625", "Paris")
    c.call("GEOADD", "g8", "-0.0884948", "51.506479", "London")
    got = [c.call("GEOSEARCH", "g8", "FROMLONLAT", "2", "48", "BYRADIUS", r, "m") for r in ("100000", "500000")]
    got.append(c.call("GEOSEARCH", "g8", "FROMLONLAT", "11", "50", "BYRADIUS", "300000", "m"))
    check("geospatial-08 GEOSEARCH gives the locations within the radius",
          [sorted(g) if isinstance(g, list) else g for g in got] == [["Paris"], ["London", "Paris"], ["Munich"]], repr(got))

    # Local cases.
    check("local: GEODIST in each unit", [c.call("GEODIST", "g7", "Munich", "Paris", u) for u in ("km", "mi", "ft", "M")]
          == ["682.4778", "424.0731", "2239100.2564", "682477.7582"])
    check("local: GEODIST of a missing member gives a null bulk", c.call("GEODIST", "g7", "Munich", "nosuch") is None)
    check("local: GEODIST of a missing key gives a null bulk", c.call("GEODIST", "nokey", "a", "b") is None)
    for cmd, want in ((("GEODIST", "g7", "Munich", "Paris", "yd"), "ERR unsupported unit provided"),
                      (("GEODIST", "g7", "Munich"), "ERR wrong number of arguments for 'geodist'"),
                      (("GEODIST", "g7", "a", "b", "m", "x"), "ERR syntax error"),
                      (("GEOADD", "g9", "1", "2"), "ERR wrong number of arguments for 'geoadd'"),
                      (("GEOADD", "g9", "1", "2", "a", "3"), "ERR syntax error"),
                      (("GEOADD", "g9", "abc", "2", "a"), "ERR value is not a valid float"),
                      (("GEOADD", "g9", "200", "100", "foo"), "ERR invalid longitude,latitude pair 200.000000,100.000000"),
                      (("GEOADD", "g9", "-180.5", "0", "foo"), "ERR invalid longitude,latitude pair -180.500000,0.000000"),
                      (("GEOPOS",), "ERR wrong number of arguments for 'geopos'"),
                      (("GEOSEARCH", "g8", "FROMLONLAT", "2", "48", "BYRADIUS", "-1", "m"), "ERR radius cannot be negative"),
                      (("GEOSEARCH", "g8", "FROMLONLAT", "2", "48", "BYRADIUS", "abc", "m"), "ERR need numeric radius"),
                      (("GEOSEARCH", "g8", "FROMLONLAT", "2", "48", "BYRADIUS", "1", "yd"), "ERR unsupported unit provided"),
                      (("GEOSEARCH", "g8", "FROMLONLAT", "200", "48", "BYRADIUS", "1", "m"), "ERR invalid longitude,latitude pair"),
                      (("GEOSEARCH", "g8", "FROMMEMBER", "nosuch", "BYRADIUS", "1", "m"), "ERR could not decode requested zset member"),
                      (("GEOSEARCH", "g8", "BYRADIUS", "1", "m", "x", "y"), "ERR syntax error")):
        got = c.call(*cmd)
        check("local: %s gives the error of Redis" % " ".join(cmd), isinstance(got, Error) and got.startswith(want), repr(got))
    check("local: GEOSEARCH FROMMEMBER uses the place of the member",
          sorted(c.call("GEOSEARCH", "g8", "FROMMEMBER", "Paris", "BYRADIUS", "500", "km")) == ["London", "Paris"])
    check("local: GEOSEARCH of a missing key is empty", c.call("GEOSEARCH", "nokey", "FROMLONLAT", "2", "48", "BYRADIUS", "1", "m") == [])
    check("local: GEOSEARCH FROMMEMBER of a missing key is empty, as in Redis",
          c.call("GEOSEARCH", "nokey", "FROMMEMBER", "a", "BYRADIUS", "1", "m") == [])
    check("local: GEOADD of an existing member at the same place gives 0",
          c.call("GEOADD", "g8", "2.2944692", "48.8584625", "Paris") == 0)
    check("local: a place at the top corner is decoded and clamped", c.call("GEOADD", "g8", "180", "85.05112878", "edge") == 1
          and c.call("GEOPOS", "g8", "edge") == [["180", "85.05112878"]], repr(c.call("GEOPOS", "g8", "edge")))
    c.call("ZADD", "g8", "-1", "neg")
    check("local: GEOPOS of a negative score gives a null array", c.call("GEOPOS", "g8", "neg") == [None])
    c.call("SET", "str", "v")
    for cmd in (("GEOADD", "str", "1", "2", "a"), ("GEOPOS", "str", "a"), ("GEODIST", "str", "a", "b"),
                ("GEOSEARCH", "str", "FROMLONLAT", "1", "2", "BYRADIUS", "1", "m")):
        got = c.call(*cmd)
        check("local: %s of a string gives WRONGTYPE" % cmd[0], isinstance(got, Error) and got.startswith("WRONGTYPE"), repr(got))
    # GEOSEARCH reads the 9 boxes in the order of Redis: a reply in that
    # order, from a set where the order of score would differ.
    pts = []
    for i in range(60):
        pts += ["%.4f" % (2 + (i % 8) * 0.07), "%.4f" % (48 + (i // 8) * 0.05), "q%d" % i]
    c.call("GEOADD", "order", *pts)
    got = c.call("GEOSEARCH", "order", "FROMLONLAT", "2.25", "48.2", "BYRADIUS", "20", "km")
    check("local: GEOSEARCH gives the members of the boxes in their order",
          isinstance(got, list) and len(got) > 3 and got == GEO_ORDER_WANT, repr(got)[:120])
    # Near the edge of its box the step is one less, as Redis makes it;
    # the order of the reply shows the boxes (redis-server 7.0 gives it).
    near = ['-69.28528', '79.45329', 'k0', '-68.51768', '80.68773', 'k1', '-67.85737', '77.09054', 'k2',
            '-64.85591', '77.85720', 'k3', '-64.10863', '79.47066', 'k4', '-67.33019', '80.95595', 'k5',
            '-63.91068', '79.88423', 'k6', '-66.93099', '81.94326', 'k7', '-66.75277', '76.74126', 'k8',
            '-68.05013', '77.16314', 'k9', '-66.73072', '80.30099', 'k10', '-67.38640', '77.63146', 'k11']
    c.call("GEOADD", "near", *near)
    got = c.call("GEOSEARCH", "near", "FROMLONLAT", "-63.5937", "81.3621", "BYRADIUS", "200", "km")
    check("local: GEOSEARCH takes a step less near the edge of the box, as Redis does",
          got == ["k1", "k10", "k5", "k7", "k6"], repr(got))
    c.call("SET", "str", "v")
    # Redis drops a box outside the search area, also when a member in it
    # is in the radius: p0 is in the radius, and the reply is empty.
    c.call("GEOADD", "prune", "-31.3558", "85", "p0")
    check("local: GEOSEARCH leaves out the boxes outside the search area, as Redis does",
          c.call("GEOSEARCH", "prune", "FROMLONLAT", "0", "80", "BYRADIUS", "1200", "km") == [])
    # South of the equator the bounding box takes the other edge, so the
    # boxes and the order change (redis-server 7.0 gives this order).
    c.call("GEOADD", "south", "23.8262", "-67.2450", "p0", "9.4288", "-35.6265", "p1")
    got = c.call("GEOSEARCH", "south", "FROMLONLAT", "7.4526", "-47.7530", "BYRADIUS", "2398855", "m")
    check("local: GEOSEARCH south of the equator takes the bounding box of Redis", got == ["p0", "p1"], repr(got))
    for cmd, want in ((("GEOSEARCH", "str", "FROMLONLAT", "999", "0", "BYRADIUS", "1", "m"), "WRONGTYPE"),
                      (("GEOSEARCH", "g8", "FROMMEMBER", "nosuch", "BYRADIUS", "1", "xx"), "ERR could not decode requested zset member"),
                      (("GEOSEARCH", "nokey", "FROMMEMBER", "a", "BYRADIUS", "1", "xx"), "ERR unsupported unit provided"),
                      (("GEOSEARCH", "g8", "FROMLONLAT", "2", "48", "FROMMEMBER", "Paris", "BYRADIUS", "1", "m"),
                       "ERR syntax error"),
                      (("GEOSEARCH", "g8", "FROMMEMBER", "Paris", "FROMLONLAT", "999", "48", "BYRADIUS", "1", "m"),
                       "ERR syntax error"),
                      (("GEOADD", "g9", "inf", "0", "x"), "ERR invalid longitude,latitude pair inf,0.000000"),
                      (("GEOADD", "g9", "-0", "100", "x"), "ERR invalid longitude,latitude pair -0.000000,100.000000")):
        got = c.call(*cmd)
        check("local: %s gives the error of Redis, in its order" % " ".join(cmd),
              isinstance(got, Error) and got.startswith(want), repr(got))
    got = c.call("GEOSEARCH", "g8", "FROMLONLAT", "100", "0", "FROMLONLAT", "2", "48", "BYRADIUS", "1", "m",
                 "BYRADIUS", "500", "km")
    check("local: GEOSEARCH takes the last of an option that comes again, as Redis does",
          isinstance(got, list) and sorted(got) == ["London", "Paris"], repr(got))
    got = c.call("GEOSEARCH", "g8", "FROMLONLAT", "0", "0", "BYRADIUS", "20000", "km")
    # redis-server 7.0 gives these three: the place at the top corner has
    # bit 52 set, past the range of every box.
    check("local: GEOSEARCH over the whole earth gives each member once",
          got == ["Paris", "Munich", "London"], repr(got))
    c.close()


# The reply of redis-server 7.0 for the order case above.
GEO_ORDER_WANT = ['q34', 'q42', 'q35', 'q36', 'q43', 'q44', 'q50', 'q51', 'q52', 'q37', 'q45', 'q38', 'q46', 'q53',
                  'q54', 'q58', 'q59', 'q39', 'q47', 'q10', 'q11', 'q12', 'q18', 'q26', 'q19', 'q20', 'q27', 'q28',
                  'q13', 'q21', 'q29', 'q22', 'q30', 'q31', 'q32', 'q40', 'q33', 'q41', 'q49', 'q24', 'q17', 'q25']


def stages_geo_limits(prog):
    srv = Server(prog, [])
    try:
        names = ["local: G1 GEOSEARCH over 524288 members of the boxes stalls PING at most 1 s",
                 "local: G2 GEOADD of 16384 members stalls PING at most 1 s",
                 "local: GEOADD of more than 16384 members is refused",
                 "local: a GEOSEARCH whose client goes stops with the connection",
                 "local: EXEC refuses a GEOSEARCH over the element budget",
                 "local: GEOPOS of more than 16384 members is refused",
                 "local: G3 EXEC of GEOSEARCH stalls PING at most 1 s, and the searches over the limit are refused",
                 "local: G4 GEOADD parses its places in steps",
                 "local: GEOPOS spends the sorted set work of EXEC"]
        if aof_started(srv, names):
            c = Asker(srv)
            c.c.sock.settimeout(60)
            args = []
            for i in range(16385):
                args += ["1", "1", "x%d" % i]
            got = c.call("GEOADD", "lim", *args)
            check(names[2], isinstance(got, Error) and "16384" in got, repr(got))
            got = c.call("GEOPOS", "lim", *["x%d" % i for i in range(16385)])
            check("local: GEOPOS of more than 16384 members is refused", isinstance(got, Error) and "16384" in got, repr(got))
            # 524288 members in a square of about 1 km.
            for b in range(32):
                part = []
                for i in range(16384):
                    k = b * 16384 + i
                    part += ["%.6f" % (2 + (k % 724) * 0.00001), "%.6f" % (48 + (k // 724) * 0.00001), "m%d" % k]
                c.raw().send_raw(encode("GEOADD", "dense", *part))
                c.raw().try_read(60)
            got = [None]

            def search():
                c.raw().send_raw(encode("GEOSEARCH", "dense", "FROMLONLAT", "2.004", "48.004", "BYRADIUS", "100", "km"))
                got[0] = c.raw().try_read(60)
            worst, wrong, _ = measure_while(srv, search)
            check(names[0], worst <= 1.0 and not wrong and isinstance(got[0], list) and len(got[0]) == 524288,
                  "worst %.3f s, %r" % (worst, len(got[0]) if isinstance(got[0], list) else got[0]))
            # A GEOSEARCH whose client goes stops its filter with the
            # connection.
            def geo_clients():
                info = c.call("INFO", "clients")
                for line in (info or "").split("\r\n"):
                    if line.startswith("connected_clients:"):
                        return int(line.split(":")[1])
                return -1
            base = geo_clients()
            for _ in range(3):
                g = Client(srv.port)
                g.send_raw(encode("GEOSEARCH", "dense", "FROMLONLAT", "2.004", "48.004", "BYRADIUS", "100", "km"))
                time.sleep(0.05)
                g.close()
            deadline = time.time() + 1.5
            now = geo_clients()
            while now != base and time.time() < deadline:
                time.sleep(0.05)
                now = geo_clients()
            check("local: a GEOSEARCH whose client goes stops with the connection", now == base, "%d, %d before" % (now, base))
            # G3: one EXEC walks at most 2097152 members of boxes. With one
            # member more (524289, which does not divide the limit), 3
            # GEOSEARCH run and 61 are refused; a refused walk spends what is
            # left, so the others walk no more. With 64 of them, a walk that
            # did not stop, or did not spend, would stall for seconds.
            c.call("GEOADD", "dense", "2.0045", "48.0045", "extra")
            reply = [None]

            def exec_searches():
                x = Client(srv.port)
                x.sock.settimeout(120)
                x.send_raw(encode("MULTI") + encode("GEOSEARCH", "dense", "FROMLONLAT", "2.004", "48.004", "BYRADIUS", "100", "km") * 64
                           + encode("EXEC"))
                for _ in range(65):
                    x.read()
                reply[0] = x.read()
                x.close()
            worst, wrong, _ = measure_while(srv, exec_searches)
            r = reply[0]
            check("local: G3 EXEC of GEOSEARCH stalls PING at most 1 s, and the searches over the limit are refused",
                  worst <= 1.0 and not wrong and isinstance(r, list) and len(r) == 64 and all(len(x) == 524289 for x in r[:3])
                  and all(isinstance(e, Error) and "GEOSEARCH" in e for e in r[3:]), "worst %.3f s, %r" % (worst, repr(r)[-80:]))
            c.call("ZREM", "dense", "extra")
            # The element budget: 8 ZRANGE WITHSCORES of 524288 fill it, and a
            # GEOSEARCH after them is refused.
            x = Client(srv.port)
            x.sock.settimeout(120)
            x.send_raw(encode("MULTI") + encode("ZRANGE", "dense", "0", "-1", "WITHSCORES") * 8
                       + encode("GEOSEARCH", "dense", "FROMLONLAT", "2.004", "48.004", "BYRADIUS", "100", "km") + encode("EXEC"))
            for _ in range(10):
                x.read()
            reply = x.read()
            x.close()
            check("local: EXEC refuses a GEOSEARCH over the element budget",
                  isinstance(reply, list) and len(reply) == 9 and isinstance(reply[8], Error)
                  and "elements" in reply[8], repr(reply[8])[:80] if isinstance(reply, list) else repr(reply)[:80])
            more = []
            for i in range(16384):
                more += ["%.6f" % (10 + i * 0.0001), "%.6f" % (20 + i * 0.0001), "n%d" % i]

            def geoadd():
                c.raw().send_raw(encode("GEOADD", "dense", *more))
                c.raw().try_read(60)
            worst, wrong, _ = measure_while(srv, geoadd)
            check(names[1], worst <= 1.0 and not wrong and c.call("ZCARD", "dense") == 524288 + 16384, "worst %.3f s" % worst)
            # G4: the same GEOADD again changes nothing, so its parse is the
            # work: with a yield every 1024 places it stalls little (rule 2).
            worst, wrong, _ = measure_while(srv, geoadd)
            check("local: G4 GEOADD parses its places in steps", worst <= 0.1 and not wrong, "worst %.3f s" % worst)
            # GEOPOS spends the sorted set work of EXEC: two of 16384 names
            # fill it, and the third is refused.
            names16 = ["m%d" % i for i in range(16384)]
            x = Client(srv.port)
            x.sock.settimeout(60)
            x.send_raw(encode("MULTI") + encode("GEOPOS", "dense", *names16) * 3 + encode("EXEC"))
            for _ in range(4):
                x.read()
            reply = x.read()
            x.close()
            check("local: GEOPOS spends the sorted set work of EXEC",
                  isinstance(reply, list) and len(reply) == 3 and isinstance(reply[2], Error), repr(reply)[-80:])
            c.close()
    finally:
        srv.stop()


def stages_geo_replication(prog):
    srv = Server(prog, [])
    try:
        name = "local: GEOADD goes to the replicas as the ZADD that it does"
        if aof_started(srv, [name]):
            r, _, _ = replica_handshake(srv.port)
            c = Asker(srv)
            c.call("GEOADD", "rg", "2.2944692", "48.8584625", "Paris")
            c.call("GEOADD", "rg", "2.2944692", "48.8584625", "Paris")
            c.call("SET", "end", "1")
            got = [r.try_read(2), r.try_read(2)]
            check(name, got == [["ZADD", "rg", "3663832614298053", "Paris"], ["SET", "end", "1"]], repr(got))
            r.close()
            c.close()
    finally:
        srv.stop()
    m = FakeMaster()
    rep = Server(prog, ["--replicaof", "localhost %d" % m.port])
    try:
        name = "local: a replica refuses GEOADD from a client"
        if aof_started(rep, [name]):
            link = m.accept()
            fake_handshake(link)
            c = Asker(rep)
            got = c.call("GEOADD", "q", "1", "2", "a")
            check(name, isinstance(got, Error) and got.startswith("READONLY"), repr(got))
            c.close()
    finally:
        rep.stop()
        m.close()


def stages_bitmaps(prog):
    srv = Server(prog, [])
    try:
        check("bitmaps: server starts", srv.up, srv.log_text())
        if not srv.up:
            return
        stages_bitmaps_on(srv)
    finally:
        srv.stop()
    stages_bitmaps_limits(prog)
    stages_bitmaps_replication(prog)


def stages_bitmaps_on(srv):
    c = Asker(srv)
    check("bitmaps-01 SETBIT gives the old bit", c.call("SETBIT", "bitmap_key", "3", "1") == 0)
    c.call("SETBIT", "bit_key", "2", "1")
    check("bitmaps-02 GETBIT of a set bit gives 1", c.call("GETBIT", "bit_key", "2") == 1)
    check("bitmaps-02 GETBIT of a bit that is not set gives 0", c.call("GETBIT", "bit_key", "3") == 0)
    check("bitmaps-02 GETBIT of a missing key gives 0", c.call("GETBIT", "new_key", "2") == 0)
    c.call("SET", "mykey", "A")
    check("bitmaps-03 GETBIT reads a string from SET", [c.call("GETBIT", "mykey", o) for o in ("1", "7", "0")] == [1, 1, 0])
    c.call("SETBIT", "mykey4", "1", "1")
    check("bitmaps-04 GET reads a bitmap", c.call("GET", "mykey4") == "@")
    c.call("SETBIT", "grow", "1", "1")
    check("bitmaps-05 STRLEN of a bitmap of one byte gives 1", c.call("STRLEN", "grow") == 1)
    check("bitmaps-05 SETBIT in a second byte gives 0", c.call("SETBIT", "grow", "10", "1") == 0)
    check("bitmaps-05 GETBIT of the new bit gives 1", c.call("GETBIT", "grow", "10") == 1)
    check("bitmaps-05 STRLEN after the growth gives 2", c.call("STRLEN", "grow") == 2)
    c.call("SETBIT", "bc", "1", "1")
    c.call("SETBIT", "bc", "10", "1")
    got = [c.call("BITCOUNT", "bc"), c.call("BITCOUNT", "bc", "0", "0"), c.call("BITCOUNT", "bc", "1", "1"),
           c.call("BITCOUNT", "bc", "0", "1"), c.call("BITCOUNT", "bc", "1", "5")]
    check("bitmaps-06 BITCOUNT counts the set bits", got == [2, 1, 1, 2, 1], repr(got))
    for k, offs in (("k1", ("0", "4")), ("k2", ("0", "6"))):
        for o in offs:
            c.call("SETBIT", k, o, "1")
    check("bitmaps-07 BITOP AND gives the length", c.call("BITOP", "AND", "dest", "k1", "k2") == 1)
    check("bitmaps-07 BITOP AND keeps the common bits", [c.call("GETBIT", "dest", o) for o in ("0", "4")] == [1, 0])
    c.call("SETBIT", "k3", "1", "1")
    c.call("SETBIT", "k3", "10", "1")
    c.call("SETBIT", "k4", "1", "1")
    check("bitmaps-08 BITOP AND of different lengths gives the longest", c.call("BITOP", "AND", "dest8", "k3", "k4") == 2)
    check("bitmaps-08 BITOP AND reads a missing byte as 0", [c.call("GETBIT", "dest8", o) for o in ("1", "10")] == [1, 0])
    check("bitmaps-09 BITOP OR gives the length", c.call("BITOP", "OR", "dest9", "k1", "k2") == 1)
    check("bitmaps-09 BITOP OR keeps a bit of one source", c.call("GETBIT", "dest9", "4") == 1)
    check("bitmaps-09 BITOP OR of different lengths", c.call("BITOP", "OR", "dest10", "k3", "k4") == 2
          and c.call("GETBIT", "dest10", "10") == 1)

    # Local cases.
    check("local: BITOP XOR and NOT", c.call("BITOP", "XOR", "x", "k1", "k2") == 1 and c.call("GET", "x") == chr(0x0a)
          and c.call("BITOP", "NOT", "n", "k1") == 1 and c.call("GET", "n") == chr(0x77))
    check("local: BITOP of missing keys only removes the destination",
          c.call("BITOP", "AND", "dest", "nokey", "nokey2") == 0 and c.call("TYPE", "dest") == "none")
    got = [c.call("BITCOUNT", "bc", "0", "-1"), c.call("BITCOUNT", "bc", "1", "10", "BIT"),
           c.call("BITCOUNT", "bc", "-5", "-1", "bit"), c.call("BITCOUNT", "bc", "2", "1"), c.call("BITCOUNT", "nokey")]
    check("local: BITCOUNT with negative indexes and in bits", got == [2, 2, 0, 0, 0], repr(got))
    c.call("SET", "abc", "abc")
    got = [c.call("BITCOUNT", "abc", "-100", "-200"), c.call("BITCOUNT", "abc", "-1", "-4", "foo"),
           c.call("BITCOUNT", "abc", "0", "-100"), c.call("BITCOUNT", "abc", "0", "4", "BIT")]
    check("local: BITCOUNT with two negative indexes and start after end gives 0, as Redis 7.0 does", got[:2] == [0, 0], repr(got))
    check("local: BITCOUNT clamps a negative end to 0", got[2] == 3, repr(got))
    check("local: BITCOUNT in bits stops at the last bit", got[3] == 2, repr(got))
    t0 = time.time()
    got = [c.call("BITCOUNT", "abc", "0", "999999999999999"), c.call("BITCOUNT", "abc", "0", "9223372036854775807"),
           c.call("BITCOUNT", "abc", "0", "9223372036854775808")]
    check("local: BITCOUNT clamps an end past the string, also of 19 digits, and replies at once",
          got[:2] == [10, 10] and isinstance(got[2], Error) and time.time() - t0 < 1.0, "%r in %.2f s" % (got, time.time() - t0))
    got = [c.call("BITCOUNT", "abc", "-99999999999999999", "-1"), c.call("BITCOUNT", "abc", "-9223372036854775808", "-1"),
           c.call("BITCOUNT", "abc", "0", "0999999999999999999"), c.call("BITCOUNT", "abc", "-9223372036854775809", "-1")]
    check("local: BITCOUNT reads a negative index of 17 digits as before the start", got[0] == 10, repr(got))
    check("local: BITCOUNT reads the least 64-bit index", got[1] == 10, repr(got))
    check("local: BITCOUNT refuses an index of 19 digits with a leading zero, and one past the 64-bit range",
          isinstance(got[2], Error) and isinstance(got[3], Error), repr(got))
    check("local: BITOP OR keeps a bit of both sources", c.call("GETBIT", "dest9", "0") == 1)
    check("local: GETBIT past the end gives 0", c.call("GETBIT", "bc", "100") == 0 and c.call("GETBIT", "mykey", "9") == 0)
    check("local: BITCOUNT of a missing key gives 0 before it reads the arguments, as Redis does",
          c.call("BITCOUNT", "nokey", "0") == 0 and c.call("BITCOUNT", "nokey", "a", "b") == 0)
    for cmd, want in ((("SETBIT", "x", "-1", "1"), "ERR bit offset is not an integer or out of range"),
                      (("SETBIT", "x", "4294967296", "1"), "ERR bit offset is not an integer or out of range"),
                      (("SETBIT", "x", "1", "2"), "ERR bit is not an integer or out of range"),
                      (("GETBIT", "x", "abc"), "ERR bit offset is not an integer or out of range"),
                      (("BITCOUNT", "bc", "0"), "ERR syntax error"),
                      (("BITCOUNT", "bc", "0", "1", "FOO"), "ERR syntax error"),
                      (("BITCOUNT", "bc", "a", "b"), "ERR value is not an integer or out of range"),
                      (("BITOP", "FOO", "d", "k1"), "ERR syntax error"),
                      (("BITOP", "NOT", "d", "k1", "k2"), "ERR BITOP NOT must be called with a single source key."),
                      (("SETBIT", "x", "1"), "ERR wrong number of arguments for 'setbit'"),
                      (("STRLEN",), "ERR wrong number of arguments for 'strlen'")):
        got = c.call(*cmd)
        check("local: %s gives the error of Redis" % " ".join(cmd), isinstance(got, Error) and got.startswith(want), repr(got))
    c.call("LPUSH", "l", "a")
    for cmd in (("SETBIT", "l", "1", "1"), ("GETBIT", "l", "1"), ("STRLEN", "l"), ("BITCOUNT", "l"), ("BITOP", "AND", "d", "l")):
        got = c.call(*cmd)
        check("local: %s of a list gives WRONGTYPE" % cmd[0], isinstance(got, Error) and got.startswith("WRONGTYPE"), repr(got))
    check("local: BITOP to a list key replaces it", c.call("BITOP", "AND", "l", "k1") == 1 and c.call("TYPE", "l") == "string")
    c.call("SET", "num", "10")
    check("local: INCR reads a string that SETBIT changed", c.call("SETBIT", "num", "15", "1") == 0 and c.call("GET", "num") == "11"
          and c.call("INCR", "num") == 12)
    c.call("SET", "s", "hello")
    c.call("SETBIT", "s", "0", "1")
    check("local: SET GET gives the old value of a bitmap", c.call("SET", "s", "x", "GET") == chr(ord("h") | 0x80) + "ello"
          and c.call("GET", "s") == "x")
    check("local: SETBIT 0 on a new key makes a zero byte", c.call("SETBIT", "z", "0", "0") == 0
          and c.call("GET", "z") == "\x00" and c.call("STRLEN", "z") == 1)
    # Across chunks: bits in many chunks, then GET, GETBIT and BITCOUNT agree
    # with a model.
    model = bytearray(20000)
    for o in (0, 7, 8191, 32767, 32768, 65535, 100001, 159999):
        c.call("SETBIT", "chunks", str(o), "1")
        model[o // 8] |= 0x80 >> (o % 8)
    c.call("SETBIT", "chunks", "32767", "0")
    model[32767 // 8] &= ~(0x80 >> 7)
    got = c.call("GET", "chunks")
    check("local: a bitmap over several chunks reads back as written",
          isinstance(got, str) and got.encode("latin-1") == bytes(model) and c.call("STRLEN", "chunks") == 20000
          and c.call("BITCOUNT", "chunks") == sum(bin(b).count("1") for b in model)
          and c.call("BITCOUNT", "chunks", "1000", "15000") == sum(bin(b).count("1") for b in model[1000:15001]),
          repr(got)[:60])
    # WATCH: SETBIT and BITOP mark the watchers.
    w = Asker(srv)
    w.call("WATCH", "k1")
    c.call("SETBIT", "k1", "1", "1")
    w.call("MULTI")
    w.call("PING")
    check("local: SETBIT fails EXEC of a client that watches the key", w.call("EXEC") is None)
    w.call("WATCH", "dest7")
    c.call("BITOP", "OR", "dest7", "k1", "k2")
    w.call("MULTI")
    w.call("PING")
    check("local: BITOP fails EXEC of a client that watches the destination", w.call("EXEC") is None)
    w.call("WATCH", "dest7")
    c.call("BITOP", "AND", "dest7", "nokey")
    w.call("MULTI")
    w.call("PING")
    check("local: a BITOP that removes the destination fails EXEC of a client that watches it", w.call("EXEC") is None)
    w.close()
    c.close()


def stages_bitmaps_limits(prog):
    srv = Server(prog, [])
    try:
        names = ["local: B1 SETBIT at the largest offset stalls PING at most 1 s",
                 "local: B1 the bitmap is 512 MB with the bit set",
                 "local: B2 BITCOUNT of a dense 8 MB stalls PING at most 1 s",
                 "local: B3 BITCOUNT of a sparse bitmap of 512 MB replies within 2 s",
                 "local: B4 BITOP of 1048576 source bytes stalls PING at most 1 s",
                 "local: BITOP over 1048576 source bytes is refused",
                 "local: B5 GET of a bitmap of 64 MB stalls PING at most 1 s",
                 "local: EXEC refuses bitmap work over 1048576",
                 "local: BITCOUNT counts the bitmap as it was when it ran",
                 "local: B6 INCR of a bitmap of 512 MB stalls PING at most 1 s",
                 "local: the split of a string by SETBIT spends the bitmap work of EXEC",
                 "local: BITOP spends the bitmap work of EXEC",
                 "local: the copy of GET spends the bitmap work of EXEC",
                 "local: SET GET over the bitmap work of EXEC does not write",
                 "local: the copy of BITCOUNT spends the bitmap work of EXEC",
                 "local: GET of a bitmap writes it as it was when it ran",
                 "local: a BITCOUNT whose client goes stops with the connection"]
        if aof_started(srv, names):
            c = Asker(srv)
            c.c.sock.settimeout(120)
            got = [None]

            def setbit_max():
                got[0] = c.call("SETBIT", "max", "4294967295", "1")
            worst, wrong, _ = measure_while(srv, setbit_max)
            check(names[0], worst <= 1.0 and not wrong and got[0] == 0, "worst %.3f s" % worst)
            check(names[1], c.call("STRLEN", "max") == 536870912 and c.call("GETBIT", "max", "4294967295") == 1)
            # A dense 8 MB: each byte 0x01, kept as Bytes after one SETBIT.
            c.call("SET", "b8", "\x01" * (8 * 1024 * 1024))
            c.call("SETBIT", "b8", "0", "0")

            def bitcount8():
                c.raw().send_raw(encode("BITCOUNT", "b8", "0", "-1"))
                got[0] = c.raw().try_read(60)
            worst, wrong, _ = measure_while(srv, bitcount8)
            check(names[2], worst <= 1.0 and not wrong and got[0] == 8 * 1024 * 1024, "worst %.3f s, %r" % (worst, got[0]))
            # BITCOUNT counts the bitmap as it was when it ran: a bit that a
            # SETBIT sets near the end while the count is near the start
            # does not count.
            x = Client(srv.port)
            x.sock.settimeout(60)
            x.send_raw(encode("BITCOUNT", "b8", "0", "-1"))
            time.sleep(0.3)
            c.call("SETBIT", "b8", str(8 * 8 * 1024 * 1024 - 100), "1")
            got_count = x.read()
            again = x.call("BITCOUNT", "b8")
            x.close()
            check("local: BITCOUNT counts the bitmap as it was when it ran",
                  got_count == 8 * 1024 * 1024 and again == 8 * 1024 * 1024 + 1, repr((got_count, again)))
            t0 = time.time()
            got = c.call("BITCOUNT", "max")
            check(names[3], got == 1 and time.time() - t0 < 2.0, "%r in %.2f s" % (got, time.time() - t0))
            c.call("SET", "h1", "\xff" * (1 << 19))
            c.call("SET", "h2", "\x0f" * (1 << 19))
            got = [None]

            def bitop():
                got[0] = c.call("BITOP", "AND", "hd", "h1", "h2")
            worst, wrong, _ = measure_while(srv, bitop)
            check(names[4], worst <= 1.0 and not wrong and got[0] == 524288 and c.call("GETBIT", "hd", "4") == 1
                  and c.call("GETBIT", "hd", "3") == 0, "worst %.3f s" % worst)
            got = c.call("BITOP", "AND", "hd", "h1", "h2", "h1")
            check(names[5], isinstance(got, Error) and "1048576" in got, repr(got))
            c.call("SETBIT", "b64", str(64 * 8 * 1024 * 1024 - 1), "1")
            got = [None]

            def get64():
                c.raw().send_raw(encode("GET", "b64"))
                got[0] = c.raw().try_read(60)
            worst, wrong, _ = measure_while(srv, get64)
            check(names[6], worst <= 1.0 and not wrong and isinstance(got[0], str) and len(got[0]) == 64 * 1024 * 1024
                  and got[0][-1] == "\x01", "worst %.3f s" % worst)
            got = [None]

            def incr_max():
                got[0] = c.call("INCR", "max")
            worst, wrong, _ = measure_while(srv, incr_max)
            check("local: B6 INCR of a bitmap of 512 MB stalls PING at most 1 s",
                  worst <= 1.0 and not wrong and isinstance(got[0], Error), "worst %.3f s, %r" % (worst, got[0]))
            # 7 SETBIT at the largest offset on new keys are 7 x 131073
            # chunks, under 1048576; the eighth is over.
            x = Client(srv.port)
            x.sock.settimeout(60)
            x.send_raw(encode("MULTI") + b"".join(encode("SETBIT", "e%d" % i, "4294967295", "1") for i in range(8)) + encode("EXEC"))
            for _ in range(9):
                x.read()
            reply = x.read()
            x.close()
            check(names[7], isinstance(reply, list) and reply[:7] == [0] * 7 and isinstance(reply[7], Error)
                  and "bitmap work" in reply[7] and c.call("TYPE", "e7") == "none", repr(reply)[-80:])
            # Each kind of bitmap work spends the budget of EXEC: a BITOP at
            # the limit takes all of it, so what comes after is refused.
            c.call("SET", "p1", "x")
            c.call("SETBIT", "small", "0", "1")

            def exec_after_bitop(*cmd):
                x = Client(srv.port)
                x.sock.settimeout(60)
                x.send_raw(encode("MULTI") + encode("BITOP", "AND", "hd", "h1", "h2") + encode(*cmd) + encode("EXEC"))
                for _ in range(3):
                    x.read()
                reply = x.read()
                x.close()
                return reply
            # SET ... GET over the budget does not write.
            reply = exec_after_bitop("SET", "small", "x", "GET")
            check("local: SET GET over the bitmap work of EXEC does not write",
                  isinstance(reply, list) and isinstance(reply[1], Error) and c.call("GETBIT", "small", "0") == 1, repr(reply)[-80:])
            for name, cmd in (("the split of a string by SETBIT", ("SETBIT", "p1", "0", "1")),
                              ("BITOP", ("BITOP", "NOT", "hn", "p1")),
                              ("the copy of GET", ("GET", "small")),
                              ("the copy of BITCOUNT", ("BITCOUNT", "small"))):
                reply = exec_after_bitop(*cmd)
                check("local: %s spends the bitmap work of EXEC" % name,
                      isinstance(reply, list) and reply[0] == 524288 and isinstance(reply[1], Error)
                      and "bitmap work" in reply[1], repr(reply)[-80:])
            # GET writes a copy of the chunks: a SETBIT while the reply waits
            # does not change it.
            x = Client(srv.port)
            x.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
            x.sock.settimeout(60)
            x.send_raw(encode("GET", "b64"))
            time.sleep(0.3)
            c.call("SETBIT", "b64", str(64 * 8 * 1024 * 1024 - 1000), "1")
            x.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 22)
            val = x.read()
            x.close()
            check("local: GET of a bitmap writes it as it was when it ran",
                  isinstance(val, str) and len(val) == 64 * 1024 * 1024 and val[-125] == "\x00" and val[-1] == "\x01",
                  repr(val[-130:-120]) if isinstance(val, str) else repr(val))
            # A BITCOUNT whose client goes stops: the connection leaves the
            # count of connected clients at once.
            def clients():
                info = c.call("INFO", "clients")
                for line in (info or "").split("\r\n"):
                    if line.startswith("connected_clients:"):
                        return int(line.split(":")[1])
                return -1
            base = clients()
            for _ in range(5):
                g = Client(srv.port)
                g.send_raw(encode("BITCOUNT", "b8"))
                time.sleep(0.05)
                g.close()
            deadline = time.time() + 1.5
            now = clients()
            while now != base and time.time() < deadline:
                time.sleep(0.05)
                now = clients()
            check("local: a BITCOUNT whose client goes stops with the connection", now == base, "%d, %d before" % (now, base))
            c.close()
    finally:
        srv.stop()


def stages_bitmaps_replication(prog):
    srv = Server(prog, [])
    try:
        name = "local: SETBIT and BITOP are propagated"
        if aof_started(srv, [name]):
            r, _, _ = replica_handshake(srv.port)
            c = Asker(srv)
            c.call("SETBIT", "rb", "3", "1")
            c.call("BITOP", "NOT", "rd", "rb")
            c.call("BITOP", "AND", "rd", "nokey")
            c.call("SET", "end", "1")
            got = [r.try_read(2) for _ in range(4)]
            check(name, got == [["SETBIT", "rb", "3", "1"], ["BITOP", "NOT", "rd", "rb"], ["BITOP", "AND", "rd", "nokey"],
                                ["SET", "end", "1"]], repr(got))
            r.close()
            c.close()
    finally:
        srv.stop()
    m = FakeMaster()
    rep = Server(prog, ["--replicaof", "localhost %d" % m.port])
    try:
        names = ["local: a replica refuses SETBIT and BITOP from a client", "local: a replica applies SETBIT of its master"]
        if aof_started(rep, names):
            link = m.accept()
            fake_handshake(link)
            c = Asker(rep)
            got = [c.call("SETBIT", "q", "1", "1"), c.call("BITOP", "NOT", "q2", "q")]
            check(names[0], all(isinstance(g, Error) and g.startswith("READONLY") for g in got), repr(got))
            link.send_raw(encode("SETBIT", "q", "1", "1"))
            time.sleep(0.3)
            check(names[1], c.call("GET", "q") == "@")
            c.close()
    finally:
        rep.stop()
        m.close()


GROUPS = [
    ("base", stages_base),
    ("lists", stages_lists),
    ("streams", stages_streams),
    ("transactions", stages_transactions),
    ("optimistic-locking", stages_optimistic_locking),
    ("persistence-rdb", stages_rdb),
    ("replication", stages_replication),
    ("persistence-aof", stages_aof),
    ("pub-sub", stages_pubsub),
    ("sorted-sets", stages_zset),
    ("geospatial", stages_geo),
    ("bitmaps", stages_bitmaps),
    ("fairness", stages_fairness),
    ("query limit", stages_query_limit),
]


def main():
    prog = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else DEFAULT_PROG)
    if not os.path.exists(prog):
        print("entrypoint not found: %s" % prog)
        return 1
    random.seed(1)
    # REDIS_STAGE_GROUPS=a,b runs only those groups: a mutation run that
    # names cases of one group needs only that group.
    only = os.environ.get("REDIS_STAGE_GROUPS")
    for name, run in GROUPS:
        if only and name not in only.split(","):
            continue
        print("== %s" % name)
        try:
            run(prog)
        except Exception as e:  # a crash in one group must not hide the rest
            check("%s: group ran to the end" % name, False, repr(e))
    print("\n%d passed, %d failed" % (PASS, FAIL))
    return 0 if FAIL == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
