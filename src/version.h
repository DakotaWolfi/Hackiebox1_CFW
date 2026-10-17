/*
 * Identity values, set by the Makefile.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef VERSION_H
#define VERSION_H

#ifndef CFW_BUILD_TIME
#error "CFW_BUILD_TIME must come from the build"
#endif

#define CFW_VERSION_STRING "CFW_" CFW_VERSION /* long version text, <= 15 characters */
#define CFW_BRANCH_STRING CFW_VERSION "_CFW"  /* short version text, <= 15 characters */

#endif
