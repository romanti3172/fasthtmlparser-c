AMD Ryzen 7 3800X (Windows 10 VM, and Linux on the same machine),  lexbor 3.0.1, Gumbo 0.14.1, `cmake -S bench -B bench/build -G Ninja -DCMAKE_BUILD_TYPE=Release` (2026-11-19).

"borrowed": `fhp_parse_borrowed` (no copy of the page); the other fasthtmlparser rows use `fhp_parse` (copy + UTF-8 check). "lexbor (document reused)": one `lxb_html_document_t` parsed again and again, lexbor at its best.

## MSVC 19.52 (Visual Studio 2026 Build Tools), /O2

| task | library | time | throughput |
|---|---|---:|---:|
| parse quotes 11 KB | fasthtmlparser | 15.4 µs | 736 MB/s |
| parse quotes 11 KB | fasthtmlparser (borrowed) | 14.7 µs | 774 MB/s |
| parse quotes 11 KB | lexbor | 122.0 µs | 93 MB/s |
| parse quotes 11 KB | lexbor (document reused) | 93.9 µs | 121 MB/s |
| parse quotes 11 KB | gumbo | 652.5 µs | 17 MB/s |
| parse laptops 38 KB | fasthtmlparser | 46.3 µs | 834 MB/s |
| parse laptops 38 KB | fasthtmlparser (borrowed) | 44.4 µs | 871 MB/s |
| parse laptops 38 KB | lexbor | 363.5 µs | 106 MB/s |
| parse laptops 38 KB | lexbor (document reused) | 301.6 µs | 128 MB/s |
| parse laptops 38 KB | gumbo | 1956.2 µs | 20 MB/s |
| parse books 51 KB | fasthtmlparser | 49.7 µs | 1078 MB/s |
| parse books 51 KB | fasthtmlparser (borrowed) | 48.4 µs | 1106 MB/s |
| parse books 51 KB | lexbor | 354.2 µs | 151 MB/s |
| parse books 51 KB | lexbor (document reused) | 314.4 µs | 170 MB/s |
| parse books 51 KB | gumbo | 2696.3 µs | 20 MB/s |
| parse books x10 510 KB | fasthtmlparser | 553.9 µs | 966 MB/s |
| parse books x10 510 KB | fasthtmlparser (borrowed) | 464.3 µs | 1153 MB/s |
| parse books x10 510 KB | lexbor | 4580.7 µs | 117 MB/s |
| parse books x10 510 KB | lexbor (document reused) | 4070.1 µs | 132 MB/s |
| parse books x10 510 KB | gumbo | 28160.7 µs | 19 MB/s |
| parse books, no </li> </p> | fasthtmlparser | 50.3 µs | 1052 MB/s |
| parse books, no </li> </p> | fasthtmlparser (borrowed) | 50.4 µs | 1051 MB/s |
| parse books, no </li> </p> | lexbor | 305.1 µs | 173 MB/s |
| parse books, no </li> </p> | lexbor (document reused) | 295.3 µs | 179 MB/s |
| parse books, no </li> </p> | gumbo | 2726.3 µs | 19 MB/s |
| scrape 20 books | fasthtmlparser find | 70.0 µs |  |
| scrape 20 books | fasthtmlparser CSS | 70.7 µs |  |
| scrape 20 books | fasthtmlparser XPath | 104.6 µs |  |
| scrape 20 books | lexbor CSS | 355.5 µs |  |
| scrape 20 books | gumbo (tree walk) | 2722.0 µs |  |
| scrape 10 quotes | fasthtmlparser find | 25.3 µs |  |
| scrape 10 quotes | fasthtmlparser CSS | 23.6 µs |  |
| scrape 10 quotes | lexbor CSS | 123.3 µs |  |
| scrape 10 quotes | gumbo (tree walk) | 686.1 µs |  |

## gcc 16.2.0 (MinGW-w64 UCRT, winlibs), -O3

