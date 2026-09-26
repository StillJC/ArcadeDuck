#!/bin/sh
# SPDX-FileCopyrightText: 2026 StillJC
# SPDX-License-Identifier: GPL-3.0-only
# Modified for ArcadeDuck by StillJC, 2026.


VERSION_FILE="scmversion.cpp"
CURDIR=$(pwd)

if [ "$(uname -s)" = "Darwin" ]; then
  cd "$(dirname "$(python3 -c 'import os,sys;print(os.path.realpath(sys.argv[1]))' "$0")")"
else
  cd "$(dirname "$(readlink -f "$0")")"
fi

VERSION=$(sed -n 's/^#define ARCADEDUCK_SEMANTIC_VERSION "\(.*\)"/\1/p' arcadeduck_version.h | tr -d '\r\n')
BASELINE=$(sed -n 's/^#define ARCADEDUCK_GPL_BASELINE_HASH "\(.*\)"/\1/p' arcadeduck_version.h | tr -d '\r\n')

HASH=$(git rev-parse HEAD)
SHORT_HASH=$(git rev-parse --short=9 HEAD | tr -d '\r\n')
BRANCH=$(git rev-parse --abbrev-ref HEAD | tr -d '\r\n')
TAG=$(git describe --tags --always | tr -d '\r\n')
DATE=$(git log -1 --date=iso8601-strict --format=%cd)
if [ -n "${ARCADEDUCK_CI_BUILD:-}" ]; then
  BUILD="${ARCADEDUCK_CI_BUILD}"
else
  BUILD=$(git rev-list --count "${BASELINE}..HEAD" 2>/dev/null | tr -d '\r\n')
fi

if [ -z "$VERSION" ]; then
  echo "ERROR: Could not read ARCADEDUCK_SEMANTIC_VERSION."
  exit 1
fi

if [ -z "$BASELINE" ]; then
  echo "ERROR: Could not read ARCADEDUCK_GPL_BASELINE_HASH."
  exit 1
fi

if [ -z "$BUILD" ]; then
  echo "ERROR: Could not determine ArcadeDuck build number."
  exit 1
fi

DIRTY=false
DIRTY_SUFFIX=""
if [ -z "${ARCADEDUCK_CI_CLEAN:-}" ] && [ -n "$(git status --porcelain --untracked-files=no)" ]; then
  DIRTY=true
  DIRTY_SUFFIX="-dirty"
fi

cd "$CURDIR"

SIGNATURE_LINE="// ${HASH} ${BRANCH} ${TAG} ${DATE} ${DIRTY} ${VERSION} ${BUILD}"

if [ -f "$VERSION_FILE" ]; then
  EXISTING_LINE=$(head -n1 "$VERSION_FILE" | tr -d '\r\n')
  if [ "$EXISTING_LINE" = "$SIGNATURE_LINE" ]; then
    echo "Signature matches, skipping writing ${VERSION_FILE}"
    exit 0
  fi
fi

echo "Writing ${VERSION_FILE} for ${HASH}..."

cat > "$VERSION_FILE" << EOF_SCM
${SIGNATURE_LINE}
const char* g_scm_hash_str = "${HASH}";
const char* g_scm_branch_str = "${BRANCH}";
const char* g_scm_tag_str = "${TAG}";
const char* g_scm_date_str = "${DATE}";
const char* g_scm_build_str = "${BUILD}";
const char* g_scm_version_id_str = "${VERSION}-build${BUILD}-g${SHORT_HASH}${DIRTY_SUFFIX}";
const char* g_scm_version_str = "ArcadeDuck v${VERSION} (Build ${BUILD}, g${SHORT_HASH}${DIRTY_SUFFIX})";
EOF_SCM