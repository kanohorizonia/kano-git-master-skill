add_library(kano_git_test_core STATIC)

target_sources(kano_git_test_core
  PRIVATE
    generators/public/tui_state_generator.hpp
    generators/public/command_string_generator.hpp
    support/public/functional_test_support.hpp

    generators/private/tui_state_generator.cpp
    generators/private/command_string_generator.cpp
    support/private/functional_test_support.cpp
)

add_library(kano::git_test_core ALIAS kano_git_test_core)

target_compile_features(kano_git_test_core PUBLIC cxx_std_23)
if(KOG_ENABLE_MODULES)
  set_target_properties(kano_git_test_core PROPERTIES CXX_SCAN_FOR_MODULES ON)
else()
  set_target_properties(kano_git_test_core PROPERTIES CXX_SCAN_FOR_MODULES OFF)
endif()

target_include_directories(kano_git_test_core
  PUBLIC
    generators/public
    support/public
  PRIVATE
    generators/private
    support/private
)

target_link_libraries(kano_git_test_core
  PUBLIC Catch2::Catch2
  PRIVATE kano_git_core
)
