set(COMPONENT_SRCS "main.c" "app_wifi.c"  "app_httpd.c")
set(COMPONENT_ADD_INCLUDEDIRS "include")

# set(COMPONENT_REQUIRES
#     nvs_flash
#     esp_http_server
#     )

set(COMPONENT_EMBED_FILES
	"www/SaveResponseSucc.html.gz"
	"www/SaveResponseError.html.gz"
        "www/settingPage.html.gz")

register_component()
