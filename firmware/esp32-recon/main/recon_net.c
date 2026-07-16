// esp32-recon networking: WiFi STA (creds from NVS), mDNS (esp32-recon.local), and
// a raw-TCP "telnet" console on :2323 that drives the SAME esp_console command set
// as the UART REPL. Reach it with:  nc esp32-recon.local 2323
#include <string.h>
#include <unistd.h>

#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_console.h"
#include "nvs.h"
#include "mdns.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "recon.h"

// Optional baked-in WiFi creds (for headless/OTA flashing — no UART to provision).
// Provide main/wifi_secrets.h (gitignored; see wifi_secrets.h.example) to define
// RECON_WIFI_SSID / RECON_WIFI_PASS. Absent -> UART provisioning only.
#if defined(__has_include)
#  if __has_include("wifi_secrets.h")
#    include "wifi_secrets.h"
#  endif
#endif

static const char *TAG = "recon-net";
#define NVS_NS "recon"
#define TCP_PORT 2323

static bool         s_wifi_inited = false;
static bool         s_connected   = false;
static int          s_retries     = 0;
static char         s_ip[16]      = "0.0.0.0";
static char         s_ssid[33]    = {0};

// ---------------------------------------------------------------------------
// WiFi
// ---------------------------------------------------------------------------
static void start_mdns(void)
{
    static bool up = false;
    if (up) return;
    if (mdns_init() != ESP_OK) { ESP_LOGW(TAG, "mdns_init failed"); return; }
    mdns_hostname_set("esp32-recon");
    mdns_instance_name_set("Hisense RS-485 recon");
    mdns_service_add(NULL, "_telnet", "_tcp", TCP_PORT, NULL, 0);
    up = true;
    ESP_LOGI(TAG, "mDNS: esp32-recon.local (telnet :%d)", TCP_PORT);
}

static void wifi_evt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        if (s_retries++ < 100) { vTaskDelay(pdMS_TO_TICKS(2000)); esp_wifi_connect(); }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_connected = true; s_retries = 0;
        ESP_LOGI(TAG, "WiFi up: %s  (esp32-recon.local)", s_ip);
        start_mdns();
    }
}

static void wifi_init_once(void)
{
    if (s_wifi_inited) return;
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_evt, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_evt, NULL, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    s_wifi_inited = true;
}

static void wifi_connect(const char *ssid, const char *pass)
{
    wifi_init_once();
    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));
    strlcpy(s_ssid, ssid, sizeof(s_ssid));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    esp_wifi_start();       // fires STA_START -> connect; if already started, connect directly
    esp_wifi_connect();
}

bool recon_wifi_start_from_nvs(void)
{
    nvs_handle_t h;
    char ssid[33] = {0}, pass[65] = {0};
    size_t ls = sizeof(ssid), lp = sizeof(pass);
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    bool ok = nvs_get_str(h, "ssid", ssid, &ls) == ESP_OK && ls > 1;
    nvs_get_str(h, "pass", pass, &lp);
    nvs_close(h);
    if (!ok) return false;
    ESP_LOGI(TAG, "WiFi: connecting to '%s' (from NVS)", ssid);
    wifi_connect(ssid, pass);
    return true;
}

esp_err_t recon_wifi_set_creds(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    nvs_set_str(h, "ssid", ssid);
    nvs_set_str(h, "pass", pass);
    e = nvs_commit(h);
    nvs_close(h);
    wifi_connect(ssid, pass);
    return e;
}

void recon_wifi_clear(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, "ssid"); nvs_erase_key(h, "pass");
        nvs_commit(h); nvs_close(h);
    }
    if (s_wifi_inited) esp_wifi_disconnect();
    s_connected = false;
}

void recon_wifi_status(FILE *out)
{
    fprintf(out, "wifi: %s ssid='%s' ip=%s\r\n",
            s_connected ? "connected" : "down", s_ssid, s_ip);
}

