/**
 * @file http_server.c
 * @brief HTTP server for WiFi configuration and Geogram Station API
 */

#include <stdio.h>
#include <string.h>
#include "http_server.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "station.h"
#include "ws_server.h"
#include "app_config.h"

#if BOARD_MODEL == MODEL_ESP32S3_EPAPER_1IN54
#include "tiles.h"
#include "updates.h"
#endif

// Chat support (in-memory history; mesh broadcast optional)
#define CHAT_ENABLED 1
#include "mesh_chat.h"

#ifdef CONFIG_GEOGRAM_MESH_ENABLED
#include "mesh_bsp.h"
#endif

static const char *TAG = "http_server";

static httpd_handle_t s_server = NULL;
static wifi_config_callback_t s_config_callback = NULL;
static bool s_station_api_enabled = false;

// HTML configuration page
static const char *CONFIG_PAGE_HTML =
    "<!DOCTYPE html>"
    "<html>"
    "<head>"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
    "<title>Geogram WiFi Setup</title>"
    "<style>"
    "body{font-family:Arial,sans-serif;max-width:400px;margin:40px auto;padding:20px;background:#f5f5f5;}"
    ".container{background:white;padding:30px;border-radius:8px;box-shadow:0 2px 10px rgba(0,0,0,0.1);}"
    "h1{color:#333;margin-bottom:20px;font-size:24px;}"
    "label{display:block;margin:15px 0 5px;color:#555;}"
    "input[type=text],input[type=password]{width:100%;padding:12px;border:1px solid #ddd;border-radius:4px;box-sizing:border-box;font-size:16px;}"
    "input[type=submit]{width:100%;padding:14px;background:#2196F3;color:white;border:none;border-radius:4px;cursor:pointer;font-size:16px;margin-top:20px;}"
    "input[type=submit]:hover{background:#1976D2;}"
    "</style>"
    "</head>"
    "<body>"
    "<div class=\"container\">"
    "<h1>Geogram WiFi Setup</h1>"
    "<form action=\"/connect\" method=\"POST\">"
    "<label for=\"ssid\">WiFi Network Name (SSID)</label>"
    "<input type=\"text\" id=\"ssid\" name=\"ssid\" required maxlength=\"32\">"
    "<label for=\"password\">Password</label>"
    "<input type=\"password\" id=\"password\" name=\"password\" maxlength=\"64\">"
    "<input type=\"submit\" value=\"Connect\">"
    "</form>"
    "</div>"
    "</body>"
    "</html>";

