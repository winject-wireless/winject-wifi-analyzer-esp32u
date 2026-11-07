set(winject_srcs
    "winject-esp32/config_types.cpp"
    "winject-esp32/iq/iq_server.cpp"
    "winject-esp32/mplane/device_backend_esp.cpp"
    "winject-esp32/mplane/mplane_args.cpp"
    "winject-esp32/mplane/mplane_commands.cpp"
    "winject-esp32/mplane/mplane_reply.cpp"
    "winject-esp32/mplane/mplane_req_id.cpp"
    "winject-esp32/mplane/mplane_version.cpp"
    "winject-esp32/net/mplane_udp.cpp"
    "winject-esp32/net/net_proto.cpp"
    "winject-esp32/net/netstack.cpp"
    "winject-esp32/settings.cpp"
    "winject-esp32/settings_blob.cpp"
)

# No lwIP / esp_netif / httpd: the raw Ethernet stack (net/) replaces them.
idf_component_register(
    SRCS ${winject_srcs} "winject-esp32/main.cpp"
    INCLUDE_DIRS
        "."
        "winject-esp32"
        "winject-esp32/iq"
        "winject-esp32/mplane"
        "winject-esp32/net"
    REQUIRES
        analyzer_iq
        app_update
        esp_driver_gpio
        esp_eth
        esp_event
        esp_rom
        esp_timer
        esp_wifi
        nvs_flash
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
