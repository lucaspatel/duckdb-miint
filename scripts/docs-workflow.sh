#!/usr/bin/env bash
# Standardized end-to-end docs workflow:
#   1. (re)build the extension if needed
#   2. regenerate the docs site (introspect + reference + doctests)
#   3. run the test suite (including auto-generated doctests)
#   4. format-check
#
# Use after any change to a RegisterDocumented* call to verify the docs
# build, the doctest registry, and the SQL test suite all agree.
#
# Pass --no-build to skip step 1 if you've just built (e.g. iterating on
# JS/markdown without C++ changes).

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

BUILD=true
for arg in "$@"; do
  case "$arg" in
    --no-build) BUILD=false ;;
    -h|--help)
      sed -n '2,/^$/p' "$0" | sed 's/^# \?//'
      exit 0
      ;;
    *) echo "Unknown argument: $arg" >&2; exit 1 ;;
  esac
done

if [ "$BUILD" = true ]; then
  echo "==> Building extension"
  bash build.sh
fi

echo "==> Regenerating site (introspect + reference + doctests)"
( cd site && npm run prebuild )

echo "==> Building site"
( cd site && npm run build )

echo "==> Running test suite (sqllogictest + Catch2 + auto-generated doctests)"
bash run_tests.sh

echo "==> Format check"
make format-check

echo "==> All clean"
