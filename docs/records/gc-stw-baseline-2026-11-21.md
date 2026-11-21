# Baseline of the stop-the-world collector on the large-heap programs

This record is the baseline for the collector mission (#1277). Node N3 moves
the current collector behind the collector interface as `stw`, and it must
give the same counts as this record. Nothing in this record changes after it
merges.

## What was measured

The four programs of `benchmarks/gc_large_heap/`, run with the native VM and
`LOXPP_GC_TRACE` (trace format 3), three times each:

```
cmake --preset release && cmake --build build
LOXPP_GC_TRACE=<file> build/loxpp benchmarks/gc_large_heap/<program>.lox
python3 tools/gc_report.py <file>
```

Code: `main` at 9deb416 plus the version 3 trace header (the N2 pull
request). The collector is the stop-the-world mark-sweep of that code.

Host: Linux 6.18.44, 4 cores, clang 18.1.3. It is a virtual machine. A tight
loop on this host sees stalls longer than 1 ms about every 0.3 s, so the times
below are a rough guide and only the counts are exact.

## Counts (exact, the same in all 3 runs)

N3 must give the same numbers for the same programs. A collection is one row.
The columns are the cause, the objects before and after the collection, the
bytes before and after.

### live_then_churn

```
n  cause      objs_before  objs_after   bytes_before   bytes_after
1  threshold         5287        5287        1048725       1048725
2  threshold        10408       10408        2097493       2097493
3  threshold        20650       20650        4195029       4195029
4  threshold        41134       41134        8390101       8390101
5  threshold        82102       82102       16780245      16780245
6  threshold       164038      164038       33560533      33560533
7  threshold       327910      327910       67121109      67121109
8  threshold       655654      655654      134242261     134242261
9  threshold      1311142     1311142      268484565     268484565
10 threshold      2622118     2622118      536969173     536969173
11 threshold      5418833     3000291     1073938453     609578389
12 threshold      6175179     3000291     1219156885     609578389
13 exit           3406861           0      687639693          5120
```

### list_write

```
n  cause      objs_before  objs_after   bytes_before   bytes_after
1  threshold         5286        5286        1048669       1048669
2  threshold        10407       10407        2097437       2097437
3  threshold        20649       20649        4194973       4194973
4  threshold        41133       41133        8390045       8390045
5  threshold        82101       82101       16780189      16780189
6  threshold       164037      164037       33560477      33560477
7  threshold       327909      327909       67121053      67121053
8  threshold       655653      655653      134242205     134242205
9  threshold      1311141     1311141      268484509     268484509
10 threshold      2622117     2622117      536969117     536969117
11 threshold      5418832     3000290     1073938397     609578333
12 exit           4581748           0      913218133          5120
```

### big_map

```
n  cause      objs_before  objs_after   bytes_before   bytes_after
1  threshold        12428        6360        1551365       1211557
2  threshold        24716       15538        3091461       2577493
3  threshold        49292       32415        6171653       5226541
4  threshold        98444       65429       12332037      10483197
5  threshold       196748      131089       24652805      20975901
6  threshold       393356      262222       49294341      41950837
7  threshold       786572      524397       98577413      83895613
8  threshold      1573004     1048701      197143557     167782589
9  threshold      3336192     2384601      335565213     282276117
10 threshold      3573157     3573157      648890901     648890901
11 exit           7000293           0     1256569229      67109888
```

### long_chain

```
n  cause      objs_before  objs_after   bytes_before   bytes_after
1  threshold         5629        5629        1048725       1048725
2  threshold        11092       11092        2097621       2097621
3  threshold        22018       22018        4195413       4195413
4  threshold        43870       43870        8390997       8390997
5  threshold        87574       87574       16782165      16782165
6  threshold       174982      174982       33564501      33564501
7  threshold       349798      349798       67129173      67129173
8  threshold       699430      699430      134258517     134258517
9  threshold      1398694     1398694      268517205     268517205
10 threshold      2797222     2797222      537034581     537034581
11 threshold      5594278     3000292     1074069333     576024021
12 exit           5406306           0     1037978573          5120
```

## Times (indicative, the median of 3 runs)

Pause is the pause of one collection. MMU is the share of a window in which
the program runs.

| program | collections | wall ms | gc ms | pause p50 ms | pause max ms | MMU 10 ms | MMU 100 ms | MMU 1000 ms |
|---|---|---|---|---|---|---|---|---|
| live_then_churn | 13 | 3421 | 1203 | 15.5 | 405 | 0% | 0% | 30% |
| list_write | 12 | 3668 | 1042 | 8.5 | 446 | 0% | 0% | 55% |
| big_map | 11 | 6093 | 810 | 9.3 | 287 | 0% | 0% | 70% |
| long_chain | 12 | 2729 | 675 | 5.3 | 306 | 0% | 0% | 69% |

Each program stops the program for hundreds of milliseconds in one pause, and
MMU(10 ms) is 0%. This is problem #1165.
