idf_component_register(
    SRCS "src/state.c" "src/cmd.c"
    INCLUDE_DIRS "include"
    REQUIRES sloop_core
    PRIV_REQUIRES sloop_hal_tab5 freertos log
)
