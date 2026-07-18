#!/bin/sh
# CI entrypoint (T0.6): clean build + full test run under strict flags for
# portable sources. Exits non-zero on any build or test failure.
set -e

cd "$(dirname "$0")/../.."
make clean
make test
