idf_component_register(
    SRCS
        "app_main.c"
        "config.c"
        "wifi_manager.c"
        "uart_bridge.c"
        "ws_hub.c"
        "http_server.c"
    INCLUDE_DIRS "include"
    EMBED_TXTFILES "www/index.html"
)
