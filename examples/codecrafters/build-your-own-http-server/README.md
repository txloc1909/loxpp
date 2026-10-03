# Build your own HTTP server (Codecrafters) in Lox++

A complete implementation of the public
[Build your own HTTP server](https://github.com/codecrafters-io/build-your-own-http-server)
challenge in pure Lox++. No membership is needed: the stage cases are
transcribed from the public `stage_descriptions/*.md` files, and local
regression cases are marked `local:`. A second battery diffs against
Python's `http.client`/`gzip`/`zlib` as an independent oracle.

The HTTP/1.1 parser, the router, and the `gzip` encoder (DEFLATE + CRC32) all
run **inside the VM**. Lox++ has no bitwise operators, so the DEFLATE bit
stream and the CRC32 checksum are built with arithmetic (`/`, `%`,
`math.floor`) over the 32-bit range.

## Layout

| File | Role |
|---|---|
| `http.lox` | HTTP server, parser, router, gzip — all pure Lox++ |
| `your_http.sh` | Challenge entrypoint: resolves `loxpp`, passes `--directory` through |
| `tests/run_stages.py` | Transcribed stage cases and `local:` regression cases (raw TCP, decodes gzip with Python) |
| `tests/diff_http.py` | Differential vs Python `http.client` + `gzip` + `zlib` |
| `tests/gaps.sh` | Evidence for the one unreachable stage (not run in CI) |

## Stage coverage

| Stage | Status |
|---|---|
| base-01 bind to a port | pass |
| base-02 respond 200 | pass |
| base-03 respond 404 | pass |
| base-04 `/echo/{str}` | pass |
| base-05 `/user-agent` | pass |
| base-06 concurrent connections | pass |
| base-07 GET `/files/{filename}` | pass |
| base-08 POST `/files/{filename}` | pass |
| compression-01 `Accept-Encoding` header | pass |
| compression-02 multiple schemes | pass |
| compression-03 `gzip` | pass |
| persistent-01 keep-alive | pass |
| persistent-02 concurrent keep-alive | **blocked** — no concurrency |
| persistent-03 `Connection: close` | pass |

Thirteen of fourteen. The one that fails is a genuine capability gap, not a
library: it needs a primitive the language cannot compose from what it already
exposes. `tests/gaps.sh` reproduces it.

### Gap — persistent-02: no concurrency

Two keep-alive connections open at once need threads or an event loop. One
`accept()`/`readline()` blocks the whole VM, so while connection A is open,
connection B is never accepted. A serial server passes `base-06` and
`persistent-01` because those clients close after reading their response, but
`persistent-02` holds both connections open simultaneously.

## One VM change this example depends on

The sockets merged in #516 used stdio `r+` streams, where reading and then
writing requires a file-positioning call — impossible on a socket. `readline()`
followed by `write()` failed with `ESPIPE` unless the read had hit EOF (which
is why `socket_echo.lox`, which reads to EOF, passed). An HTTP server always
reads a request line and then writes a response, so this blocked every stage.
The fix opens socket streams unbuffered (`setvbuf(..., _IONBF, ...)`), which
never needs to seek, keeping `Socket` the true bidirectional byte stream
`spec/05-stdlib.md` already promised. `NetProcessTest.SocketReadlineThenWriteInterleaves`
pins it.

## Bounded reads

`base-08` needs a `Content-Length` body, which no line- or EOF-delimited read
can frame. `Socket.read_bytes(n)` (and `Process.read_bytes`/`err_read_bytes`)
blocks until `n` bytes arrive or the peer closes, then returns what it read;
at end of stream it returns `""`. The server reads the body with one call, so
the bytes are consumed before the next request on a keep-alive connection.

## gzip

`gzip` is fixed-Huffman DEFLATE with literals only (no LZ77 back-references),
so a very short body can grow — the challenge explicitly allows this. For
`GET /echo/abc` the encoder produces exactly the bytes the stage description
shows:

```
1f 8b 08 00 00 00 00 00 00 03 4b 4c 4a 06 00 c2 41 24 35 03 00 00 00
```

A `--gzip-probe <in> <out>` mode compresses a file with the same encoder. A URL
path cannot carry every byte value, so the tests use it to reach the 9-bit
literal codes (bytes 144-255). The server starts when the flag is absent.

## Run

```sh
./examples/codecrafters/build-your-own-http-server/your_http.sh --directory /tmp
curl -v http://localhost:4221/echo/hello
python3 examples/codecrafters/build-your-own-http-server/tests/run_stages.py
python3 examples/codecrafters/build-your-own-http-server/tests/diff_http.py
./examples/codecrafters/build-your-own-http-server/tests/gaps.sh
```

The server binds `0.0.0.0:4221`; set `LOXPP_HTTP_PORT` to use another port.
Binary resolution: `build/loxpp` beside this checkout first, then `loxpp` on
`PATH`.
