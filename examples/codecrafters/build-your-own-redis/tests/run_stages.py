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
    def __init__(self, prog, args):
        self.port = free_port()
        self.log = tempfile.NamedTemporaryFile(
            prefix="loxpp-redis-", suffix=".log", delete=False)
        self.proc = subprocess.Popen(
            [prog, "--port", str(self.port)] + args,
            stdout=self.log, stderr=subprocess.STDOUT)
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
        got = c.call("GET", "L" + key)
        check("local: GET on a list key is WRONGTYPE",
              isinstance(got, Error) and got.startswith("WRONGTYPE"), repr(got))

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
        check("local: S7 XRANGE of 64 entries of almost 1 MB stalls PING at most 0.2 s",
              worst <= 0.2 and not wrong and size[0] >= 64 * len(field),
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
        check("local: W3 a second client that watches the same 1048575 keys stalls PING at most 0.3 s",
              worst <= 0.3 and not wrong and got[0] == "OK", "worst %.3f s, %r" % (worst, got[0]))
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
        out.append(value if isinstance(value, bytes) and value[:1] in (b"\xc0", b"\xc1", b"\xc2", b"\xc3")
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
            check("local: a missing RDB file is an empty keyspace", c.call("KEYS", "*") == [])
            c.close()
        finally:
            srv.stop()

        def with_rdb(name, entries, **kw):
            path = os.path.join(d, name)
            write_rdb(path, entries, **kw)
            return Server(prog, ["--dir", d, "--dbfilename", name])

        key = rand_word()
        srv = with_rdb("one.rdb", [(key, "orange", None, None)])
        try:
            c = srv.client()
            check("persistence-rdb-02 KEYS * gives the key", c.call("KEYS", "*") == [key])
            check("persistence-rdb-03 GET gives the value", c.call("GET", key) == "orange")
            c.close()
        finally:
            srv.stop()

        keys = [rand_word() for _ in range(4)]
        values = [rand_word() for _ in range(4)]
        srv = with_rdb("many.rdb", [(k, v, None, None) for k, v in zip(keys, values)])
        try:
            c = srv.client()
            check("persistence-rdb-04 KEYS * gives every key", sorted(c.call("KEYS", "*")) == sorted(keys))
            check("persistence-rdb-05 GET gives every value",
                  [c.call("GET", k) for k in keys] == values)
            c.close()
        finally:
            srv.stop()

        now = int(time.time() * 1000)
        live, dead, live_s = rand_word(), rand_word(), rand_word()
        srv = with_rdb("expiry.rdb", [(live, "a", now + 600000, "ms"), (dead, "b", now - 600000, "ms"),
                                      (live_s, "c", now + 600000, "s")])
        try:
            c = srv.client()
            check("persistence-rdb-06 a key with a future expiry in ms", c.call("GET", live) == "a")
            check("persistence-rdb-06 a key that has expired", c.call("GET", dead) is None)
            check("local: a key with a future expiry in seconds", c.call("GET", live_s) == "c")
            check("local: KEYS skips the expired key", sorted(c.call("KEYS", "*")) == sorted([live, live_s]))
            c.close()
        finally:
            srv.stop()

        # Integer strings and the long length forms.
        long_value = "x" * 20000
        srv = with_rdb("forms.rdb", [("i8", b"\xc0\xf6", None, None), ("i16", b"\xc1\x30\xf8", None, None),
                                     ("i32", b"\xc2\x00\x94\x35\x77", None, None),
                                     (b"\xc0\x07", "int key", None, None),
                                     ("long", long_value, None, None),
                                     # LZF: a run of three bytes, then a back
                                     # reference of 6 bytes at offset 3.
                                     ("lzf", b"\xc3\x06\x09\x02abc\x80\x02", None, None),
                                     ("huge", "y" * 70000, None, None)])
        try:
            c = srv.client()
            check("local: RDB integer strings of 1, 2 and 4 bytes",
                  [c.call("GET", k) for k in ("i8", "i16", "i32")] == ["-10", "-2000", "2000000000"],
                  repr([c.call("GET", k) for k in ("i8", "i16", "i32")]))
            check("local: an RDB key stored as an integer", c.call("GET", "7") == "int key")
            check("local: an RDB string compressed with LZF", c.call("GET", "lzf") == "abcabcabc",
                  repr(c.call("GET", "lzf")))
            check("local: RDB lengths of 14 and 32 bits",
                  c.call("GET", "long") == long_value and c.call("GET", "huge") == "y" * 70000)
            c.close()
        finally:
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
            c.send_raw(encode("KEYS", "*"))
            got[0] = c.try_read(60)
            time.sleep(0.2)
        worst, wrong, _ = measure_while(srv, keys_all)
        check("local: K1 KEYS * of 524288 keys gives each one",
              isinstance(got[0], list) and len(got[0]) == n)
        check("local: K1 KEYS * of 524288 keys stalls PING at most 0.5 s",
              worst <= 0.5 and not wrong, "worst %.3f s" % worst)
        c.call("SET", "brief", "v", "PX", "100")
        time.sleep(0.3)
        got = c.call("KEYS", "brief*")
        check("local: KEYS skips a key that expired after the load", got == [], repr(got))
        for k in ("foo", "fob", "bar", "a*b"):
            c.call("SET", k, "1")
        patterns = {"fo?": ["fob", "foo"], "[ab]*": ["a*b", "bar"], "[^fk]*": ["a*b", "bar"],
                    "a\\*b": ["a*b"], "?o?": ["fob", "foo"], "[a-b]?r": ["bar"]}
        for pattern, want in patterns.items():
            got = c.call("KEYS", pattern)
            check("local: KEYS %s" % pattern, isinstance(got, list) and sorted(got) == want,
                  repr(got)[:80])
        c.call("SET", "slow", "a" * 100000)

        def keys_slow():
            c.send_raw(encode("KEYS", "*a" * 15 + "*b"))
            got[0] = c.try_read(120)
            time.sleep(0.2)
        worst, wrong, _ = measure_while(srv, keys_slow)
        check("local: K2 KEYS with a slow pattern gives no key", got[0] == [], repr(got[0])[:60])
        check("local: K2 KEYS with a slow pattern stalls PING at most 0.2 s",
              worst <= 0.2 and not wrong, "worst %.3f s" % worst)
        c.close()
    finally:
        srv.stop()


GROUPS = [
    ("base", stages_base),
    ("lists", stages_lists),
    ("streams", stages_streams),
    ("transactions", stages_transactions),
    ("optimistic-locking", stages_optimistic_locking),
    ("persistence-rdb", stages_rdb),
    ("fairness", stages_fairness),
    ("query limit", stages_query_limit),
]


def main():
    prog = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else DEFAULT_PROG)
    if not os.path.exists(prog):
        print("entrypoint not found: %s" % prog)
        return 1
    random.seed(1)
    for name, run in GROUPS:
        print("== %s" % name)
        try:
            run(prog)
        except Exception as e:  # a crash in one group must not hide the rest
            check("%s: group ran to the end" % name, False, repr(e))
    print("\n%d passed, %d failed" % (PASS, FAIL))
    return 0 if FAIL == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