// ---------------------------------------------------------------------------
// TCP telnet console on :2323
// ---------------------------------------------------------------------------
// Read one line from the socket into `line` (NUL-terminated), stripping CR and
// minimal Telnet IAC (0xFF cmd opt) sequences. Returns line length, or -1 on
// disconnect/error.
static int read_line(int fd, char *line, int cap)
{
    int n = 0;
    for (;;) {
        uint8_t b;
        int r = recv(fd, &b, 1, 0);
        if (r <= 0) return -1;
        if (b == 0xFF) {                 // Telnet IAC: skip command + option
            uint8_t skip[2];
            recv(fd, skip, 2, 0);
            continue;
        }
        if (b == '\r') continue;
        if (b == '\n') { line[n] = 0; return n; }
        if (b == 0x08 || b == 0x7f) { if (n > 0) n--; continue; }  // backspace
        if (n < cap - 1) line[n++] = (char)b;
    }
}

static void handle_client(int fd)
{
    // One write-stream over the socket fd for command output; input is read raw
    // with recv() on the same fd (independent directions). fclose(sf) closes fd.
    FILE *sf = fdopen(fd, "w");
    if (!sf) { close(fd); return; }
    setvbuf(sf, NULL, _IONBF, 0);

    // Route this task's stdout to the socket so command printf() reaches the client.
    FILE *saved_out = stdout, *saved_err = stderr;
    stdout = sf; stderr = sf;

    fprintf(sf, "esp32-recon remote console (mode=%s). type 'help'.\r\n",
            recon_mode_str(recon_mode_get()));
    fprintf(sf, "esp32-recon> ");

    char line[256];
    for (;;) {
        int len = read_line(fd, line, sizeof(line));
        if (len < 0) break;
        if (len == 0) { fprintf(sf, "esp32-recon> "); continue; }
        if (!strcmp(line, "exit") || !strcmp(line, "quit")) break;
        int ret = 0;
        esp_err_t e = esp_console_run(line, &ret);
        if (e == ESP_ERR_NOT_FOUND) fprintf(sf, "unknown command (try 'help')\r\n");
        else if (e == ESP_ERR_INVALID_ARG) { /* empty */ }
        fprintf(sf, "esp32-recon> ");
    }

    // Tear down: make sure the bus/tap task stops writing to this stream first.
    recon_watch_clear_if(sf);
    stdout = saved_out; stderr = saved_err;
    fclose(sf);     // closes the socket fd
}

static void tcp_task(void *arg)
{
    (void)arg;
    int ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls < 0) { ESP_LOGE(TAG, "socket() failed"); vTaskDelete(NULL); return; }
    int yes = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(TCP_PORT),
                                .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(ls, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(ls, 1) < 0) {
        ESP_LOGE(TAG, "bind/listen :%d failed", TCP_PORT); close(ls); vTaskDelete(NULL); return;
    }
    ESP_LOGI(TAG, "TCP console listening on :%d", TCP_PORT);
    for (;;) {
        struct sockaddr_in cli; socklen_t cl = sizeof(cli);
        int fd = accept(ls, (struct sockaddr *)&cli, &cl);
        if (fd < 0) { vTaskDelay(pdMS_TO_TICKS(200)); continue; }
        ESP_LOGI(TAG, "console client connected");
        handle_client(fd);
        ESP_LOGI(TAG, "console client disconnected");
    }
}

void recon_net_start(void)
{
    if (!recon_wifi_start_from_nvs()) {
#if defined(RECON_WIFI_SSID)
        ESP_LOGI(TAG, "WiFi: no NVS creds — using baked-in SSID '%s'", RECON_WIFI_SSID);
        wifi_connect(RECON_WIFI_SSID, RECON_WIFI_PASS);
#else
        ESP_LOGW(TAG, "no WiFi creds (NVS or baked) — set over USB console: wifi <ssid> <pass>");
#endif
    }
    // The TCP listener binds on INADDR_ANY and simply waits; it starts serving as
    // soon as WiFi comes up. Give it a generous stack (esp_console + printf).
    xTaskCreate(tcp_task, "recon_tcp", 6144, NULL, 5, NULL);
}
