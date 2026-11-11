#!/usr/bin/env python3
# diff_redis.py — differential test of the Lox++ Redis server against
# redis-server, an independent oracle.
#
# Each script is a list of commands. The test sends the same script to a
# fresh redis-server and to a fresh Lox++ server, and compares the replies one
# by one, error texts included. The oracle is the redis-server of the dev
# image (7.0.15); another version can word an error in another way.
#
# Usage: tests/diff_redis.py [your_program.sh]

import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from run_stages import DEFAULT_PROG, Client, Error, Server, encode, free_port  # noqa: E402

import subprocess  # noqa: E402
import random  # noqa: E402
import socket  # noqa: E402
import time  # noqa: E402

PASS = 0
FAIL = 0

# One entry per group: (script name, [command, ...]). A command is a list of
# arguments.

def both_ends_script():
    """LPUSH, RPUSH, LPOP and LRANGE in a fixed random order, so that the
    elements cross between the two parts of the queue and the free slots
    are dropped many times."""
    rng = random.Random(1151)
    script = [["RPUSH", "q"] + [f"r{i}" for i in range(100)],
              ["LPOP", "q", "40"], ["LRANGE", "q", "0", "-1"]]
    n = 0
    for _ in range(400):
        op = rng.randrange(6)
        k = rng.randrange(1, 12)
        if op == 0:
            script.append(["LPUSH", "q"] + [f"l{n + i}" for i in range(k)])
            n += k
        elif op == 1:
            script.append(["RPUSH", "q"] + [f"r{n + i}" for i in range(k)])
            n += k
        elif op == 2:
            script.append(["LPOP", "q"])
        elif op == 3:
            script.append(["LPOP", "q", str(k)])
        elif op == 4:
            a, b = rng.randrange(-20, 20), rng.randrange(-20, 20)
            script.append(["LRANGE", "q", str(a), str(b)])
        else:
            script.append(["LLEN", "q"])
    script.append(["LRANGE", "q", "0", "-1"])
    return script



def random_stream_script():
    """XADD with IDs that are good, too low or partly given, and XRANGE and
    XREAD with random bounds and counts, in a fixed random order."""
    rng = random.Random(1112)
    script = []
    for _ in range(400):
        key = "st%d" % rng.randrange(3)
        op = rng.randrange(5)
        ms, seq = rng.randrange(0, 40), rng.randrange(0, 4)
        if op == 0:
            fields = []
            for i in range(rng.randrange(1, 4)):
                fields += ["f%d" % i, "v%d" % rng.randrange(100)]
            idarg = rng.choice(["%d-%d" % (ms, seq), "%d" % ms, "%d-*" % ms])
            script.append(["XADD", key, idarg] + fields)
        elif op in (1, 2):
            lo = rng.choice(["-", "+", "%d" % ms, "%d-%d" % (ms, seq)])
            hi = rng.choice(["-", "+", "%d" % (ms + 5), "%d-%d" % (ms + 5, seq)])
            cmd = ["XRANGE", key, lo, hi]
            if rng.randrange(3) == 0:
                cmd += ["COUNT", str(rng.randrange(-1, 4))]
            script.append(cmd)
        else:
            keys = ["st%d" % rng.randrange(3) for _ in range(rng.randrange(1, 3))]
            ids = [rng.choice(["0", "$", "%d-%d" % (ms, seq), "%d" % ms]) for _ in keys]
            cmd = ["XREAD"]
            if rng.randrange(3) == 0:
                cmd += ["COUNT", str(rng.randrange(-1, 4))]
            script.append(cmd + ["STREAMS"] + keys + ids)
    return script


