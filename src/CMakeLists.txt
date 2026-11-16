idf_component_register(
    SRCS
        "main.c"
        "can1_twai.c"
        "xl2515.c"
        "gateway.c"
        "filter.c"
        "stats.c"
        "config.c"
        "config_json.c"
        "web.c"
        "wifi.c"
        "ota.c"
        "cli.c"
        "web_assets.c"
    INCLUDE_DIRS "."
)
