#!/usr/bin/env bash
# Sync upstream gamescope into the current branch.
#
# Auto-resolves exactly one class of conflict: a file upstream modified that we
# deleted on purpose in the cleanout. Everything else is left conflicted for a
# human, on purpose.
#
# Merge, never rebase: rebasing replays every commit onto a moving base and
# re-raises the cleanout's deletion conflicts each time.

set -euo pipefail

UPSTREAM_REMOTE="${UPSTREAM_REMOTE:-upstream}"
UPSTREAM_BRANCH="${UPSTREAM_BRANCH:-master}"

cd "$(git rev-parse --show-toplevel)"

if ! git remote get-url "$UPSTREAM_REMOTE" >/dev/null 2>&1; then
	echo "No '$UPSTREAM_REMOTE' remote. Add it with:" >&2
	echo "  git remote add $UPSTREAM_REMOTE https://github.com/ValveSoftware/gamescope.git" >&2
	exit 1
fi

if [ -n "$(git status --porcelain)" ]; then
	echo "Working tree is dirty. Commit or stash first." >&2
	exit 1
fi

# rerere is what stops us re-resolving the same deletions every sync.
git config rerere.enabled true
git config rerere.autoupdate true

echo "==> Fetching $UPSTREAM_REMOTE/$UPSTREAM_BRANCH"
git fetch "$UPSTREAM_REMOTE" "$UPSTREAM_BRANCH"

BASE="$(git merge-base HEAD "$UPSTREAM_REMOTE/$UPSTREAM_BRANCH")"
COUNT="$(git rev-list --count "$BASE..$UPSTREAM_REMOTE/$UPSTREAM_BRANCH")"

if [ "$COUNT" -eq 0 ]; then
	echo "Already up to date with $UPSTREAM_REMOTE/$UPSTREAM_BRANCH."
	exit 0
fi

echo "==> $COUNT upstream commit(s) to merge:"
git log --oneline "$BASE..$UPSTREAM_REMOTE/$UPSTREAM_BRANCH"
echo

echo "==> Merging"
if git merge --no-commit --no-ff "$UPSTREAM_REMOTE/$UPSTREAM_BRANCH"; then
	echo "==> Merged with no conflicts."
else
	echo
	echo "==> Resolving deletions we made on purpose"
	# DU = deleted by us, modified by them. That is the cleanout's signature.
	resolved=0
	while IFS= read -r file; do
		[ -z "$file" ] && continue
		echo "    stays deleted: $file"
		echo "      upstream touched it in:"
		git log --oneline "$BASE..$UPSTREAM_REMOTE/$UPSTREAM_BRANCH" -- "$file" | sed 's/^/        /'
		echo "      we removed it in:"
		git log --oneline --diff-filter=D --max-count=1 -- "$file" | sed 's/^/        /'
		git rm -q --ignore-unmatch "$file"
		resolved=$((resolved + 1))
	done < <(git status --porcelain | awk '/^DU /{print $2}')

	if [ "$resolved" -gt 0 ]; then
		echo "    ($resolved file(s) kept deleted -- read the upstream commits above:"
		echo "     the file is gone, but the bug it fixed may exist in code we kept.)"
	fi
fi

echo
remaining="$(git diff --name-only --diff-filter=U || true)"
if [ -n "$remaining" ]; then
	echo "==> Conflicts needing a human:"
	echo "$remaining" | sed 's/^/    /'
	echo
	echo "Resolve, then build and test before committing."
	exit 1
fi

cat <<'EOF'
==> No conflicts left. Nothing is committed yet -- verify first:

    export PKG_CONFIG_PATH=/usr/local/lib/x86_64-linux-gnu/pkgconfig
    ninja -C build-werror/
    meson test -C build-werror/ --suite gamescope
    git commit

EOF