SCRIPTS = {
    "base": [
        ("ping", [["PING"], ["ping"], ["PING", "hello"]]),
        ("echo", [["ECHO", "hey"], ["ECHO", ""], ["echo", "a b c"]]),
        ("set-get", [
            ["SET", "k", "v"], ["GET", "k"], ["GET", "missing"],
            ["SET", "k", "w"], ["GET", "k"],
            ["SET", "e", ""], ["GET", "e"],
        ]),
        ("set-expiry-options", [
            ["SET", "a", "1", "PX", "100000"], ["GET", "a"],
            ["SET", "b", "2", "EX", "100"], ["GET", "b"],
            ["SET", "c", "3", "px", "0"],
            ["SET", "d", "4", "PX", "-5"],
            ["SET", "f", "5", "PX", "abc"],
            ["SET", "g", "6", "PX"],
            ["SET", "h", "7", "BOGUS"],
        ]),
        ("set-options", [
            ["SET", "k", "1", "NX"], ["SET", "k", "2", "NX"], ["GET", "k"],
            ["SET", "k", "3", "XX"], ["SET", "m", "4", "XX"], ["GET", "m"],
            ["SET", "k", "5", "GET"], ["SET", "new", "6", "GET"],
            ["SET", "k", "7", "NX", "GET"], ["SET", "k", "8", "NX", "XX"],
            ["SET", "t", "1", "PX", "100000"], ["SET", "t", "2", "KEEPTTL"],
            ["GET", "t"], ["SET", "t", "3", "KEEPTTL", "PX", "10"],
            ["SET", "t", "4", "EX", "10", "PX", "100"],
            ["SET", "t", "5", "PX", "0100"], ["SET", "t", "6", "PX", "-0"],
            ["SET", "t", "7", "PX", "99999999999999"],
            ["SET", "t", "8", "EX", "9999999999999"],
            ["SET", "t", "9", "PX", "abc", "FOO"], ["SET", "t", "9", "PX", "0", "FOO"],
            ["SET", "t", "9", "PX", "abc"], ["SET", "t", "9", "PX", "0"],
        ]),
        ("set-absolute-expiry", [
            ["SET", "a", "1", "PXAT", "9999999999999"], ["GET", "a"],
            ["SET", "b", "2", "EXAT", "9999999999"], ["GET", "b"],
            ["SET", "c", "3", "PXAT", "1"], ["GET", "c"],
            ["SET", "d", "4", "EXAT", "1"], ["GET", "d"],
            ["SET", "e", "5", "PXAT", "0"], ["SET", "f", "6", "EXAT", "x"],
            ["SET", "g", "7", "PX", "10", "PXAT", "10"],
        ]),
        ("long-unknown-command", [["X" * 300], ["PING"]]),
        ("info-clients", [["INFO", "nosuchsection"]]),
        ("arity", [["GET"], ["SET", "k"], ["ECHO"], ["ECHO", "a", "b"],
                   ["PING", "a", "b"]]),
        ("unknown", [["NOSUCHCOMMAND"], ["NOSUCH", "a", "b"],
                     ["NOSUCH", "x" * 200, "y"], ["PING"]]),
    ],
    "optimistic-locking": [
        ("watch-own-writes", [
            ["SET", "k", "1"], ["WATCH", "k"], ["SET", "k", "2"], ["MULTI"], ["SET", "k", "3"], ["EXEC"],
            ["GET", "k"], ["WATCH", "k"], ["SET", "k", "2", "NX"], ["MULTI"], ["PING"], ["EXEC"],
            ["WATCH", "nol"], ["LPOP", "nol"], ["MULTI"], ["PING"], ["EXEC"],
            ["XADD", "s2", "5-1", "f", "v"], ["WATCH", "s2"], ["XADD", "s2", "1-1", "f", "v"],
            ["MULTI"], ["PING"], ["EXEC"],
            ["WATCH", "g"], ["GET", "g"], ["MULTI"], ["PING"], ["EXEC"],
            ["WATCH", "i"], ["INCR", "i"], ["MULTI"], ["PING"], ["EXEC"],
            ["RPUSH", "l", "a"], ["WATCH", "l"], ["LPOP", "l"], ["MULTI"], ["PING"], ["EXEC"],
        ]),
        ("watch-state", [
            ["MULTI"], ["WATCH", "k"], ["UNWATCH"], ["PING"], ["EXEC"],
            ["WATCH", "k"], ["MULTI"], ["EXEC"], ["SET", "k", "9"], ["MULTI"], ["PING"], ["EXEC"],
            ["WATCH", "k"], ["MULTI"], ["DISCARD"], ["SET", "k", "9"], ["MULTI"], ["PING"], ["EXEC"],
            ["WATCH", "k"], ["UNWATCH"], ["SET", "k", "8"], ["MULTI"], ["PING"], ["EXEC"],
            ["WATCH", "k"], ["SET", "k", "1"], ["MULTI"], ["NOSUCH"], ["EXEC"],
            ["MULTI"], ["PING"], ["EXEC"],
            ["WATCH"], ["UNWATCH", "x"], ["watch", "k", "k2"], ["unwatch"],
            ["WATCH", "a", "a", "b"], ["SET", "b", "1"], ["MULTI"], ["PING"], ["EXEC"],
        ]),
    ],
    "keys-config": [
        ("keys-patterns", [
            ["SET", "foo", "1"], ["SET", "fob", "1"], ["SET", "bar", "1"], ["SET", "a*b", "1"],
            ["SET", "x?y", "1"], ["SET", "[z]", "1"], ["RPUSH", "list", "a"],
            ["XADD", "stream", "1-1", "f", "v"], ["SET", "", "empty"],
            ["KEYS", "*"], ["KEYS", "f*"], ["KEYS", "fo?"], ["KEYS", "?o?"], ["KEYS", "[ab]*"],
            ["KEYS", "[a-c]*"], ["KEYS", "[^f]*"], ["KEYS", "a\\*b"], ["KEYS", "a*b"],
            ["KEYS", "x\\?y"], ["KEYS", "\\[z\\]"], ["KEYS", "*o*"], ["KEYS", "nothing"],
            ["KEYS", ""], ["KEYS", "[z"], ["KEYS", "*[b]"], ["KEYS", "[c-a]*"],
            ["KEYS", "[a-]"], ["KEYS", "**"], ["KEYS", "?**"], ["KEYS", "[fb"],
            ["SET", "_", "1"], ["KEYS", "[a-]"],
            ["SET", "\xff", "1"], ["KEYS", "[a-\xff]"], ["KEYS", "[\xff-a]"],
            ["KEYS"], ["KEYS", "a", "b"], ["keys", "f*"],
        ]),
        ("config", [
            ["CONFIG", "GET", "nosuch"], ["CONFIG"], ["CONFIG", "GET"],
            ["CONFIG", "NOSUCH"], ["config", "get", "NOSUCH"],
        ]),
    ],
    "transactions": [
        ("incr", [
            ["SET", "n", "41"], ["INCR", "n"], ["INCR", "new"], ["SET", "x", "abc"],
            ["INCR", "x"], ["SET", "sp", " 1"], ["INCR", "sp"], ["SET", "p", "+1"],
            ["INCR", "p"], ["SET", "z", "010"], ["INCR", "z"], ["SET", "neg", "-5"],
            ["INCR", "neg"], ["SET", "zero", "0"], ["INCR", "zero"], ["RPUSH", "l", "a"],
            ["INCR", "l"], ["INCR"], ["INCR", "a", "b"], ["SET", "t", "5", "PX", "100000"],
            ["INCR", "t"], ["GET", "t"], ["incr", "n"],
        ]),
        ("multi-exec", [
            ["EXEC"], ["DISCARD"], ["MULTI"], ["MULTI"], ["EXEC"],
            ["MULTI"], ["SET", "q", "1"], ["INCR", "q"], ["GET", "q"], ["EXEC"],
            ["MULTI"], ["SET", "w", "x"], ["INCR", "w"], ["SET", "v", "7"], ["EXEC"],
            ["MULTI"], ["DISCARD"], ["DISCARD"], ["multi"], ["exec"],
            ["MULTI"], ["PING"], ["ECHO", "hi"], ["XADD", "s1", "1-1", "f", "v"],
            ["XRANGE", "s1", "-", "+"], ["XREAD", "STREAMS", "s1", "0"],
            ["RPUSH", "l", "a", "b"], ["LRANGE", "l", "0", "-1"], ["LPOP", "l", "2"],
            ["TYPE", "s1"], ["EXEC"],
            ["MULTI"], ["BLPOP", "emptyl", "0"], ["XREAD", "BLOCK", "0", "STREAMS", "nos", "$"],
            ["EXEC"],
            ["MULTI"], ["XRANGE", "s1", "x", "+"], ["XREAD", "STREAMS", "s1"],
            ["XREAD", "COUNT", "x", "STREAMS", "s1", "0"], ["SET", "k", "v", "FOO"], ["EXEC"],
        ]),
        ("multi-errors", [
            ["MULTI"], ["SET", "q", "1"], ["NOSUCH", "a"], ["EXEC"], ["GET", "q"],
            ["MULTI"], ["GET"], ["EXEC"],
            ["MULTI"], ["XADD", "s", "3", "x"], ["EXEC"],
            ["MULTI"], ["EXEC", "x"], ["DISCARD", "y"], ["MULTI", "z"], ["DISCARD"],
            ["MULTI"], ["DISCARD", "y"], ["EXEC"],
            ["MULTI"], ["MULTI", "z"], ["EXEC"],
            ["EXEC", "x"], ["DISCARD", "y"], ["MULTI", "z"],
        ]),
    ],
    "streams": [
        ("xadd-ids", [
            ["XADD", "s", "1-1", "a", "b"], ["XADD", "s", "1-2", "c", "d"],
            ["XADD", "s", "2", "e", "f"], ["XADD", "s", "0-0", "x", "y"],
            ["XADD", "n", "0-0", "x", "y"], ["TYPE", "n"],
            ["XADD", "s", "1-1", "x", "y"], ["XADD", "s", "abc", "x", "y"],
            ["XADD", "s", "3-x", "x", "y"], ["XADD", "s", "-1", "x", "y"],
            ["XADD", "s", "3-", "x", "y"], ["XADD", "s", "3-4-5", "x", "y"],
            ["XADD", "s", "5-*", "x", "y"], ["XADD", "s", "5-*", "x", "y"],
            ["XADD", "s", "4-*", "x", "y"], ["XADD", "z", "0-*", "a", "b"],
            ["XADD", "s", "x" * 100, "a", "b"], ["XADD", "s", "3", "x"],
            ["XADD", "s"], ["XADD", "s", "9"], ["TYPE", "s"], ["TYPE", "z"],
        ]),
        ("xrange", [
            ["XADD", "s", "1-1", "a", "b"], ["XADD", "s", "1-2", "c", "d"],
            ["XADD", "s", "2-0", "e", "f", "g", "h"], ["XADD", "s", "5-1", "x", "y"],
            ["XRANGE", "s", "-", "+"], ["XRANGE", "s", "1", "1"],
            ["XRANGE", "s", "1-2", "5"], ["XRANGE", "s", "2", "1"],
            ["XRANGE", "s", "+", "-"], ["XRANGE", "s", "-", "-"],
            ["XRANGE", "s", "+", "+"], ["XRANGE", "s", "5-1", "5-1"],
            ["XRANGE", "s", "-", "+", "COUNT", "2"], ["XRANGE", "s", "-", "+", "count", "0"],
            ["XRANGE", "s", "-", "+", "COUNT", "-1"], ["XRANGE", "s", "-", "+", "COUNT", "x"],
            ["XRANGE", "s", "-", "+", "FOO", "1"], ["XRANGE", "s", "-", "+", "COUNT"],
            ["XRANGE", "s", "-"], ["XRANGE", "s", "x", "+"], ["XRANGE", "s", "1-1-1", "+"],
            ["XRANGE", "missing", "-", "+"],
        ]),
        ("xread", [
            ["XADD", "s", "1-1", "a", "b"], ["XADD", "s", "1-2", "c", "d"],
            ["XADD", "t", "3-0", "e", "f"],
            ["XREAD", "STREAMS", "s", "0"], ["XREAD", "streams", "s", "1-1"],
            ["XREAD", "STREAMS", "s", "1-2"], ["XREAD", "STREAMS", "s", "$"],
            ["XREAD", "COUNT", "1", "STREAMS", "s", "t", "0", "0"],
            ["XREAD", "COUNT", "0", "STREAMS", "s", "0"],
            ["XREAD", "COUNT", "-1", "STREAMS", "s", "0"],
            ["XREAD", "COUNT", "1", "COUNT", "2", "STREAMS", "s", "0"],
            ["XREAD", "STREAMS", "s", "missing", "0", "0"],
            ["XREAD", "STREAMS", "missing", "0"],
            ["XREAD", "STREAMS", "s", "s", "0", "1-1"],
            ["XREAD", "BLOCK", "10", "STREAMS", "s", "0"],
            ["XREAD", "BLOCK", "10", "STREAMS", "s", "$"],
            ["XREAD", "BLOCK", "-1", "STREAMS", "s", "0"],
            ["XREAD", "BLOCK", "x", "STREAMS", "s", "0"],
            ["XREAD", "BLOCK", "1.5", "STREAMS", "s", "0"],
            ["XREAD", "COUNT", "x", "STREAMS", "s", "0"],
            ["XREAD", "FOO", "STREAMS", "s", "0"], ["XREAD", "COUNT", "1", "s", "0"],
            ["XREAD", "STREAMS", "s", "t", "0"], ["XREAD", "STREAMS", "s", "x"],
            ["XREAD", "STREAMS", "s"], ["XREAD", "s", "0"], ["XREAD"],
        ]),
        ("random-streams", random_stream_script()),
        ("stream-errors", [
            ["SET", "str", "v"], ["XADD", "str", "1", "a", "b"],
            ["XRANGE", "str", "-", "+"], ["XREAD", "STREAMS", "str", "0"],
            ["XADD", "st", "1", "a", "b"], ["GET", "st"], ["RPUSH", "st", "x"],
            ["LLEN", "st"], ["TYPE", "str"], ["TYPE", "nope"], ["type", "st"],
            ["TYPE"], ["TYPE", "a", "b"],
        ]),
    ],
    "lists": [
        ("push-and-range", [
            ["RPUSH", "l", "a", "b", "c"], ["LPUSH", "l", "z", "y"],
            ["LRANGE", "l", "0", "-1"], ["LRANGE", "l", "1", "2"],
            ["LRANGE", "l", "-3", "-2"], ["LRANGE", "l", "-100", "100"],
            ["LRANGE", "l", "4", "2"], ["LRANGE", "l", "10", "20"],
            ["LRANGE", "missing", "0", "-1"], ["LLEN", "l"], ["LLEN", "missing"],
        ]),
        ("pop", [
            ["RPUSH", "l", "1", "2", "3", "4"], ["LPOP", "l"], ["LPOP", "l", "2"],
            ["LPOP", "l", "0"], ["LPOP", "l", "5"], ["LPOP", "l"], ["LLEN", "l"],
            ["LPOP", "missing"], ["LPOP", "missing", "2"],
        ]),
        ("both-ends", both_ends_script()),
        ("blpop-no-wait", [
            ["RPUSH", "b", "x", "y"], ["BLPOP", "missing", "b", "0"],
            ["BLPOP", "b", "0.01"], ["BLPOP", "b", "0.01"],
        ]),
        ("errors", [
            ["SET", "s", "v"], ["RPUSH", "s", "x"], ["LPUSH", "s", "x"],
            ["LLEN", "s"], ["LRANGE", "s", "0", "1"], ["LPOP", "s"],
            ["BLPOP", "s", "0"], ["RPUSH", "l", "x"], ["GET", "l"],
            ["LRANGE", "l", "a", "1"], ["LPOP", "l", "-1"], ["LPOP", "l", "x"],
            ["BLPOP", "l", "-1"], ["BLPOP", "l", "abc"], ["RPUSH", "l"],
            ["LLEN"], ["LRANGE", "l", "0"],
        ]),
    ],
    "sorted-sets": [
        ("zset-basics", [
            ["ZADD", "z", "100", "foo"], ["ZADD", "z", "100", "bar"], ["ZADD", "z", "20", "baz"],
            ["ZADD", "z", "30.5", "caz"], ["ZADD", "z", "40.25", "paz"], ["ZADD", "z", "100", "foo"],
            ["ZRANK", "z", "caz"], ["ZRANK", "z", "foo"], ["ZRANK", "z", "bar"], ["ZRANK", "z", "nosuch"],
            ["ZRANK", "nokey", "m"], ["ZRANGE", "z", "0", "-1"], ["ZRANGE", "z", "2", "4"], ["ZRANGE", "z", "-2", "-1"],
            ["ZRANGE", "z", "-10", "1"], ["ZRANGE", "z", "3", "1"], ["ZRANGE", "z", "5", "9"], ["ZRANGE", "nokey", "0", "1"],
            ["ZRANGE", "z", "0", "-1", "WITHSCORES"], ["ZRANGE", "z", "0", "-1", "withscores"],
            ["ZCARD", "z"], ["ZCARD", "nokey"], ["ZSCORE", "z", "caz"], ["ZSCORE", "z", "nosuch"], ["ZSCORE", "nokey", "m"],
            ["ZADD", "z", "1", "a", "2", "b", "1", "a"], ["ZADD", "z2", "3", "x", "2", "x"], ["ZSCORE", "z2", "x"],
            ["ZADD", "z", "5", "a"], ["ZRANK", "z", "a"], ["ZREM", "z", "a", "a", "nosuch"], ["ZREM", "z", "nosuch"],
            ["ZREM", "nokey", "m"], ["TYPE", "z"], ["ZREM", "z2", "x"], ["TYPE", "z2"], ["ZCARD", "z2"],
        ]),
        ("zset-scores", [
            ["ZADD", "s", "1e3", "a"], ["ZADD", "s", "-inf", "b"], ["ZADD", "s", "+inf", "c"], ["ZADD", "s", "Infinity", "d"],
            ["ZADD", "s", ".5", "e"], ["ZADD", "s", "1.", "f"], ["ZADD", "s", "-0.0", "g"], ["ZADD", "s", "00012.50", "h"],
            ["ZADD", "s", "1e400", "i"], ["ZADD", "s", "1e-400", "i"], ["ZADD", "s", "nan", "i"], ["ZADD", "s", "abc", "i"],
            ["ZADD", "s", " 1", "i"], ["ZADD", "s", "1 ", "i"], ["ZADD", "s", "1e", "i"], ["ZADD", "s", ".", "i"],
            ["ZADD", "s", "9", "new", "abc", "x"], ["ZSCORE", "s", "new"],
            ["ZRANGE", "s", "0", "-1", "WITHSCORES"], ["ZSCORE", "s", "a"], ["ZSCORE", "s", "b"], ["ZSCORE", "s", "c"],
        ]),
        ("zset-errors", [
            ["ZADD", "z", "1"], ["ZADD", "z", "1", "a", "2"], ["ZADD"], ["ZRANGE", "z", "a", "b"], ["ZRANGE", "z", "0"],
            ["ZRANGE", "z", "0", "1", "FOO"], ["ZREM", "z"], ["ZRANK", "z"], ["ZSCORE", "z"], ["ZCARD"], ["ZCARD", "a", "b"],
            ["SET", "str", "v"], ["ZADD", "str", "1", "a"], ["ZREM", "str", "a"], ["ZCARD", "str"], ["ZSCORE", "str", "a"],
            ["ZRANK", "str", "a"], ["ZRANGE", "str", "0", "1"], ["ZADD", "zz", "1", "a"], ["LPUSH", "zz", "x"],
            ["GET", "zz"], ["XADD", "zz", "*", "f", "v"],
        ]),
    ],
    "geospatial": [
        ("geo-basics", [
            ["GEOADD", "p", "11.5030378", "48.164271", "Munich"], ["GEOADD", "p", "2.2944692", "48.8584625", "Paris"],
            ["GEOADD", "p", "-0.127758", "51.507351", "London", "-0.0884948", "51.506479", "London2"],
            ["GEOADD", "p", "2.2944692", "48.8584625", "Paris"], ["ZSCORE", "p", "Paris"], ["ZSCORE", "p", "London"],
            ["ZRANGE", "p", "0", "-1"], ["GEODIST", "p", "Munich", "Paris"], ["GEODIST", "p", "Munich", "Paris", "km"],
            ["GEODIST", "p", "Munich", "Paris", "mi"], ["GEODIST", "p", "Munich", "Paris", "FT"], ["GEODIST", "p", "London", "London2"],
            ["GEODIST", "p", "Munich", "nosuch"], ["GEODIST", "nokey", "a", "b"], ["GEOPOS", "nokey", "a", "b"],
            ["GEOPOS", "p", "nosuch"],
            ["GEOSEARCH", "p", "FROMLONLAT", "2", "48", "BYRADIUS", "100000", "m"],
            ["GEOSEARCH", "p", "FROMLONLAT", "2", "48", "BYRADIUS", "500000", "m"],
            ["GEOSEARCH", "p", "FROMLONLAT", "11", "50", "BYRADIUS", "300000", "m"],
            ["GEOSEARCH", "p", "FROMLONLAT", "2", "48", "BYRADIUS", "5000", "km"],
            ["GEOSEARCH", "p", "FROMMEMBER", "Paris", "BYRADIUS", "1000", "km"],
            ["GEOSEARCH", "p", "FROMLONLAT", "0", "0", "BYRADIUS", "20000", "km"],
            ["GEOSEARCH", "p", "FROMLONLAT", "180", "85", "BYRADIUS", "1", "m"],
            ["GEOSEARCH", "nokey", "FROMLONLAT", "2", "48", "BYRADIUS", "1", "m"],
        ]),
        ("geo-errors", [
            ["GEOADD", "x", "200", "100", "foo"], ["GEOADD", "x", "10", "100", "foo"], ["GEOADD", "x", "-180.5", "0", "foo"],
            ["GEOADD", "x", "1", "2"], ["GEOADD", "x", "1", "2", "a", "3"], ["GEOADD", "x", "abc", "2", "a"],
            ["GEODIST", "x", "a"], ["GEODIST", "x", "a", "b", "yd"], ["GEODIST", "x", "a", "b", "m", "z"], ["GEOPOS"],
            ["GEOADD", "p", "1", "2", "m"],
            ["GEOSEARCH", "p", "FROMLONLAT", "2", "48", "BYRADIUS", "-1", "m"],
            ["GEOSEARCH", "p", "FROMLONLAT", "2", "48", "BYRADIUS", "abc", "m"],
            ["GEOSEARCH", "p", "FROMLONLAT", "2", "48", "BYRADIUS", "1", "yd"],
            ["GEOSEARCH", "p", "FROMLONLAT", "200", "48", "BYRADIUS", "1", "m"],
            ["GEOSEARCH", "p", "FROMMEMBER", "nosuch", "BYRADIUS", "1", "m"],
            ["SET", "str", "v"], ["GEOADD", "str", "1", "2", "a"], ["GEOPOS", "str", "a"], ["GEODIST", "str", "a", "b"],
            ["GEOSEARCH", "str", "FROMLONLAT", "1", "2", "BYRADIUS", "1", "m"],
            ["GEOSEARCH", "str", "FROMLONLAT", "999", "0", "BYRADIUS", "1", "m"],
            ["GEOSEARCH", "p", "FROMMEMBER", "nosuch", "BYRADIUS", "1", "xx"],
            ["GEOSEARCH", "nokey", "FROMMEMBER", "a", "BYRADIUS", "1", "m"],
            ["GEOSEARCH", "nokey", "FROMMEMBER", "a", "BYRADIUS", "1", "xx"],
            ["GEOSEARCH", "nokey", "FROMMEMBER", "a", "BYRADIUS", "-1", "m"],
            ["GEOSEARCH", "nokey", "FROMMEMBER", "a", "FOO"],
            ["GEOSEARCH", "p", "FROMLONLAT", "1", "2", "FROMMEMBER", "a", "BYRADIUS", "1", "m"],
            ["GEOSEARCH", "p", "FROMMEMBER", "a", "FROMLONLAT", "999", "2", "BYRADIUS", "1", "m"],
            ["GEOSEARCH", "p", "FROMLONLAT", "100", "0", "FROMLONLAT", "2", "48", "BYRADIUS", "1", "m", "BYRADIUS", "500", "km"],
            ["GEOSEARCH", "p", "BYRADIUS", "1", "m", "x", "y"], ["GEOSEARCH", "p", "FROMLONLAT", "1", "2", "x", "y", "z"],
            ["GEOADD", "x", "inf", "0", "z"], ["GEOADD", "x", "-0", "100", "z"],
        ]),
    ],
    "bitmaps": [
        ("bitmap-basics", [
            ["SETBIT", "b", "3", "1"], ["SETBIT", "b", "3", "1"], ["GETBIT", "b", "3"], ["GETBIT", "b", "4"], ["GETBIT", "nokey", "2"],
            ["SET", "a", "A"], ["GETBIT", "a", "1"], ["GETBIT", "a", "7"], ["GETBIT", "a", "0"], ["GETBIT", "a", "100"],
            ["SETBIT", "m", "1", "1"], ["GET", "m"], ["STRLEN", "m"], ["SETBIT", "m", "10", "1"], ["STRLEN", "m"], ["GET", "m"],
            ["BITCOUNT", "m"], ["BITCOUNT", "m", "0", "0"], ["BITCOUNT", "m", "1", "1"], ["BITCOUNT", "m", "0", "-1"],
            ["BITCOUNT", "m", "1", "10", "BIT"], ["BITCOUNT", "m", "-5", "-1", "bit"], ["BITCOUNT", "m", "2", "1"],
            ["BITCOUNT", "m", "-100", "100"], ["BITCOUNT", "m", "0", "1", "byte"], ["BITCOUNT", "nokey"],
            ["SETBIT", "k1", "0", "1"], ["SETBIT", "k1", "4", "1"], ["SETBIT", "k2", "0", "1"], ["SETBIT", "k2", "6", "1"],
            ["BITOP", "AND", "d", "k1", "k2"], ["GET", "d"], ["BITOP", "OR", "d", "k1", "k2"], ["GET", "d"],
            ["BITOP", "XOR", "d", "k1", "k2"], ["GET", "d"], ["BITOP", "NOT", "d", "k1"], ["GET", "d"],
            ["SETBIT", "k3", "1", "1"], ["SETBIT", "k3", "10", "1"], ["SETBIT", "k4", "1", "1"],
            ["BITOP", "AND", "d", "k3", "k4"], ["GET", "d"], ["BITOP", "OR", "d", "k3", "k4"], ["GET", "d"],
            ["BITOP", "AND", "d", "nokey", "nokey2"], ["TYPE", "d"], ["BITOP", "OR", "d", "k3", "nokey"], ["GET", "d"],
            ["SET", "n", "10"], ["SETBIT", "n", "15", "1"], ["GET", "n"], ["INCR", "n"],
            ["SET", "s", "hello"], ["SETBIT", "s", "0", "1"], ["SET", "s", "x", "GET"], ["STRLEN", "nokey"],
            ["SETBIT", "z", "0", "0"], ["GET", "z"], ["SETBIT", "big", "100000", "1"], ["STRLEN", "big"], ["BITCOUNT", "big"],
        ]),
        ("bitmap-errors", [
            ["SETBIT", "x", "-1", "1"], ["SETBIT", "x", "4294967296", "1"], ["SETBIT", "x", "1", "2"], ["SETBIT", "x", "1", "a"],
            ["GETBIT", "x", "-1"], ["GETBIT", "x", "abc"], ["SETBIT", "x", "1"], ["GETBIT", "x"], ["STRLEN"], ["BITCOUNT"],
            ["BITCOUNT", "x", "0"], ["BITCOUNT", "x", "a", "b"], ["BITCOUNT", "x", "0", "1", "FOO"], ["BITOP", "AND", "d"],
            ["BITOP", "FOO", "d", "k"], ["BITOP", "NOT", "d", "a", "b"], ["LPUSH", "l", "a"], ["SETBIT", "l", "1", "1"],
            ["GETBIT", "l", "1"], ["STRLEN", "l"], ["BITCOUNT", "l"], ["BITOP", "AND", "d", "l"], ["BITOP", "AND", "l", "nokey"],
            ["TYPE", "l"],
        ]),
    ],
    "authentication": [
        ("acl-commands", [
            ["ACL", "WHOAMI"], ["acl", "whoami"], ["ACL"], ["ACL", "WHOAMI", "x"], ["ACL", "GETUSER"], ["ACL", "SETUSER"],
            ["ACL", "FOO"], ["ACL", "GETUSER", "nobody"], ["ACL", "GETUSER", "Default"], ["AUTH", "x"], ["AUTH", "default", "x"],
            ["AUTH"], ["AUTH", "a", "b", "c"], ["AUTH", "default", "x", "y"], ["ACL", "SETUSER", "default", "bogus"],
            ["ACL", "SETUSER", "default", "<nothere"], ["ACL", "SETUSER", "default", ">a", "bogus"],
            ["ACL", "SETUSER", "default", ">a", "<b"], ["ACL", "SETUSER", "default", "NOPASS"], ["ACL", "SETUSER", "default", "on"],
            ["MULTI"], ["ACL", "FOO"], ["EXEC"], ["MULTI"], ["CONFIG", "FOO"], ["EXEC"], ["MULTI"], ["ACL", "WHOAMI"], ["AUTH", "default", "x"], ["EXEC"],
            ["ACL", "SETUSER", "default", ">mypassword", ">two", ">two"], ["AUTH", "default", "wrong"], ["AUTH", "wrong"],
            ["AUTH", "mypassword"], ["AUTH", "default", "two"], ["AUTH", "nobody", "two"], ["ACL", "SETUSER", "default", "<two"],
            ["AUTH", "default", "two"], ["ACL", "SETUSER", "default", "resetpass"], ["AUTH", "default", ""],
            ["ACL", "SETUSER", "default", ">", "off"], ["AUTH", "default", ""], ["ACL", "SETUSER", "default", "on"],
            ["AUTH", "default", ""], ["MULTI"], ["AUTH", "default", "bad"], ["ACL", "WHOAMI"], ["EXEC"], ["PING"],
        ]),
    ],
    "pub-sub": [
        ("subscribed-mode", [
            ["UNSUBSCRIBE"], ["UNSUBSCRIBE", "x"], ["PUBLISH", "ch", "m"], ["PUBLISH", "a"], ["SUBSCRIBE"],
            ["SUBSCRIBE", "a"], ["SUBSCRIBE", "b"], ["SUBSCRIBE", "a"], ["PING"], ["PING", "hi"],
            ["PING", "a", "b"], ["ECHO", "x"], ["GET"], ["NOSUCH"], ["set", "a", "b"], ["MULTI"],
            ["WATCH", "k"], ["PUBLISH", "a", "m"], ["INCR", "n"], ["UNSUBSCRIBE", "a"], ["UNSUBSCRIBE", "zz"],
            ["UNSUBSCRIBE", "b"], ["PING"], ["ECHO", "back"], ["PUBLISH", "a", "m"],
        ]),
    ],
}


