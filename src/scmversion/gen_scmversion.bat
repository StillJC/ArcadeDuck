@echo off
REM SPDX-FileCopyrightText: 2026 StillJC
REM SPDX-License-Identifier: GPL-3.0-only
REM Modified for ArcadeDuck by StillJC, 2026.

setlocal

SET VERSIONFILE="scmversion.cpp"

FOR /F "tokens=3" %%g IN ('findstr /B /C:"#define ARCADEDUCK_SEMANTIC_VERSION " "%~dp0arcadeduck_version.h"') do (SET "VERSION=%%~g")
FOR /F "tokens=3" %%g IN ('findstr /B /C:"#define ARCADEDUCK_GPL_BASELINE_HASH " "%~dp0arcadeduck_version.h"') do (SET "BASELINE=%%~g")

IF NOT DEFINED VERSION (
  ECHO ERROR: Could not read ARCADEDUCK_SEMANTIC_VERSION.
  EXIT /B 1
)

IF NOT DEFINED BASELINE (
  ECHO ERROR: Could not read ARCADEDUCK_GPL_BASELINE_HASH.
  EXIT /B 1
)

FOR /F "tokens=* USEBACKQ" %%g IN (`git rev-parse HEAD`) do (SET "HASH=%%g")
SET "SHORT_HASH=%HASH:~0,9%"

FOR /F "tokens=* USEBACKQ" %%g IN (`git rev-parse --abbrev-ref HEAD`) do (SET "BRANCH=%%g")
FOR /F "tokens=* USEBACKQ" %%g IN (`git describe --tags --always`) do (SET "TAG=%%g")
FOR /F "tokens=* USEBACKQ" %%g IN (`git log -1 --date=iso8601-strict "--format=%%cd"`) do (SET "CDATE=%%g")
IF DEFINED ARCADEDUCK_CI_BUILD (
  SET "BUILD=%ARCADEDUCK_CI_BUILD%"
) ELSE (
  REM The public ArcadeDuck repository may not contain the historical GPL baseline commit object.
  REM If the baseline exists, keep using the original baseline-relative count.
  REM Otherwise, use the current repository commit count for local/non-CI builds.
  git cat-file -e %BASELINE%^{commit} 2>NUL
  IF ERRORLEVEL 1 (
    ECHO INFO: GPL baseline commit is not present in this repository; using repository commit count.
    FOR /F "tokens=* USEBACKQ" %%g IN (`git rev-list --count HEAD`) do (SET "BUILD=%%g")
  ) ELSE (
    FOR /F "tokens=* USEBACKQ" %%g IN (`git rev-list --count %BASELINE%..HEAD`) do (SET "BUILD=%%g")
  )
)

IF NOT DEFINED HASH (
  ECHO ERROR: Could not determine Git revision.
  EXIT /B 1
)

IF NOT DEFINED BUILD (
  ECHO ERROR: Could not determine ArcadeDuck build number.
  EXIT /B 1
)

SET "DIRTY=false"
SET "DIRTY_SUFFIX="
IF NOT DEFINED ARCADEDUCK_CI_CLEAN (
  git diff-index --quiet HEAD --
  IF ERRORLEVEL 1 (
    SET "DIRTY=true"
    SET "DIRTY_SUFFIX=-dirty"
  )
)

SET SIGNATURELINE=// %HASH% %BRANCH% %TAG% %CDATE% %DIRTY% %VERSION% %BUILD%

IF NOT EXIST %VERSIONFILE% GOTO :write_version

findstr /X /L /C:"%SIGNATURELINE%" %VERSIONFILE% >NUL
IF ERRORLEVEL 1 GOTO :write_version

ECHO Signature matches, skipping writing %VERSIONFILE%
EXIT /B 0

:write_version

ECHO Updating %VERSIONFILE% for %HASH%...
(ECHO %SIGNATURELINE%
ECHO const char* g_scm_hash_str = "%HASH%";
ECHO const char* g_scm_branch_str = "%BRANCH%";
ECHO const char* g_scm_tag_str = "%TAG%";
ECHO const char* g_scm_date_str = "%CDATE%";
ECHO const char* g_scm_build_str = "%BUILD%";
ECHO const char* g_scm_version_id_str = "%VERSION%-build%BUILD%-g%SHORT_HASH%%DIRTY_SUFFIX%";
ECHO const char* g_scm_version_str = "ArcadeDuck v%VERSION% (Build %BUILD%, g%SHORT_HASH%%DIRTY_SUFFIX%)";
)>%VERSIONFILE%

EXIT /B 0