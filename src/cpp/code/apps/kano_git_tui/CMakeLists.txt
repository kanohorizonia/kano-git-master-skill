add_executable(kano_git_tui
    main.cpp
)

set_target_properties(kano_git_tui PROPERTIES OUTPUT_NAME kano-git-tui)

target_link_libraries(kano_git_tui PRIVATE kano::git_core kano::git_command KanoInfra::unattended)

# KOG-BUG-0107 r13: same libstdc++ link fix as kano_git_cli (the
# production-path TUI entrypoint also uses std::println transitively
# through commit_utils, which on MinGW GCC 15 needs libstdc++ to be
# in the link line so std::__open_terminal / std::__write_to_terminal
# are found).
if(MINGW)
  target_link_libraries(kano_git_tui PRIVATE stdc++)
endif()