def normalize(reply):
    if isinstance(reply, Error):
        return ("error", str(reply))
    if isinstance(reply, list):
        return [normalize(r) for r in reply]
    return reply


class RedisServer:
    def __init__(self):
        self.port = free_port()
        self.dir = tempfile.mkdtemp(prefix="loxpp-redis-oracle-")
        self.proc = subprocess.Popen(
            ["redis-server", "--port", str(self.port), "--bind", "127.0.0.1",
             "--save", "", "--appendonly", "no", "--dir", self.dir],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        deadline = time.time() + 5
        self.up = False
        while time.time() < deadline:
            try:
                socket.create_connection(
                    ("127.0.0.1", self.port), timeout=0.2).close()
                self.up = True
                break
            except OSError:
                time.sleep(0.05)

    def client(self):
        return Client(self.port)

    def stop(self):
        self.proc.terminate()
        self.proc.wait(timeout=5)
        shutil.rmtree(self.dir, ignore_errors=True)


def run_script(server, script):
    c = server.client()
    try:
        out = []
        for cmd in script:
            reply = normalize(c.call(*cmd))
            # KEYS gives no order.
            if cmd and cmd[0].upper() == "KEYS" and isinstance(reply, list):
                reply = sorted(reply)
            out.append(reply)
        return out
    finally:
        c.close()


def main():
    global PASS, FAIL
    prog = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else DEFAULT_PROG)
    if shutil.which("redis-server") is None:
        print("redis-server not found on PATH")
        return 1
    for group, scripts in SCRIPTS.items():
        print("== %s" % group)
        for name, script in scripts:
            oracle = RedisServer()
            ours = Server(prog, [])
            try:
                if not oracle.up or not ours.up:
                    FAIL += 1
                    print("  FAIL %s  a server did not start (redis=%s lox=%s)"
                          % (name, oracle.up, ours.up))
                    continue
                want = run_script(oracle, script)
                try:
                    got = run_script(ours, script)
                except (OSError, EOFError, ValueError) as e:
                    FAIL += 1
                    print("  FAIL %s  the Lox++ server failed: %r" % (name, e))
                    print("       server log:\n%s" % ours.log_text())
                    continue
                if want == got:
                    PASS += 1
                    print("  ok   %s" % name)
                else:
                    FAIL += 1
                    print("  FAIL %s" % name)
                    for cmd, w, g in zip(script, want, got):
                        if w != g:
                            print("       %r: redis %r, lox %r" % (cmd, w, g))
            finally:
                oracle.stop()
                ours.stop()
    rdb_checks(prog)
    replication_checks(prog)
    aof_checks(prog)
    pubsub_checks(prog)
    auth_checks(prog)
    print("\n%d passed, %d failed" % (PASS, FAIL))
    return 0 if FAIL == 0 else 1


