idf_component_register(
    SRCS "main.c" "heartbeat.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_event esp_netif esp_http_server esp_http_client esp_timer nvs_flash
    PRIV_REQUIRES mbedtls
    EMBED_TXTFILES "status.html"
)

file(GLOB local_links CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/links.local.h")
if(local_links)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE S3NET_LOCAL_LINKS)
endif()
