add_executable(kano_git_tui
    main.cpp
)

set_target_properties(kano_git_tui PROPERTIES OUTPUT_NAME kano-git-tui)

target_link_libraries(kano_git_tui PRIVATE kano::git_core kano::git_command KanoInfra::unattended)

# KOG-BUG-0107 r16: r15'"'"'s --whole-archive brought in BOTH the
# dynamic libstdc++_dll import library AND the static libstdc++.a
# contents, causing "multiple definition" errors.  Switch to
# -static-libstdc++ which forces the static archive and excludes
# the DLL import library.  The duplicate-definition problem goes
# away, and the missing-symbol problem is solved in the same link.
if(MINGW)
  target_link_options(kano_git_tui PRIVATE -static-libstdc++)
endif()