| task | library | time | throughput |
|---|---|---:|---:|
| parse quotes 11 KB | fasthtmlparser | 17.7 µs | 640 MB/s |
| parse quotes 11 KB | fasthtmlparser (borrowed) | 16.3 µs | 695 MB/s |
| parse quotes 11 KB | lexbor | 94.2 µs | 120 MB/s |
| parse quotes 11 KB | lexbor (document reused) | 83.9 µs | 135 MB/s |
| parse quotes 11 KB | gumbo | 665.3 µs | 17 MB/s |
| parse laptops 38 KB | fasthtmlparser | 48.7 µs | 793 MB/s |
| parse laptops 38 KB | fasthtmlparser (borrowed) | 46.2 µs | 837 MB/s |
| parse laptops 38 KB | lexbor | 288.4 µs | 134 MB/s |
| parse laptops 38 KB | lexbor (document reused) | 278.9 µs | 139 MB/s |
| parse laptops 38 KB | gumbo | 1978.8 µs | 20 MB/s |
| parse books 51 KB | fasthtmlparser | 55.5 µs | 965 MB/s |
| parse books 51 KB | fasthtmlparser (borrowed) | 52.6 µs | 1017 MB/s |
| parse books 51 KB | lexbor | 320.5 µs | 167 MB/s |
| parse books 51 KB | lexbor (document reused) | 297.8 µs | 180 MB/s |
| parse books 51 KB | gumbo | 2545.4 µs | 21 MB/s |
| parse books x10 510 KB | fasthtmlparser | 549.0 µs | 975 MB/s |
| parse books x10 510 KB | fasthtmlparser (borrowed) | 514.2 µs | 1041 MB/s |
| parse books x10 510 KB | lexbor | 4026.3 µs | 133 MB/s |
| parse books x10 510 KB | lexbor (document reused) | 3458.7 µs | 155 MB/s |
| parse books x10 510 KB | gumbo | 25598.7 µs | 21 MB/s |
| parse books, no </li> </p> | fasthtmlparser | 53.8 µs | 984 MB/s |
| parse books, no </li> </p> | fasthtmlparser (borrowed) | 51.0 µs | 1037 MB/s |
| parse books, no </li> </p> | lexbor | 291.3 µs | 182 MB/s |
| parse books, no </li> </p> | lexbor (document reused) | 280.8 µs | 188 MB/s |
| parse books, no </li> </p> | gumbo | 2569.5 µs | 21 MB/s |
| scrape 20 books | fasthtmlparser find | 71.4 µs |  |
| scrape 20 books | fasthtmlparser CSS | 74.5 µs |  |
| scrape 20 books | fasthtmlparser XPath | 144.4 µs |  |
| scrape 20 books | lexbor CSS | 341.2 µs |  |
| scrape 20 books | gumbo (tree walk) | 2721.0 µs |  |
| scrape 10 quotes | fasthtmlparser find | 27.1 µs |  |
| scrape 10 quotes | fasthtmlparser CSS | 25.8 µs |  |
| scrape 10 quotes | lexbor CSS | 107.5 µs |  |
| scrape 10 quotes | gumbo (tree walk) | 667.8 µs |  |

## Linux (Ubuntu 26.04 host, kernel 7.0, Docker image `fhp-bench-c` from bench/Dockerfile, `--cpuset-cpus 8-15`), gcc 14.2.0, -O3

| task | library | time | throughput |
|---|---|---:|---:|
| parse quotes 11 KB | fasthtmlparser | 12.1 µs | 937 MB/s |
| parse quotes 11 KB | fasthtmlparser (borrowed) | 11.1 µs | 1026 MB/s |
| parse quotes 11 KB | lexbor | 114.5 µs | 99 MB/s |
| parse quotes 11 KB | lexbor (document reused) | 78.2 µs | 145 MB/s |
| parse quotes 11 KB | gumbo | 437.2 µs | 26 MB/s |
| parse laptops 38 KB | fasthtmlparser | 34.9 µs | 1108 MB/s |
| parse laptops 38 KB | fasthtmlparser (borrowed) | 33.7 µs | 1148 MB/s |
| parse laptops 38 KB | lexbor | 493.3 µs | 78 MB/s |
| parse laptops 38 KB | lexbor (document reused) | 260.6 µs | 148 MB/s |
| parse laptops 38 KB | gumbo | 1342.9 µs | 29 MB/s |
| parse books 51 KB | fasthtmlparser | 43.5 µs | 1232 MB/s |
| parse books 51 KB | fasthtmlparser (borrowed) | 40.6 µs | 1320 MB/s |
| parse books 51 KB | lexbor | 484.2 µs | 111 MB/s |
| parse books 51 KB | lexbor (document reused) | 281.6 µs | 190 MB/s |
| parse books 51 KB | gumbo | 2084.7 µs | 26 MB/s |
| parse books x10 510 KB | fasthtmlparser | 453.1 µs | 1182 MB/s |
| parse books x10 510 KB | fasthtmlparser (borrowed) | 378.7 µs | 1414 MB/s |
| parse books x10 510 KB | lexbor | 4153.0 µs | 129 MB/s |
| parse books x10 510 KB | lexbor (document reused) | 4497.2 µs | 119 MB/s |
| parse books x10 510 KB | gumbo | 21998.9 µs | 24 MB/s |
| parse books, no </li> </p> | fasthtmlparser | 40.6 µs | 1302 MB/s |
| parse books, no </li> </p> | fasthtmlparser (borrowed) | 37.2 µs | 1422 MB/s |
| parse books, no </li> </p> | lexbor | 249.1 µs | 212 MB/s |
| parse books, no </li> </p> | lexbor (document reused) | 242.2 µs | 219 MB/s |
| parse books, no </li> </p> | gumbo | 1971.5 µs | 27 MB/s |
| scrape 20 books | fasthtmlparser find | 54.1 µs |  |
| scrape 20 books | fasthtmlparser CSS | 56.1 µs |  |
| scrape 20 books | fasthtmlparser XPath | 81.2 µs |  |
| scrape 20 books | lexbor CSS | 292.1 µs |  |
| scrape 20 books | gumbo (tree walk) | 2019.6 µs |  |
| scrape 10 quotes | fasthtmlparser find | 18.9 µs |  |
| scrape 10 quotes | fasthtmlparser CSS | 18.6 µs |  |
| scrape 10 quotes | lexbor CSS | 89.4 µs |  |
| scrape 10 quotes | gumbo (tree walk) | 439.9 µs |  |