// Landing page with chat - Terminimal theme
static const char *LANDING_PAGE_HTML =
    "<!DOCTYPE html>"
    "<html>"
    "<head>"
    "<meta charset=\"UTF-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no\">"
    "<title>Geogram</title>"
    "<style>"
    ":root{--accent:#ffa86a;--bg:#101010;--text:#f0f0f0;--border:rgba(255,240,224,.125);--muted:#888}"
    "*{box-sizing:border-box;margin:0;padding:0}"
    "html,body{height:100%;overflow:hidden}"
    "body{font-family:-apple-system,BlinkMacSystemFont,sans-serif;background:var(--bg);color:var(--text);font-size:14px;display:flex;flex-direction:column}"
    ".header{border-bottom:1px solid var(--border);padding:12px;display:flex;align-items:center;gap:12px}"
    ".header .logo{font-size:18px;font-weight:bold;color:var(--accent)}"
    ".header nav{display:flex;gap:12px;margin-left:auto}"
    ".header nav a{color:var(--text);text-decoration:none;font-size:12px}"
    ".chat{flex:1;display:flex;flex-direction:column;min-height:0}"
    ".messages{flex:1;overflow-y:auto;padding:12px;display:flex;flex-direction:column;gap:8px}"
    ".msg{max-width:85%}"
    ".msg .meta{font-size:11px;margin-bottom:2px}"
    ".msg .author{color:var(--accent);font-weight:bold}"
    ".msg .time{color:var(--muted);margin-left:6px}"
    ".msg .text{color:var(--text);word-wrap:break-word}"
    ".msg.local{align-self:flex-end;text-align:right}"
    ".msg.remote{align-self:flex-start}"
    ".msg.system{align-self:center;color:var(--muted);font-size:12px;font-style:italic}"
    ".input-area{border-top:1px solid var(--border);padding:12px;display:flex;gap:8px}"
    ".input-area input{flex:1;background:transparent;border:1px solid var(--border);border-radius:4px;padding:10px;color:var(--text);font-size:16px;outline:none}"
    ".input-area input:focus{border-color:var(--accent)}"
    ".input-area button{background:var(--accent);color:var(--bg);border:none;border-radius:4px;padding:10px 16px;font-weight:bold;cursor:pointer}"
    ".status-bar{border-top:1px solid var(--border);padding:6px 12px;font-size:10px;color:var(--muted);display:flex;justify-content:space-between}"
    "</style>"
    "</head>"
    "<body>"
    "<div class=\"header\">"
    "<span class=\"logo\">> geogram</span>"
    "</div>"
    "<div class=\"chat\">"
    "<div class=\"messages\" id=\"messages\"></div>"
    "<div class=\"input-area\">"
    "<input type=\"text\" id=\"input\" placeholder=\"Type a message...\" maxlength=\"200\">"
    "<button id=\"send\">SEND</button>"
    "</div>"
    "</div>"
    "<div class=\"status-bar\">"
    "<span id=\"status\">Connecting...</span>"
    "<span id=\"count\"></span>"
    "</div>"
    "<script>"
    "let lastId=0,maxLen=200;"
    "const $=id=>document.getElementById(id);"
    "function esc(s){return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');}"
    "function fmtTime(ts){const d=new Date(ts*1000);return d.toLocaleTimeString([],{hour:'2-digit',minute:'2-digit'});}"
    "const storageKey='geogram_nostr_keys';"
    "let clientKeys=null;"
    "const BECH32_ALPHABET='qpzry9x8gf2tvdw0s3jn54khce6mua7l';"
    "function b64urlToBytes(s){s=s.replace(/-/g,'+').replace(/_/g,'/');const pad=s.length%4?4-(s.length%4):0;const str=s+'='.repeat(pad);const bin=atob(str);const out=new Uint8Array(bin.length);for(let i=0;i<bin.length;i++){out[i]=bin.charCodeAt(i);}return out;}"
    "function bech32Polymod(values){let chk=1;const gen=[0x3b6a57b2,0x26508e6d,0x1ea119fa,0x3d4233dd,0x2a1462b3];for(const v of values){const top=chk>>25;chk=(chk&0x1ffffff)<<5^v;for(let i=0;i<5;i++){if((top>>i)&1){chk^=gen[i];}}}return chk;}"
    "function bech32HrpExpand(hrp){const ret=[];for(let i=0;i<hrp.length;i++)ret.push(hrp.charCodeAt(i)>>5);ret.push(0);for(let i=0;i<hrp.length;i++)ret.push(hrp.charCodeAt(i)&31);return ret;}"
    "function bech32CreateChecksum(hrp,data){const values=bech32HrpExpand(hrp).concat(data);values.push(0,0,0,0,0,0);const mod=bech32Polymod(values)^1;const ret=[];for(let p=0;p<6;p++){ret.push((mod>>5*(5-p))&31);}return ret;}"
    "function bech32Encode(hrp,data){const combined=data.concat(bech32CreateChecksum(hrp,data));let ret=hrp+'1';for(const d of combined){ret+=BECH32_ALPHABET[d];}return ret;}"
    "function convertBits(data,fromBits,toBits,pad){let acc=0,bits=0;const ret=[];const maxv=(1<<toBits)-1;for(const value of data){if(value<0||(value>>fromBits))return null;acc=(acc<<fromBits)|value;bits+=fromBits;while(bits>=toBits){bits-=toBits;ret.push((acc>>bits)&maxv);}}if(pad){if(bits){ret.push((acc<<(toBits-bits))&maxv);}}else if(bits>=fromBits||((acc<<(toBits-bits))&maxv)){return null;}return ret;}"
    "function bech32FromBytes(hrp,bytes){const data=convertBits(bytes,8,5,true);return bech32Encode(hrp,data);}"
    "function callsignFromNpub(npub){const base=npub.startsWith('npub1')?npub.slice(5):npub;return 'X1'+base.slice(0,4).toUpperCase();}"
    "async function generateKeys(){"
    "if(!window.crypto||!window.crypto.subtle){throw new Error('WebCrypto unavailable');}"
    "const keyPair=await crypto.subtle.generateKey({name:'ECDSA',namedCurve:'K-256'},true,['sign','verify']);"
    "const jwkPriv=await crypto.subtle.exportKey('jwk',keyPair.privateKey);"
    "const jwkPub=await crypto.subtle.exportKey('jwk',keyPair.publicKey);"
    "const privBytes=b64urlToBytes(jwkPriv.d);"
    "const pubX=b64urlToBytes(jwkPub.x);"
    "const nsec=bech32FromBytes('nsec',Array.from(privBytes));"
    "const npub=bech32FromBytes('npub',Array.from(pubX));"
    "const callsign=callsignFromNpub(npub);"
    "return {nsec,npub,callsign};"
    "}"
    "async function initKeys(){"
    "const saved=localStorage.getItem(storageKey);"
    "if(saved){try{clientKeys=JSON.parse(saved);}catch(e){clientKeys=null;}}"
    "if(!clientKeys||!clientKeys.nsec||!clientKeys.npub||!clientKeys.callsign){"
    "clientKeys=await generateKeys();"
    "localStorage.setItem(storageKey,JSON.stringify(clientKeys));"
    "}"
    "}"
    "function updateStatus(){"
    "if(clientKeys&&clientKeys.callsign){$('status').textContent='You: '+clientKeys.callsign;}else{$('status').textContent='No keys';}"
    "}"
    "function render(m){"
    "const div=document.createElement('div');"
    "div.className='msg '+(m.local?'local':'remote');"
    "div.innerHTML='<div class=\"meta\"><span class=\"author\">'+esc(m.from)+'</span><span class=\"time\">'+fmtTime(m.ts)+'</span></div><div class=\"text\">'+esc(m.text)+'</div>';"
    "return div;}"
    "async function load(){"
    "try{"
    "const r=await fetch('/api/chat/messages?since='+lastId);"
    "if(!r.ok)return;"
    "const d=await r.json();"
    "if(d.max_len)maxLen=d.max_len;"
    "$('input').maxLength=maxLen;"
    "if(d.messages&&d.messages.length){"
    "d.messages.forEach(m=>{if(m.id>lastId){$('messages').appendChild(render(m));lastId=m.id;}});"
    "$('messages').scrollTop=$('messages').scrollHeight;}"
    "if(d.latest_id>lastId)lastId=d.latest_id;"
    "const station=d.my_callsign?('Station '+d.my_callsign):'';"
    "$('count').textContent=d.count?(d.count+' msgs '+station):station;"
    "}catch(e){$('status').textContent='Offline';}}"
    "async function send(){"
    "const inp=$('input'),txt=inp.value.trim();"
    "if(!txt)return;"
    "if(!clientKeys){await initKeys();updateStatus();}"
    "$('send').disabled=true;"
    "try{"
    "const body='text='+encodeURIComponent(txt)+'&callsign='+(clientKeys?encodeURIComponent(clientKeys.callsign):'');"
    "const r=await fetch('/api/chat/send',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body});"
    "if(r.ok){inp.value='';await load();}"
    "}catch(e){}"
    "$('send').disabled=false;inp.focus();}"
    "$('send').onclick=send;"
    "$('input').onkeypress=e=>{if(e.key==='Enter')send();};"
    "(async()=>{try{await initKeys();updateStatus();await load();setInterval(load,3000);}catch(e){$('status').textContent='Keygen failed';}})();"
    "if(window.visualViewport){"
    "const vv=window.visualViewport;"
    "vv.onresize=()=>{document.body.style.height=vv.height+'px';$('messages').scrollTop=$('messages').scrollHeight;};}"
    "</script>"
    "</body>"
    "</html>";

