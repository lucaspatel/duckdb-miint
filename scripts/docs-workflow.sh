#!/usr/bin/env bash
# Standardized end-to-end docs workflow:
#   1. (re)build the extension if needed
#   2. regenerate the docs site (introspect + reference + doctests)
#   3. build the site to dist/
#   4. run only the auto-generated doctests (fast)
#   5. format-check
#
# Use after any change to a RegisterDocumented* call to verify the docs
# build, the doctest registry, and the new doctests pass.
#
# Flags (combine as needed):
#   --no-build     skip step 1 (the extension binary already matches your edits)
#   --full-tests   run the entire test suite instead of doctests-only
#   --no-tests     skip the test step entirely
#   --no-format    skip format-check

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

BUILD=true
TESTS=doctests   # doctests | full | none
FORMAT=true
for arg in "$@"; do
  case "$arg" in
    --no-build)   BUILD=false ;;
    --full-tests) TESTS=full ;;
    --no-tests)   TESTS=none ;;
    --no-format)  FORMAT=false ;;
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

case "$TESTS" in
  doctests)
    if [ -d test/sql/doctests ] && ls test/sql/doctests/*.test >/dev/null 2>&1; then
      echo "==> Running doctests only ($(ls test/sql/doctests/*.test | wc -l) files)"
      ./build/release/test/unittest 'test/sql/doctests/*'
    else
      echo "==> No doctests to run; skipping"
    fi
    ;;
  full)
    echo "==> Running full test suite"
    bash run_tests.sh
    ;;
  none)
    echo "==> Skipping tests (--no-tests)"
    ;;
esac

if [ "$FORMAT" = true ]; then
  echo "==> Format check"
  make format-check
else
  echo "==> Skipping format-check (--no-format)"
fi

echo "==> All clean"
