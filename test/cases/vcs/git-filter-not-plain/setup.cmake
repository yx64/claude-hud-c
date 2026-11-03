# A repository whose own config names a filter driver with an = in its name, which -c cannot name back:
# git must not run at all. The driver itself succeeds, so a git that ran would show its status.
set(ENV{GIT_AUTHOR_NAME} t)
set(ENV{GIT_AUTHOR_EMAIL} t@t)
set(ENV{GIT_COMMITTER_NAME} t)
set(ENV{GIT_COMMITTER_EMAIL} t@t)
set(ENV{GIT_AUTHOR_DATE} "2026-10-10T10:00:00Z")
set(ENV{GIT_COMMITTER_DATE} "2026-10-10T10:00:00Z")
function(git)
  execute_process(COMMAND git ${ARGN} WORKING_DIRECTORY ${HUD_DIR}/proj OUTPUT_QUIET ERROR_VARIABLE err RESULT_VARIABLE code
                  TIMEOUT ${HUD_RUN_TIMEOUT})
  if(NOT code EQUAL 0)
    message(FATAL_ERROR "git ${ARGN}: ${code} ${err}")
  endif()
endfunction()
git(init -q -b main)
file(WRITE ${HUD_DIR}/proj/a.txt "one\n")
git(add .)
git(commit -q -m init)
# The name goes through a file, as a repository would carry it.
hud_copy_subst(${CMAKE_CURRENT_LIST_DIR}/gitattributes ${HUD_DIR}/proj/.gitattributes)
hud_copy_subst(${CMAKE_CURRENT_LIST_DIR}/filter.cfg ${HUD_DIR}/proj/.git/hud-filter.cfg)
git(config include.path hud-filter.cfg)
