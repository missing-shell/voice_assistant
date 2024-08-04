set(APP_DIR ./app)
file(GLOB_RECURSE APP_SRCS ${APP_DIR}/*.c)


idf_component_register(
    SRCS
        "main.c"
        ${APP_SRCS}
    INCLUDE_DIRS
        "."
        ${APP_DIR}
        
)
spiffs_create_partition_image(storage ../spiffs FLASH_IN_PROJECT)