add_executable(kano_git_cli
    main.cpp
)

set_target_properties(kano_git_cli PROPERTIES OUTPUT_NAME kano-git)

target_link_libraries(kano_git_cli PRIVATE kano::git_core kano::git_command kano::git_build_info)
