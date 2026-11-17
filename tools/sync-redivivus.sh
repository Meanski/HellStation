#!/usr/bin/env bash
# Bring upstream Redivivus changes into HellStation's snapshot import.
#   tools/sync-redivivus.sh [ref]          (default: upstream/main)
# Applies the upstream diff from the revision recorded in UPSTREAM to <ref>
# (minus the paths UPSTREAM lists as removed) as a three-way patch, and stages
# it with UPSTREAM updated. Build and smoke-test, then commit with
#   HS_UPSTREAM_SYNC=1 git commit -m "sync: Redivivus <short sha>"
# (the variable lets the pre-commit hook accept changes to generated code).
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
[ -z "$(git status --porcelain --untracked-files=no)" ] || { echo "sync: commit or stash local changes first" >&2; exit 1; }
git remote get-url upstream >/dev/null 2>&1 || git remote add upstream "$(awk '$1=="repository"{print $2}' UPSTREAM)"
git fetch -q upstream
old=$(awk '$1=="revision"{print $2}' UPSTREAM)
new=$(git rev-parse "${1:-upstream/main}^{commit}")
[ "$old" = "$new" ] && { echo "sync: already at $new"; exit 0; }
excludes=()
while read -r p; do excludes+=(":(exclude)$p"); done < <(awk '$1=="removed"{print $2}' UPSTREAM)
rc=0
git diff --binary "$old" "$new" -- . "${excludes[@]}" | git apply --3way --index || rc=$?
sed -i.bak "s/^revision .*/revision $new/" UPSTREAM && rm -f UPSTREAM.bak
git add UPSTREAM
echo "sync: staged Redivivus ${old:0:12}..${new:0:12} ($(git rev-list --count "$old..$new") upstream commits)"
[ $rc -eq 0 ] || echo "sync: some hunks conflicted; resolve the markers and git add those files" >&2
echo "sync: build and smoke-test, then: HS_UPSTREAM_SYNC=1 git commit -m \"sync: Redivivus ${new:0:12}\""