// Success page
static const char *SUCCESS_PAGE_HTML =
    "<!DOCTYPE html>"
    "<html>"
    "<head>"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
    "<title>Geogram - Connected</title>"
    "<style>"
    "body{font-family:Arial,sans-serif;max-width:400px;margin:40px auto;padding:20px;background:#f5f5f5;}"
    ".container{background:white;padding:30px;border-radius:8px;box-shadow:0 2px 10px rgba(0,0,0,0.1);text-align:center;}"
    "h1{color:#2e7d32;margin-bottom:20px;}"
    "p{color:#555;line-height:1.6;}"
    "</style>"
    "</head>"
    "<body>"
    "<div class=\"container\">"
    "<h1>Configuration Saved</h1>"
    "<p>The device will now attempt to connect to the WiFi network.</p>"
    "<p>If successful, the AP will be disabled and you can close this page.</p>"
    "</div>"
    "</body>"
    "</html>";

/**
 * @brief URL decode a string in-place
 */
static void url_decode(char *str)
{
    char *src = str;
    char *dst = str;

    while (*src) {
        if (*src == '%' && src[1] && src[2]) {
            char hex[3] = {src[1], src[2], 0};
            *dst++ = (char)strtol(hex, NULL, 16);
            src += 3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

/**
 * @brief Extract value from form data
 */
static bool extract_form_value(const char *data, const char *key, char *value, size_t value_len)
{
    char search_key[64];
    snprintf(search_key, sizeof(search_key), "%s=", key);

    const char *start = strstr(data, search_key);
    if (start == NULL) {
        return false;
    }

    start += strlen(search_key);
    const char *end = strchr(start, '&');
    size_t len = end ? (size_t)(end - start) : strlen(start);

    if (len >= value_len) {
        len = value_len - 1;
    }

    strncpy(value, start, len);
    value[len] = '\0';
    url_decode(value);

    return true;
}

/**
 * @brief Handler for captive portal detection - return 204 so devices stay connected
 */
static esp_err_t captive_portal_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/**
 * @brief Handler for root page - serves landing page with chat
 */
static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, LANDING_PAGE_HTML, strlen(LANDING_PAGE_HTML));
    return ESP_OK;
}

/**
 * @brief Handler for setup page (WiFi config form)
 */
static esp_err_t setup_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, CONFIG_PAGE_HTML, strlen(CONFIG_PAGE_HTML));
    return ESP_OK;
}

