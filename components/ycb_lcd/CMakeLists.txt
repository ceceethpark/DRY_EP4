idf_component_register(
    SRCS
        "src/ycb_7seg.c"
        "src/ycb_hangul.c"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "src"
    REQUIRES
        lvgl__lvgl
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=missing-braces)
