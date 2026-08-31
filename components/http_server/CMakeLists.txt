set(FRONTEND_SOURCE_DIR        "${CMAKE_CURRENT_LIST_DIR}/frontend_source")
set(FRONTEND_DIST_HTML_GZ      "${FRONTEND_SOURCE_DIR}/dist/index.html.gz")
set(FRONTEND_PUBLIC_FAVICON_ICO  "${FRONTEND_SOURCE_DIR}/public/favicon.ico")

if(NOT EXISTS "${FRONTEND_DIST_HTML_GZ}")
    message(FATAL_ERROR
        "\n"
        "Frontend bundle not found: ${FRONTEND_DIST_HTML_GZ}\n"
        "Build the SolidJS UI before running idf.py build, e.g.:\n"
        "    cd ${FRONTEND_SOURCE_DIR}\n"
        "    pnpm install --frozen-lockfile\n"
        "    pnpm run build\n")
endif()

set(http_server_srcs
    "http_server_core.c"
    "http_server_assets.c"
    "http_server_capabilities_api.c"
    "http_server_config_api.c"
    "http_server_lua_modules_api.c"
    "http_server_status_api.c"
    "http_server_files_api.c"
    "http_server_wechat_api.c"
    "http_server_webim_api.c"
    "http_server_json.c"
    "http_server_utils.c"
)

set(http_server_embed_files
    "${FRONTEND_DIST_HTML_GZ}"
    "${FRONTEND_PUBLIC_FAVICON_ICO}"
)

set(http_server_requires
    app_config
    app_claw
    cap_im_local
    claw_cap
    esp_http_server
    heap
)

if(CONFIG_MP4_ROBOT_ENABLE)
    # The /v1 API the MPX-Dog PWA speaks.
    list(APPEND http_server_srcs
        "http_server_mpx_robot_api.c"
        "http_server_mpx_skills_api.c"
        "http_server_mpx_fs_api.c"
        "http_server_mpx_studio_api.c"
        "http_server_mpx_wifi_api.c"
        "http_server_mpx_market_api.c"
        "http_server_mpx_chat_api.c")
    list(APPEND http_server_requires mpx_robot mpx_wasm mpx_util fatfs esp_http_client)

    # Both web bundles move to the read-only system partition: the app image
    # keeps neither, and gains room instead of losing 199 KB of .rodata to
    # files that never execute. http_server_mpx_web.c streams them from there
    # and takes over "/" from http_server_assets.c, which is why that file is
    # dropped rather than sitting alongside registering a competing route.
    list(REMOVE_ITEM http_server_srcs "http_server_assets.c")
    list(APPEND http_server_srcs "http_server_mpx_web.c")
    set(http_server_embed_files "${FRONTEND_PUBLIC_FAVICON_ICO}")
    if(CONFIG_APP_CLAW_CAP_LUA)
        # The PWA's /v1/lua store, backed by ESP-Claw's Lua runtime.
        list(APPEND http_server_srcs "http_server_mpx_lua_api.c")
        list(APPEND http_server_requires cap_lua)
    endif()
endif()

if(CONFIG_APP_CLAW_LUA_MODULE_HTTP_SERVER)
    list(APPEND http_server_srcs "http_server_lua_app_api.c")
    list(APPEND http_server_requires lua_module_http_server)
endif()

idf_component_register(
    SRCS
        ${http_server_srcs}
    INCLUDE_DIRS
        "include"
    EMBED_FILES
        ${http_server_embed_files}
    REQUIRES
        ${http_server_requires}
)

# Rebuild the component whenever the bundled UI asset changes on disk.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${FRONTEND_DIST_HTML_GZ}")

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
