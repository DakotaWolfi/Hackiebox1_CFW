# Writes cfw_build.h with the build identity (version.h) at every
# build: the commit time, or the current time for a tree with uncommitted
# changes, as in the CC3200 Makefile. The file changes only when the values do.
# SPDX-License-Identifier: GPL-3.0-or-later
execute_process(COMMAND git status --porcelain --untracked-files=no WORKING_DIRECTORY ${TOP}
                OUTPUT_VARIABLE DIRTY OUTPUT_STRIP_TRAILING_WHITESPACE)
if(DIRTY)
    string(TIMESTAMP BUILD_TIME "%s")
else()
    execute_process(COMMAND git log -1 --format=%ct WORKING_DIRECTORY ${TOP}
                    OUTPUT_VARIABLE BUILD_TIME OUTPUT_STRIP_TRAILING_WHITESPACE)
endif()
execute_process(COMMAND git rev-parse --short=7 HEAD WORKING_DIRECTORY ${TOP}
                OUTPUT_VARIABLE REVISION OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND date -u -d @${BUILD_TIME} "+%d %b %H:%M"
                OUTPUT_VARIABLE BUILD_DATE OUTPUT_STRIP_TRAILING_WHITESPACE)
file(WRITE ${OUT}.tmp "#define CFW_BUILD_TIME ${BUILD_TIME}u\n#define CFW_REVISION \"${REVISION}\"\n#define CFW_BUILD_DATE \"${BUILD_DATE}\"\n")
execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different ${OUT}.tmp ${OUT})
