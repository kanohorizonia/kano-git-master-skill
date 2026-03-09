add_executable(kano_git_tui
    main.cpp
)

set_target_properties(kano_git_tui PROPERTIES OUTPUT_NAME kano-git-tui)

target_link_libraries(kano_git_tui PRIVATE kano::git_core kano::git_command)
