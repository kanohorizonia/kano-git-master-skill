add_executable(kog-cli
    main.cpp
)

# Optional: Output name as kano-git instead of kog-cli if desired,
# but the user requested src/cpp/kog-cli structure. We'll stick to kog-cli.
set_target_properties(kog-cli PROPERTIES OUTPUT_NAME kano-git)

target_include_directories(kog-cli PRIVATE ../kano-git-core/src/modules)
target_link_libraries(kog-cli PRIVATE kano::git_core)
