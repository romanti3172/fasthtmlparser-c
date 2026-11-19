# The C benchmark (fasthtmlparser vs lexbor and Gumbo) on Linux, as the fhp-bench-<lang> images of the other ports.
# Build from the repository root, run one image at a time (nothing else benchmarking on the machine):
#   docker build -f bench/Dockerfile -t fhp-bench-c .
#   docker run --rm --cpuset-cpus 8-15 fhp-bench-c
# The tables (gcc, then clang) are printed; the image also checks first that every library extracts the same records.
FROM debian:trixie-slim

RUN apt-get update \
 && apt-get install -y --no-install-recommends build-essential clang cmake ninja-build git ca-certificates \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /work
COPY . .

# lexbor and Gumbo are fetched by CMake (FetchContent) at configure time
RUN cmake -S . -B /build/lib -G Ninja -DCMAKE_BUILD_TYPE=Release \
 && cmake --build /build/lib && ctest --test-dir /build/lib --output-on-failure \
 && cmake -S bench -B /build/gcc -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc \
 && cmake --build /build/gcc \
 && cmake -S bench -B /build/clang -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang \
 && cmake --build /build/clang \
 && /build/gcc/fhp_bench --check && /build/clang/fhp_bench --check

CMD ["sh", "-c", "uname -srm; /build/gcc/fhp_bench && echo && /build/clang/fhp_bench"]