## Linux, clang 19.1.7, -O3

| task | library | time | throughput |
|---|---|---:|---:|
| parse quotes 11 KB | fasthtmlparser | 12.0 µs | 946 MB/s |
| parse quotes 11 KB | fasthtmlparser (borrowed) | 10.8 µs | 1046 MB/s |
| parse quotes 11 KB | lexbor | 107.3 µs | 106 MB/s |
| parse quotes 11 KB | lexbor (document reused) | 71.2 µs | 159 MB/s |
| parse quotes 11 KB | gumbo | 409.1 µs | 28 MB/s |
| parse laptops 38 KB | fasthtmlparser | 37.9 µs | 1020 MB/s |
| parse laptops 38 KB | fasthtmlparser (borrowed) | 33.3 µs | 1161 MB/s |
| parse laptops 38 KB | lexbor | 455.3 µs | 85 MB/s |
| parse laptops 38 KB | lexbor (document reused) | 248.5 µs | 156 MB/s |
| parse laptops 38 KB | gumbo | 1239.7 µs | 31 MB/s |
| parse books 51 KB | fasthtmlparser | 41.7 µs | 1283 MB/s |
| parse books 51 KB | fasthtmlparser (borrowed) | 38.7 µs | 1383 MB/s |
| parse books 51 KB | lexbor | 428.0 µs | 125 MB/s |
| parse books 51 KB | lexbor (document reused) | 259.3 µs | 206 MB/s |
| parse books 51 KB | gumbo | 1968.6 µs | 27 MB/s |
| parse books x10 510 KB | fasthtmlparser | 433.7 µs | 1234 MB/s |
| parse books x10 510 KB | fasthtmlparser (borrowed) | 394.6 µs | 1357 MB/s |
| parse books x10 510 KB | lexbor | 4023.4 µs | 133 MB/s |
| parse books x10 510 KB | lexbor (document reused) | 3892.8 µs | 138 MB/s |
| parse books x10 510 KB | gumbo | 21283.4 µs | 25 MB/s |
| parse books, no </li> </p> | fasthtmlparser | 41.7 µs | 1270 MB/s |
| parse books, no </li> </p> | fasthtmlparser (borrowed) | 38.7 µs | 1366 MB/s |
| parse books, no </li> </p> | lexbor | 232.0 µs | 228 MB/s |
| parse books, no </li> </p> | lexbor (document reused) | 226.9 µs | 233 MB/s |
| parse books, no </li> </p> | gumbo | 1891.6 µs | 28 MB/s |
| scrape 20 books | fasthtmlparser find | 55.8 µs |  |
| scrape 20 books | fasthtmlparser CSS | 58.6 µs |  |
| scrape 20 books | fasthtmlparser XPath | 82.8 µs |  |
| scrape 20 books | lexbor CSS | 276.4 µs |  |
| scrape 20 books | gumbo (tree walk) | 2036.4 µs |  |
| scrape 10 quotes | fasthtmlparser find | 19.4 µs |  |
| scrape 10 quotes | fasthtmlparser CSS | 20.0 µs |  |
| scrape 10 quotes | lexbor CSS | 97.0 µs |  |
| scrape 10 quotes | gumbo (tree walk) | 412.9 µs |  |

On Linux the server also runs production services, so single rows can be noisy (lexbor on books 51 KB: 484 µs, but 249 µs on the same page without `</li>` `</p>`).