/**
 * @brief Handler for WiFi configuration POST
 */
static esp_err_t connect_post_handler(httpd_req_t *req)
{
    char content[256];
    int ret;

    int total_len = req->content_len;
    if (total_len >= sizeof(content)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Content too long");
        return ESP_FAIL;
    }

    ret = httpd_req_recv(req, content, total_len);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to receive data");
        return ESP_FAIL;
    }
    content[total_len] = '\0';

    ESP_LOGI(TAG, "Received config: %s", content);

    char ssid[33] = {0};
    char password[65] = {0};

    if (!extract_form_value(content, "ssid", ssid, sizeof(ssid))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing SSID");
        return ESP_FAIL;
    }

    extract_form_value(content, "password", password, sizeof(password));

    ESP_LOGI(TAG, "WiFi config received - SSID: %s", ssid);

    nvs_handle_t nvs;
    esp_err_t err = nvs_open("wifi_config", NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        nvs_set_str(nvs, "ssid", ssid);
        nvs_set_str(nvs, "password", password);
        nvs_commit(nvs);
        nvs_close(nvs);
        ESP_LOGI(TAG, "WiFi credentials saved to NVS");
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, SUCCESS_PAGE_HTML, strlen(SUCCESS_PAGE_HTML));

    if (s_config_callback != NULL) {
        s_config_callback(ssid, password);
    }

    return ESP_OK;
}

/**
 * @brief Handler for status endpoint (JSON) - basic
 */
