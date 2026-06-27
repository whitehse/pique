#!/bin/bash
set -e

echo "Building libpqwire fuzzers..."

mkdir -p corpus

echo "Building basic fuzzer..."
clang -g -O1 -fsanitize=fuzzer,address,undefined \
    -I../include \
    ../src/pqwire.c \
    ../src/scram.c \
    fuzz_pqwire.c \
    -o fuzz_pqwire

echo "Building MITM fuzzer..."
clang -g -O1 -fsanitize=fuzzer,address,undefined \
    -I../include \
    ../src/pqwire.c \
    ../src/scram.c \
    ../src/mitm.c \
    fuzz_mitm.c \
    -o fuzz_mitm

echo ""
echo "Fuzzers built successfully:"
echo "  ./fuzz_pqwire corpus/"
echo "  ./fuzz_mitm corpus/"
echo ""
echo "The MITM fuzzer uses adversarial mutation between client and server."