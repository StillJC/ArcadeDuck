// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#define ARCADEDUCK_PRODUCT_NAME "ArcadeDuck"

// Versioning policy:
// - Increment MINOR for each completed project or hardware-system milestone.
// - Increment PATCH for fixes made to an existing milestone.
// - Keep the -dev suffix while the next milestone remains on the development branch.
#define ARCADEDUCK_SEMANTIC_VERSION "0.5.1-dev"

#define ARCADEDUCK_WINDOWS_RESOURCE_VERSION 0,5,1,0
#define ARCADEDUCK_WINDOWS_RESOURCE_VERSION_DOTTED "0.5.1.0"
#define ARCADEDUCK_WINDOWS_RESOURCE_VERSION_DISPLAY "v" ARCADEDUCK_SEMANTIC_VERSION

// Retained for source provenance and licensing records, not the primary display version.
#define ARCADEDUCK_GPL_BASELINE_HASH "25bc8a64803df7e702db66e0f11d7b7d0fdc99f2"
