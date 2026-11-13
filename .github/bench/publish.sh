#!/usr/bin/env bash
# slz - Copyright (c) 2026 Seregon. SPDX-License-Identifier: MIT
#
# Publishes a benchmark result to the bench-data branch (.github/workflows/bench.yml).
#
#   publish.sh <result.json> <dir> [--dry-run]
#
# bench-data is an orphan branch of generated files only (history.json, the README banners, the
# shields.io badges and a dashboard), so main gets no bot commits. This checks it out as a worktree
# in <dir> (or starts it when it does not exist yet), lets report.py add the result and re-render,
# commits as Seregon and pushes. When another run pushed first, it refetches and re-renders (5 tries).
# --dry-run renders <result.json> as the newest run and commits locally, but never pushes: every
# benchmark run does that, so the publishing path is exercised outside main too.
set -euo pipefail
result=$(realpath "$1")
site=$(realpath -m "$2")
dry=${3:-}
here=$(dirname "$(realpath "$0")")
branch=bench-data
ref=refs/remotes/origin/$branch

fetch() { git fetch -q --depth=1 --no-tags origin "+refs/heads/$branch:$ref"; }

rc=0
git ls-remote --exit-code --heads origin "refs/heads/$branch" > /dev/null || rc=$?
case $rc in
  0) fetch
     git worktree add -q -f -B "$branch" "$site" "$ref" ;;
  2) echo "$branch does not exist yet: starting it"
     git worktree add -q -f --detach "$site"
     git -C "$site" checkout -q --orphan "$branch"
     git -C "$site" rm -rfq --ignore-unmatch . ;;
  *) echo "git ls-remote origin failed ($rc)" >&2
     exit 1 ;;
esac

read -r sha subject < <(python3 -c 'import json, sys
r = json.load(open(sys.argv[1]))
print(r["commit"][:7], (r.get("subject") or "").strip())' "$result")
run_url=$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1])).get("run_url") or "")' "$result")

for try in 1 2 3 4 5; do
  python3 "$here/report.py" publish "$result" --site "$site" ${dry:+--preview}
  git -C "$site" add -A
  if git -C "$site" diff --cached --quiet; then
    echo "$branch: nothing to commit"
    exit 0
  fi
  git -C "$site" -c user.name=Seregon -c user.email=109359355+seregonwar@users.noreply.github.com \
    commit -q -m "bench: $sha $subject" ${run_url:+-m "$run_url"}
  git -C "$site" --no-pager log -1 --stat --format='%h %an <%ae>%n%s%n%b'
  if [ -n "$dry" ]; then
    echo "dry run: not pushed"
    exit 0
  fi
  if git -C "$site" push -q origin "HEAD:refs/heads/$branch"; then
    echo "pushed to $branch (try $try)"
    exit 0
  fi
  sleep $((5 + RANDOM % 15))
  fetch
  git -C "$site" reset -q --hard "$ref"
  git -C "$site" clean -q -fdx
done
echo "::error::could not push to $branch after 5 tries"
exit 1
