#!/usr/bin/env python3
# shards.py [--matrix]
#
# The stage groups of run_stages.py, split into CI shards. Each shard runs its
# groups with REDIS_STAGE_GROUPS. With no argument, this checks that the shards
# cover every group exactly once, and exits 1 when a group is missing, repeated,
# or unknown. With --matrix, it prints the shards as the JSON of a CI matrix.
#
# The split balances the local wall time of each group (about 115 to 125 s a
# shard, against 473 s for one run of all groups).
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import run_stages  # noqa: E402

SHARDS = [
    ["pub-sub", "transactions", "lists", "base"],
    ["authentication", "sorted-sets", "optimistic-locking", "replication", "query limit"],
    ["persistence-rdb", "streams", "persistence-aof"],
    ["geospatial", "fairness", "bitmaps"],
]


def main():
    groups = [name for name, _ in run_stages.GROUPS]
    listed = [g for shard in SHARDS for g in shard]
    missing = sorted(set(groups) - set(listed))
    unknown = sorted(set(listed) - set(groups))
    repeated = sorted({g for g in listed if listed.count(g) > 1})
    if missing or unknown or repeated:
        print("shards.py: missing %r, unknown %r, repeated %r" % (missing, unknown, repeated))
        return 1
    if "--matrix" in sys.argv[1:]:
        print(json.dumps({"include": [
            {"name": "shard %d" % (i + 1), "groups": ",".join(shard)}
            for i, shard in enumerate(SHARDS)]}))
        return 0
    print("shards.py: %d groups in %d shards, each once" % (len(groups), len(SHARDS)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
