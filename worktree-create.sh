#!/bin/sh
set -e

worktree_name="$1"

git worktree add "../burst_wts/$worktree_name"
cp -a .env .claude/ "../burst_wts/$worktree_name"