static esp_err_t status_get_handler(httpd_req_t *req)
{
    char response[128];
    snprintf(response, sizeof(response), "{\"status\":\"ok\",\"device\":\"geogram\"}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, strlen(response));
    return ESP_OK;
}

/**
 * @brief Handler for /api/status endpoint - full station status
 */
static esp_err_t api_status_get_handler(httpd_req_t *req)
{
    char response[512];
    size_t len = station_build_status_json(response, sizeof(response));

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, response, len);
    return ESP_OK;
}

// ============================================================================
// Chat API Endpoints
// ============================================================================

#ifdef CHAT_ENABLED

/**
 * @brief Handler for /api/chat/messages - get chat history
 */
static esp_err_t api_chat_messages_get_handler(httpd_req_t *req)
{
    char query[64] = {0};
    uint32_t since_id = 0;

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char param[16];
        if (httpd_query_key_value(query, "since", param, sizeof(param)) == ESP_OK) {
            since_id = (uint32_t)atoi(param);
        }
    }

    // Get callsign (with null check)
    const char *callsign = station_get_callsign();
    if (!callsign) callsign = "NOCALL";

    // Build response
    const size_t buffer_size = 32768;
    char *buffer = malloc(buffer_size);
    if (!buffer) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    // mesh_chat_build_json returns {"messages":[...]} so we build around it
    int offset = snprintf(buffer, buffer_size,
        "{\"my_callsign\":\"%s\",\"max_len\":%d,\"count\":%d,\"latest_id\":%lu,",
        callsign, MESH_CHAT_MAX_MESSAGE_LEN, (int)mesh_chat_get_count(),
        (unsigned long)mesh_chat_get_latest_id());

    // mesh_chat_build_json writes {"messages":[...]} - we skip the opening { and include rest
    char *json_buf = buffer + offset;
    size_t json_len = mesh_chat_build_json(json_buf, buffer_size - offset - 1, since_id);

    // Skip the opening '{' from mesh_chat_build_json output and keep the rest
    if (json_len > 0 && json_buf[0] == '{') {
        memmove(json_buf, json_buf + 1, json_len);
        offset += json_len - 1;  // -1 because we removed '{'
    } else {
        // Fallback: just add empty messages
        offset += snprintf(buffer + offset, buffer_size - offset, "\"messages\":[]}");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, buffer, offset);

    free(buffer);
    return ESP_OK;
}

/**
 * @brief Handler for /api/chat/send - send a chat message
 */
static esp_err_t api_chat_send_post_handler(httpd_req_t *req)
{
    char *content = malloc(512);
    if (!content) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    int total_len = req->content_len;
    if (total_len >= 512) {
        free(content);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Content too long");
        return ESP_FAIL;
    }

    int ret = httpd_req_recv(req, content, total_len);
    if (ret <= 0) {
        free(content);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to receive data");
        return ESP_FAIL;
    }
    content[total_len] = '\0';

    // Extract text from form data
    char text[MESH_CHAT_MAX_MESSAGE_LEN + 1] = {0};
    if (!extract_form_value(content, "text", text, sizeof(text)) || strlen(text) == 0) {
        free(content);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing text");
        return ESP_FAIL;
    }

    // Optional callsign from client
    char callsign[MESH_CHAT_MAX_CALLSIGN_LEN + 1] = {0};
    extract_form_value(content, "callsign", callsign, sizeof(callsign));

    free(content);

    // Store message locally with provided callsign (no mesh broadcast)
    esp_err_t err = mesh_chat_add_local_message(callsign[0] ? callsign : NULL, text);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to send");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, "{\"ok\":true}", 11);
    return ESP_OK;
}

static const httpd_uri_t uri_api_chat_messages = {
    .uri = "/api/chat/messages",
    .method = HTTP_GET,
    .handler = api_chat_messages_get_handler,
    .user_ctx = NULL
};

static const httpd_uri_t uri_api_chat_send = {
    .uri = "/api/chat/send",
    .method = HTTP_POST,
    .handler = api_chat_send_post_handler,
    .user_ctx = NULL
};

#endif // CHAT_ENABLED

// ============================================================================
// URI definitions
// ============================================================================

static const httpd_uri_t uri_root = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = root_get_handler,
    .user_ctx = NULL
};

