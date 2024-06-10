#!/usr/bin/env bash

fetch() {
  local remote=$1
  local ref=$2

  git fetch --quiet --depth=1 $remote $ref 2>/dev/null
}

tracker_target=

mkdir subprojects
cd subprojects

git clone https://gitlab.gnome.org/GNOME/tinysparql.git

if [ $? -ne 0 ]; then
  echo Checkout failed
  exit 1
fi

cd tinysparql

if [ "$CI_MERGE_REQUEST_TARGET_BRANCH_NAME" ]; then
  merge_request_remote=${CI_MERGE_REQUEST_SOURCE_PROJECT_URL//localsearch/tinysparql}
  # Fall back to old name, for existing user repositories
  merge_request_remote=${merge_request_remote//tracker-miners/tinysparql}
  merge_request_branch=$CI_MERGE_REQUEST_SOURCE_BRANCH_NAME

  echo Looking for $merge_request_branch on user repository $merge_request_remote...
  if git fetch -q $merge_request_remote $merge_request_branch 2>/dev/null; then
    tracker_target=FETCH_HEAD
  else
    tracker_target=origin/$CI_MERGE_REQUEST_TARGET_BRANCH_NAME
    echo Using $tracker_target instead
  fi
fi

if [ -z "$tracker_target" ]; then
  ref_remote=${CI_PROJECT_URL//localsearch/tinysparql}
  echo -n Looking for $CI_COMMIT_REF_NAME on upstream repository...
  if fetch $ref_remote $CI_COMMIT_REF_NAME; then
    echo \ found
    tracker_target=FETCH_HEAD
  else
    echo \ not found
    tracker_target=$(git branch -r -l origin/$CI_COMMIT_REF_NAME)
    tracker_target=${tracker_target:-origin/main}
    echo Using $tracker_target instead
  fi
fi

git checkout -q $tracker_target