RDB_CASES = [
    ("rdb-strings", [
        ["SET", "short", "bar"], ["SET", "i8", "12"], ["SET", "neg8", "-100"],
        ["SET", "i16", "30000"], ["SET", "neg16", "-30000"], ["SET", "i32", "70000"],
        ["SET", "neg32", "-2000000000"], ["SET", "big", "3000000000"],
        ["SET", "long", "abcabcabc" * 60], ["SET", "bin", "\x00\xff\r\n\x80"],
        ["SET", "empty", ""], ["SET", "a key", "a value"], ["SET", "007", "lead"],
        ["SET", "1e3", "x"],
    ], [["KEYS", "*"], ["GET", "short"], ["GET", "i8"], ["GET", "neg8"], ["GET", "i16"],
        ["GET", "neg16"], ["GET", "i32"], ["GET", "neg32"], ["GET", "big"], ["GET", "long"],
        ["GET", "bin"], ["GET", "empty"], ["GET", "a key"], ["GET", "007"], ["GET", "1e3"],
        ["KEYS", "i*"], ["CONFIG", "GET", "dbfilename"]]),
    ("rdb-expiry", [
        ["SET", "later", "v", "PX", "600000"], ["SET", "later-s", "v", "EX", "600"],
        ["SET", "soon", "v", "PX", "1500"], ["SET", "plain", "v"],
    ], [["KEYS", "*"], ["GET", "later"], ["GET", "later-s"], ["GET", "soon"], ["GET", "plain"]]),
]


