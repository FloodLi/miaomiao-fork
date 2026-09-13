idf_component_register(
    SRCS "test_startup_state.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../../main/display_policy.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../../main/display_mode.c"
    INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../../main"
    REQUIRES unity
)