static const httpd_uri_t uri_setup = {
    .uri = "/setup",
    .method = HTTP_GET,
    .handler = setup_get_handler,
    .user_ctx = NULL
};

static const httpd_uri_t uri_connect = {
    .uri = "/connect",
    .method = HTTP_POST,
    .handler = connect_post_handler,
    .user_ctx = NULL
};

static const httpd_uri_t uri_status = {
    .uri = "/status",
    .method = HTTP_GET,
    .handler = status_get_handler,
    .user_ctx = NULL
};

static const httpd_uri_t uri_api_status = {
    .uri = "/api/status",
    .method = HTTP_GET,
    .handler = api_status_get_handler,
    .user_ctx = NULL
};

// Captive portal detection URIs
static const httpd_uri_t uri_generate_204 = {
    .uri = "/generate_204",
    .method = HTTP_GET,
    .handler = captive_portal_handler,
    .user_ctx = NULL
};

static const httpd_uri_t uri_hotspot_detect = {
    .uri = "/hotspot-detect.html",
    .method = HTTP_GET,
    .handler = captive_portal_handler,
    .user_ctx = NULL
};

// ============================================================================
// Server start/stop
// ============================================================================

esp_err_t http_server_start(wifi_config_callback_t callback)
{
    return http_server_start_ex(callback, false);
}

esp_err_t http_server_start_ex(wifi_config_callback_t callback, bool enable_station_api)
{
    if (s_server != NULL) {
        ESP_LOGW(TAG, "Server already running");
        return ESP_OK;
    }

    s_config_callback = callback;
    s_station_api_enabled = enable_station_api;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.stack_size = 8192;
    config.max_uri_handlers = 16;

    ESP_LOGI(TAG, "Starting HTTP server on port %d (station_api=%d)", config.server_port, enable_station_api);

    esp_err_t ret = httpd_start(&s_server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(ret));
        return ret;
    }

    // Register base URI handlers
    httpd_register_uri_handler(s_server, &uri_root);
    httpd_register_uri_handler(s_server, &uri_setup);
    httpd_register_uri_handler(s_server, &uri_connect);
    httpd_register_uri_handler(s_server, &uri_status);

    // Register captive portal handlers
    httpd_register_uri_handler(s_server, &uri_generate_204);
    httpd_register_uri_handler(s_server, &uri_hotspot_detect);

    // Register Station API handlers if enabled
    if (enable_station_api) {
        httpd_register_uri_handler(s_server, &uri_api_status);

#ifdef CHAT_ENABLED
        httpd_register_uri_handler(s_server, &uri_api_chat_messages);
        httpd_register_uri_handler(s_server, &uri_api_chat_send);

        // Initialize chat system
        mesh_chat_init();
        ESP_LOGI(TAG, "Chat API endpoints registered");
#endif

        // Register WebSocket handler
        ret = ws_server_register(s_server);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to register WebSocket handler: %s", esp_err_to_name(ret));
        }

#if BOARD_MODEL == MODEL_ESP32S3_EPAPER_1IN54
        // Register tile server handler if SD card is available
        ret = tiles_register_http_handler(s_server);
        if (ret != ESP_OK) {
            ESP_LOGI(TAG, "Tile server not available (no SD card)");
        }

        // Register update mirror handlers if available
        ret = updates_register_http_handlers(s_server);
        if (ret != ESP_OK) {
            ESP_LOGI(TAG, "Update mirror not available (no SD card)");
        }
#endif

        ESP_LOGI(TAG, "Station API endpoints registered");
    }

    ESP_LOGI(TAG, "HTTP server started");
    return ESP_OK;
}

esp_err_t http_server_stop(void)
{
    if (s_server == NULL) {
        return ESP_OK;
    }

    esp_err_t ret = httpd_stop(s_server);
    s_server = NULL;
    s_config_callback = NULL;

    ESP_LOGI(TAG, "HTTP server stopped");
    return ret;
}

bool http_server_is_running(void)
{
    return s_server != NULL;
}

httpd_handle_t http_server_get_handle(void)
{
    return s_server;
}