def rdb_checks(prog):
    """Each case: a seed redis-server gets the commands and saves an RDB
    file; then redis-server and the Lox++ server load the same file from
    the same directory and get the same read commands. A key that expires
    while the file waits checks that the load skips an expired key."""
    global PASS, FAIL
    print("== rdb (one file, loaded by both servers)")
    for name, setup, reads in RDB_CASES:
        d = tempfile.mkdtemp(prefix="loxpp-rdb-")
        seed_port = free_port()
        seed = subprocess.Popen(
            ["redis-server", "--port", str(seed_port), "--bind", "127.0.0.1", "--save", "",
             "--appendonly", "no", "--dir", d, "--dbfilename", "seed.rdb"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        oracle = ours = None
        try:
            deadline = time.time() + 5
            while time.time() < deadline:
                try:
                    socket.create_connection(("127.0.0.1", seed_port), timeout=0.2).close()
                    break
                except OSError:
                    time.sleep(0.05)
            c = Client(seed_port)
            for cmd in setup:
                c.call(*[x.encode("latin-1").decode("unicode_escape") if "\\x" in x else x
                         for x in cmd])
            c.call("SAVE")
            c.close()
            seed.terminate()
            seed.wait(timeout=5)
            time.sleep(2)
            oracle_port = free_port()
            oracle = subprocess.Popen(
                ["redis-server", "--port", str(oracle_port), "--bind", "127.0.0.1",
                 "--save", "", "--appendonly", "no", "--dir", d, "--dbfilename", "seed.rdb"],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            ours = Server(prog, ["--dir", d, "--dbfilename", "seed.rdb"])
            deadline = time.time() + 5
            while time.time() < deadline:
                try:
                    socket.create_connection(("127.0.0.1", oracle_port), timeout=0.2).close()
                    break
                except OSError:
                    time.sleep(0.05)

            class Port:
                def __init__(self, port):
                    self.port = port

                def client(self):
                    return Client(self.port)
            want = run_script(Port(oracle_port), reads)
            got = run_script(ours, reads)
            if want == got:
                PASS += 1
                print("  ok   %s" % name)
            else:
                FAIL += 1
                print("  FAIL %s" % name)
                for cmd, w, g in zip(reads, want, got):
                    if w != g:
                        print("       %r: redis %r, lox %r" % (cmd, w, g))
        except (OSError, EOFError, ValueError) as e:
            FAIL += 1
            print("  FAIL %s  %r" % (name, e))
        finally:
            for proc in (seed, oracle):
                if proc is not None and proc.poll() is None:
                    proc.terminate()
                    proc.wait(timeout=5)
            if ours is not None:
                ours.stop()
            shutil.rmtree(d, ignore_errors=True)



REPL_WRITES = [
    ["SET", "a", "1"], ["SET", "b", "2", "PX", "600000"], ["INCR", "c"], ["INCR", "c"],
    ["RPUSH", "l", "x", "y", "z"], ["LPUSH", "l", "w"], ["LPOP", "l"], ["LPOP", "l", "2"],
    ["RPUSH", "q", "1"], ["BLPOP", "q", "0"], ["XADD", "s", "*", "f", "v"],
    ["XADD", "t", "1-1", "a", "b"], ["XADD", "t", "2-*", "c", "d"],
    ["MULTI"], ["SET", "m", "1"], ["INCR", "m"], ["EXEC"], ["SET", "big", "v" * 100000],
]
REPL_READS = [
    ["GET", "a"], ["GET", "b"], ["GET", "c"], ["LRANGE", "l", "0", "-1"], ["TYPE", "q"],
    ["XRANGE", "s", "-", "+"], ["XRANGE", "t", "-", "+"], ["GET", "m"], ["GET", "big"],
    ["KEYS", "*"],
]


def wait_port(port, seconds=5):
    deadline = time.time() + seconds
    while time.time() < deadline:
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
            return True
        except OSError:
            time.sleep(0.05)
    return False


class Port:
    def __init__(self, port):
        self.port = port

    def client(self):
        return Client(self.port)


def wait_synced(master_port, seconds=10):
    """Waits until the master has a replica, then until WAIT counts it."""
    c = Client(master_port)
    try:
        deadline = time.time() + seconds
        while time.time() < deadline:
            info = c.call("INFO", "replication")
            if "connected_slaves:1" in info:
                return True
            time.sleep(0.1)
        return False
    finally:
        c.close()


AOF_WRITES = [
    ["SET", "s", "v"], ["SET", "p", "v", "PX", "600000"], ["SET", "e", "v", "EX", "600"],
    ["SET", "soon", "v", "PX", "1000"], ["SET", "at", "v", "PXAT", "99999999999999"],
    ["SET", "s", "w", "KEEPTTL"], ["SET", "s", "x", "NX"], ["SET", "bad", "1", "FOO"],
    ["RPUSH", "l", "a", "b", "c"], ["LPUSH", "l", "z"], ["LPOP", "l"], ["LPOP", "l", "1"],
    ["INCR", "n"], ["INCR", "n"], ["INCR", "l"],
    ["XADD", "x", "1-1", "f", "v"], ["XADD", "x", "1-2", "g", "w", "h", "u"],
    ["SET", "d", "1"], ["DEL", "d", "missing"],
    ["MULTI"], ["SET", "m", "1"], ["INCR", "m"], ["RPUSH", "ml", "q"], ["EXEC"],
    ["MULTI"], ["SET", "m2", "1"], ["DISCARD"],
]
AOF_READS = [
    ["GET", "s"], ["GET", "p"], ["GET", "e"], ["GET", "soon"], ["GET", "at"], ["GET", "x"],
    ["GET", "bad"], ["LRANGE", "l", "0", "-1"], ["GET", "n"], ["XRANGE", "x", "-", "+"],
    ["GET", "d"], ["GET", "m"], ["LRANGE", "ml", "0", "-1"], ["GET", "m2"],
    ["TYPE", "s"], ["TYPE", "l"], ["TYPE", "x"], ["TYPE", "soon"],
    ["CONFIG", "GET", "appendonly"], ["CONFIG", "GET", "appenddirname"],
    ["CONFIG", "GET", "appendfilename"], ["CONFIG", "GET", "appendfsync"],
]


def start_redis_aof(d):
    port = free_port()
    proc = subprocess.Popen(
        ["redis-server", "--port", str(port), "--bind", "127.0.0.1", "--save", "",
         "--appendonly", "yes", "--appendfsync", "always", "--dir", d],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    wait_port(port)
    return proc, Port(port)


def start_lox_aof(prog, d):
    return Server(prog, ["--dir", d, "--appendonly", "yes", "--appendfsync", "always"])


def stop_proc(proc):
    if proc is not None and proc.poll() is None:
        proc.terminate()
        proc.wait(timeout=10)


def aof_checks(prog):
    """The same writes go to redis-server and to the Lox++ server, each with
    appendonly yes. Then each server starts again from the files, and the
    reads must agree: each from its own files, and each from the files of
    the other (a copy, as a start appends to them)."""
    global PASS, FAIL
    print("== aof (writes, a restart from the files, and the files of the other)")
    top = tempfile.mkdtemp(prefix="loxpp-aofdiff-")
    rdir, ldir = os.path.join(top, "redis"), os.path.join(top, "lox")
    os.mkdir(rdir)
    os.mkdir(ldir)
    procs = []
    try:
        rproc, rport = start_redis_aof(rdir)
        ours = start_lox_aof(prog, ldir)
        run_script(rport, AOF_WRITES)
        run_script(ours, AOF_WRITES)
        stop_proc(rproc)
        ours.stop()
        time.sleep(1.5)
        for a, b in (("redis", "lox-from-redis"), ("lox", "redis-from-lox")):
            shutil.copytree(os.path.join(top, a), os.path.join(top, b))
        cases = []
        for name, kind, sub in (("aof restart: redis from its files", "redis", "redis"),
                                ("aof restart: lox from its files", "lox", "lox"),
                                ("aof: lox from the files of redis", "lox", "lox-from-redis"),
                                ("aof: redis from the files of lox", "redis", "redis-from-lox")):
            d = os.path.join(top, sub)
            if kind == "redis":
                proc, srv = start_redis_aof(d)
                procs.append(proc)
            else:
                srv = start_lox_aof(prog, d)
                procs.append(srv.proc)
            try:
                cases.append((name, run_script(srv, AOF_READS)))
            except (OSError, EOFError, ValueError) as e:
                cases.append((name, repr(e)))
        want = cases[0][1]
        for name, got in cases:
            if got == want:
                PASS += 1
                print("  ok   %s" % name)
            else:
                FAIL += 1
                print("  FAIL %s" % name)
                if isinstance(got, list):
                    for cmd, w, g in zip(AOF_READS, want, got):
                        if w != g:
                            print("       %r: redis %r, this %r" % (cmd, w, g))
                else:
                    print("       %s" % got)
    finally:
        for proc in procs:
            stop_proc(proc)
        shutil.rmtree(top, ignore_errors=True)


def pubsub_checks(prog):
    """Two subscribers and a publisher on each server: the counts and the
    messages must agree."""
    global PASS, FAIL
    print("== pub-sub (two subscribers and a publisher)")
    oracle = RedisServer()
    ours = Server(prog, [])
    try:
        results = []
        for srv in (oracle, ours):
            s1, s2, p = srv.client(), srv.client(), srv.client()
            out = [s1.call("SUBSCRIBE", "c1"), s2.call("SUBSCRIBE", "c1"), s2.call("SUBSCRIBE", "c2"),
                   p.call("PUBLISH", "c1", "hello"), p.call("PUBLISH", "c2", "world"),
                   s1.read(), s2.read(), s2.read(),
                   s2.call("UNSUBSCRIBE", "c1"), p.call("PUBLISH", "c1", "again"), s1.read(),
                   p.call("PUBLISH", "c2", "")]
            out.append(s2.read())
            for c in (s1, s2, p):
                c.close()
            results.append(out)
        if results[0] == results[1]:
            PASS += 1
            print("  ok   pub-sub delivery")
        else:
            FAIL += 1
            print("  FAIL pub-sub delivery\n       redis %r\n       lox   %r" % tuple(results))
    except (OSError, EOFError, ValueError) as e:
        FAIL += 1
        print("  FAIL pub-sub delivery  %r" % e)
    finally:
        oracle.stop()
        ours.stop()


def auth_checks(prog):
    """Clients that are and are not authenticated, on each server: the
    replies must agree."""
    global PASS, FAIL
    print("== authentication (several clients)")
    oracle = RedisServer()
    ours = Server(prog, [])
    try:
        results = []
        for srv in (oracle, ours):
            a, s = srv.client(), srv.client()
            out = [s.call("SUBSCRIBE", "ch"), a.call("ACL", "SETUSER", "default", ">mypassword")]
            b = srv.client()
            for cmd in (["PING"], ["ACL", "WHOAMI"], ["FOO"], ["GET"], ["GET", "k"], ["ACL", "FOO"], ["ACL", "GETUSER"],
                        ["CONFIG", "FOO"], ["MULTI"], ["SET", "k", "v"], ["EXEC"], ["DISCARD"], ["SUBSCRIBE", "x"],
                        ["AUTH", "x"], ["PING"], ["AUTH", "mypassword"], ["ACL", "WHOAMI"], ["SET", "k", "v"],
                        ["AUTH", "default", "bad"], ["GET", "k"]):
                out.append(b.call(*cmd))
            out += [s.call("PING"), s.call("AUTH", "default", "mypassword"), a.call("PING"),
                    a.call("ACL", "SETUSER", "default", "off")]
            c = srv.client()
            out += [c.call("PING"), c.call("AUTH", "default", "mypassword"), a.call("ACL", "WHOAMI"),
                    a.call("ACL", "SETUSER", "default", "nopass")]
            e = srv.client()
            out += [e.call("PING"), e.call("AUTH", "default", "any"), a.call("ACL", "SETUSER", "default", "on")]
            f = srv.client()
            out += [f.call("PING"), c.call("AUTH", "default", "mypassword"), c.call("PING")]
            d = srv.client()
            d.send_raw(encode("PING") + encode("AUTH", "default", "mypassword") + encode("PING"))
            out += [d.read(), d.read(), d.read()]
            for x in (a, s, b, c, d, e, f):
                x.close()
            results.append([normalize(r) for r in out])
        if results[0] == results[1]:
            PASS += 1
            print("  ok   authentication with several clients")
        else:
            FAIL += 1
            print("  FAIL authentication with several clients")
            for i, (w, g) in enumerate(zip(*results)):
                if w != g:
                    print("       step %d: redis %r, lox %r" % (i, w, g))
    except (OSError, EOFError, ValueError) as e:
        FAIL += 1
        print("  FAIL authentication with several clients  %r" % e)
    finally:
        oracle.stop()
        ours.stop()


def replication_checks(prog):
    """A Lox++ master with a redis-server replica, and a redis-server master
    with a Lox++ replica: the same writes on the master, then the same reads
    on both servers must agree."""
    global PASS, FAIL
    print("== replication (one master, one replica, Lox++ and redis-server)")
    for name in ("lox master, redis replica", "redis master, lox replica"):
        d = tempfile.mkdtemp(prefix="loxpp-repl-")
        procs, servers = [], []
        try:
            if name.startswith("lox master"):
                master = Server(prog, [])
                servers.append(master)
                mport = master.port
                rport = free_port()
                procs.append(subprocess.Popen(
                    ["redis-server", "--port", str(rport), "--bind", "127.0.0.1", "--save", "",
                     "--dir", d, "--replicaof", "127.0.0.1", str(mport)],
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
                wait_port(rport)
            else:
                mport = free_port()
                procs.append(subprocess.Popen(
                    ["redis-server", "--port", str(mport), "--bind", "127.0.0.1", "--save", "", "--dir", d],
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
                wait_port(mport)
                replica = Server(prog, ["--replicaof", "127.0.0.1 %d" % mport])
                servers.append(replica)
                rport = replica.port
            if not wait_synced(mport):
                FAIL += 1
                print("  FAIL %s  the replica did not connect" % name)
                continue
            c = Client(mport)
            for cmd in REPL_WRITES:
                c.call(*cmd)
            acked = c.call("WAIT", "1", "5000")
            c.close()
            want = run_script(Port(mport), REPL_READS)
            got = run_script(Port(rport), REPL_READS)
            if acked == 1 and want == got:
                PASS += 1
                print("  ok   %s" % name)
            else:
                FAIL += 1
                print("  FAIL %s  WAIT gave %r" % (name, acked))
                for cmd, w, g in zip(REPL_READS, want, got):
                    if w != g:
                        print("       %r: master %r, replica %r" % (cmd, str(w)[:80], str(g)[:80]))
        except (OSError, EOFError, ValueError) as e:
            FAIL += 1
            print("  FAIL %s  %r" % (name, e))
        finally:
            for p in procs:
                if p.poll() is None:
                    # SIGKILL: a redis-server master with a replica waits up
                    # to 10 s for it on SIGTERM.
                    p.kill()
                    p.wait(timeout=5)
            for sv in servers:
                sv.stop()
            shutil.rmtree(d, ignore_errors=True)

if __name__ == "__main__":
    sys.exit(main())
