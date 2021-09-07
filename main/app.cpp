#include <esp_http_client.h>
#include <cJSON.h>
#include <sys/param.h>
#include <hal/gpio_types.h>
#include <driver/gpio.h>
#include <freertos/timers.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_netif.h"

#include "esp_tls.h"

extern "C" {
#include "startup/utils.h"
#include "startup/wifi.h"
#include "startup/console.h"
}

#include "secret/telegram.h"

#define TAG "app"

#define GPIO_OUTPUT_IO_0    GPIO_NUM_16
#define GPIO_OUTPUT_IO_1    GPIO_NUM_17
#define GPIO_OUTPUT_PIN_SEL  ((1ull<<GPIO_OUTPUT_IO_0) | (1ull<<GPIO_OUTPUT_IO_1))

#define STRLEN(s) (sizeof(s)/sizeof(s[0]))

#define WEB_SERVER "api.telegram.org"
#define WEB_URL "https://" WEB_SERVER "/bot" BOT_TOKEN "/"
#define API_GET_UPDATES WEB_URL "getUpdates?timeout=3&limit=1&offset=%d"
#define API_SEND_MESSAGE WEB_URL "sendMessage?disable_notification=true&chat_id=%d&text=%s&reply_to_message_id=%d"

extern const unsigned char cert_store_pem_start[] asm("_binary_store_pem_start");
extern const unsigned char cert_store_pem_end[]   asm("_binary_store_pem_end");

int update_id = -1;

typedef int (*payload_handler_cb)(esp_http_client_handle_t client, void *context, char *buf, uint16_t len);

typedef struct request_context {
    char *buf = NULL, *date = NULL;
    uint16_t offset = 0;

    payload_handler_cb payload_handler = NULL;
    int payload_result = 0;

    request_context(payload_handler_cb payload_handler) : payload_handler(payload_handler) {};

    ~request_context() {
        if (buf != NULL) delete[] buf;
        if (date != NULL) delete[] date;
    }
} request_context_t;

esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    request_context_t *context = (request_context_t *) evt->user_data;

    switch (evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGE(TAG, "HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
            break;
        case HTTP_EVENT_HEADER_SENT:
            ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
            break;
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);

            if (context->date == NULL && strcmp("Date", evt->header_key) == 0) {
                size_t sz = strlen(evt->header_value) + 1;
                context->date = new char[sz];
                memcpy(context->date, evt->header_value, sz);
            }

            break;
        case HTTP_EVENT_ON_DATA:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
            if (!esp_http_client_is_chunked_response(evt->client)) {
                // Write out data
                //printf("%.*s\n", evt->data_len, (char *) evt->data);

                int content_len = esp_http_client_get_content_length(evt->client);
                if (content_len > 0) {
                    if (evt->data_len >= content_len) {
                        if (context->payload_handler != NULL)
                            context->payload_result = context->payload_handler(evt->client, context, (char *) evt->data,
                                                                               evt->data_len);
                    } else {
                        if (context->buf == NULL)
                            context->buf = new char[content_len];

                        uint16_t len = MIN(evt->data_len, content_len - context->offset);
                        if (len > 0) {
                            memcpy(context->buf + context->offset, evt->data, len);
                            context->offset += len;
                        }

                        if (context->offset >= content_len) {
                            if (context->payload_handler != NULL)
                                context->payload_result = context->payload_handler(
                                        evt->client, context, context->buf, content_len);
                        } else {
                            ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, waiting %d bytes more", content_len - context->offset);
                        }
                    }
                } else {
                    ESP_LOGI(TAG, "HTTP_EVENT_ON_DATA, no Content-Length header");
                }
            }

            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");

            break;
        case HTTP_EVENT_DISCONNECTED: {
            ESP_LOGD(TAG, "HTTP_EVENT_DISCONNECTED");
            int mbedtls_err = 0;
            esp_err_t err = esp_tls_get_and_clear_last_error((esp_tls_error_handle_t) evt->data, &mbedtls_err, NULL);
            if (err != 0) {
                ESP_LOGI(TAG, "Last esp error code: 0x%x", err);
                ESP_LOGI(TAG, "Last mbedtls failure: 0x%x", mbedtls_err);
            }
            break;
        }
        default:
            break;
    }

    return ESP_OK;
}

static int https_get(const char *url, payload_handler_cb payload_handler) {
    url = "https://httpbin.org/get?test=1";

    ESP_LOGI(TAG, "%s", url);

    request_context_t *context = new request_context_t(payload_handler);

    esp_http_client_config_t config = {
            .url = url,
            //.cert_pem = cert_store_pem_start,
            //.timeout_ms = 10000,
            .event_handler = http_event_handler,
            .user_data = context,
            .is_async = true,
            .use_global_ca_store = true,
            //.keep_alive_enable = true,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err;
    while (1) {
        err = esp_http_client_perform(client);
        if (err != ESP_ERR_HTTP_EAGAIN) {
            break;
        }
        vTaskDelay(2000 / portTICK_RATE_MS);
        ESP_LOGI(TAG, "https_get delay");
 
    }

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "HTTPS Status = %d, content_length = %d",
                 esp_http_client_get_status_code(client),
                 esp_http_client_get_content_length(client));
    } else {
        ESP_LOGE(TAG, "Error perform http request %s", esp_err_to_name(err));
    }

    int res = context->payload_result;

    esp_http_client_cleanup(client);

    delete context;

    return err == ESP_OK ? res : -1;
}

QueueHandle_t xHttpUrlQueue;

static void https_get_task(void *pvParameters) {
    xHttpUrlQueue = xQueueCreate(5, sizeof(char *));

    char *url;

    while (1) {
        if (xQueueReceive(xHttpUrlQueue, &url, portMAX_DELAY) == pdTRUE) {
            //https_get(url, NULL);

            delete[] url;
        }
    }
}

static void queueHttpsGet(const char *url) {
    if (uxQueueSpacesAvailable(xHttpUrlQueue) == 0) {
        char *oldUrl;
        if (xQueueReceive(xHttpUrlQueue, &oldUrl, 0) == pdTRUE)
            delete[] oldUrl;
    }

    if (xQueueSend(xHttpUrlQueue, &url, 0) != pdTRUE)
        delete[] url;
}


static int onGetUpdatesJSON(esp_http_client_handle_t client, void *context, char *buf, uint16_t len) {
    // {
    // "ok":true,
    // "result":[{
    // "update_id":52188842,
    // "message":{"message_id":23153,"from":{"id":119961304,"is_bot":false,"first_name":"Alex","last_name":"A","language_code":"ru"},"chat":{"id":119961304,"first_name":"Alex","last_name":"A","type":"private"},"date":1581346608,"text":"/door1","entities":[{"offset":0,"length":6,"type":"bot_command"}]}
    // }]
    // }

    cJSON *root = cJSON_Parse(buf);
    {
        if (!cJSON_IsTrue(cJSON_GetObjectItem(root, "ok"))) goto fail;

        cJSON *result = cJSON_GetObjectItem(root, "result");

        int sz = cJSON_GetArraySize(result);
        if (sz == 0) goto ok;

        result = cJSON_GetArrayItem(result, sz - 1);
        if (!cJSON_IsObject(result)) goto fail;
        cJSON *_update_id = cJSON_GetObjectItem(result, "update_id");
        if (!cJSON_IsNumber(_update_id)) goto fail;

        update_id = _update_id->valueint;

        cJSON *_msg = cJSON_GetObjectItem(result, "message");
        if (!cJSON_IsObject(_msg)) goto fail;
        cJSON *_msgId = cJSON_GetObjectItem(_msg, "message_id");
        if (!cJSON_IsNumber(_msgId)) goto fail;
        cJSON *_msgText = cJSON_GetObjectItem(_msg, "text");
        if (!cJSON_IsString(_msgText)) goto fail;
        cJSON *_msgDate = cJSON_GetObjectItem(_msg, "date");
        if (!cJSON_IsNumber(_msgDate)) goto fail;
        cJSON *_from = cJSON_GetObjectItem(_msg, "from");
        if (!cJSON_IsObject(_from)) goto fail;
        cJSON *_fromId = cJSON_GetObjectItem(_from, "id");
        if (!cJSON_IsNumber(_fromId)) goto fail;

        time_t respDate = 0l;
        char *date = ((request_context_t *) context)->date;
        if (date != NULL) {
            tm tm;
            strptime(date, "%a, %d %b %Y %H:%M:%S", &tm);
            respDate = mktime(&tm);
        }

        char *url = new char[256];

        if (respDate - _msgDate->valueint > 1) {
            ESP_LOGI(TAG, "too late {update_id=%d, msgId=%d, fromId=%d, msgText=%s, msgDate=%d, respDate=%ld}",
                     _update_id->valueint, _msgId->valueint, _fromId->valueint, _msgText->valuestring,
                     _msgDate->valueint,
                     respDate);

            sprintf(url, API_SEND_MESSAGE, _fromId->valueint, "too+late", _msgId->valueint);
        } else {
            ESP_LOGD(TAG, "{update_id=%d, msgId=%d, fromId=%d, msgText=%s, msgDate=%d, respDate=%ld}",
                     _update_id->valueint, _msgId->valueint, _fromId->valueint, _msgText->valuestring,
                     _msgDate->valueint,
                     respDate);

            if (strcmp("/door1", _msgText->valuestring) == 0) {
                gpio_set_level(GPIO_OUTPUT_IO_0, 0);
                vTaskDelay(500 / portTICK_RATE_MS);
                gpio_set_level(GPIO_OUTPUT_IO_0, 1);

                sprintf(url, API_SEND_MESSAGE, _fromId->valueint, "triggered", _msgId->valueint);
            } else if (strcmp("/door2", _msgText->valuestring) == 0) {
                gpio_set_level(GPIO_OUTPUT_IO_1, 0);
                vTaskDelay(500 / portTICK_RATE_MS);
                gpio_set_level(GPIO_OUTPUT_IO_1, 1);

                sprintf(url, API_SEND_MESSAGE, _fromId->valueint, "triggered", _msgId->valueint);
            } else if (strcmp("/info", _msgText->valuestring) == 0) {
                char text[64];
                sprintf(text, "Free+heap+%d+KiB", esp_get_free_heap_size() / 1024);

                sprintf(url, API_SEND_MESSAGE, _fromId->valueint, text, _msgId->valueint);
            } else {
                sprintf(url, API_SEND_MESSAGE, _fromId->valueint, "unknown", _msgId->valueint);
            }
        }

        //https_get(url, NULL);
        //xTaskCreate(&https_get_task, "https_get_task", 8192, url, 5, NULL);

        //queueHttpsGet(url);
        ESP_LOGI(TAG, "queueHttpsGet %s", url);
    }

    ok:
    cJSON_Delete(root);
    return 0;

    fail:
    ESP_LOGE(TAG, "not parsed %.*s", len, buf);

    cJSON_Delete(root);
    return 1;
}

static void get_updates_task(void *pvParameters) {
    while (1) {
        static char url[STRLEN(API_GET_UPDATES) + 11];
        sprintf(url, API_GET_UPDATES, update_id + 1);

        if (https_get(url, onGetUpdatesJSON) != 0)
            vTaskDelay(3000 / portTICK_PERIOD_MS);
    }
}

TimerHandle_t xWatchdogTimer;
int64_t prevHeapSize = 0;

void onWatchdog(TimerHandle_t xTimer) {
   int64_t heapSize = esp_get_free_heap_size();

   if (heapSize - prevHeapSize > 10 * 1024) {
       prevHeapSize = heapSize;

       char text[64];
       sprintf(text, "Free+heap+%lld+KiB", heapSize / 1024);

       char *url = new char[256];
       sprintf(url, API_SEND_MESSAGE, ADMIN_ID, text, 0);

       queueHttpsGet(url);
   }

    esp_restart();
}

extern "C" void app_main() {
    esp_log_level_set("*", ESP_LOG_DEBUG);

    gpio_config_t io_conf = {
            .pin_bit_mask = GPIO_OUTPUT_PIN_SEL,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
    };

    gpio_config(&io_conf);
    gpio_set_level(GPIO_OUTPUT_IO_0, 1);
    gpio_set_level(GPIO_OUTPUT_IO_1, 1);

//    info_startup();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    nvs_startup();
    wifi_startup_sta();
#if CONFIG_STORE_HISTORY
    fs_init();
#endif

    ESP_ERROR_CHECK(esp_tls_init_global_ca_store());
    ESP_ERROR_CHECK(esp_tls_set_global_ca_store(cert_store_pem_start, cert_store_pem_end - cert_store_pem_start));

//    ESP_LOGI(TAG, "Starting webserver...");
//    webserv_start();

    console_startup();

    //xTaskCreate(&https_get_task, "https_get", 8192, NULL, 5, NULL);
    xTaskCreate(&get_updates_task, "get_updates", 8192, NULL, 5, NULL);

//    char *url = new char[256];
//    sprintf(url, API_SEND_MESSAGE, ADMIN_ID, "startup", 0);
//    queueHttpsGet(url);

    // xWatchdogTimer = xTimerCreate(
    //         /* Just a text name, not used by the RTOS kernel. */
    //         "WatchdogTimer",
    //         /* The timer period in ticks, must be grxeater than 0. */
    //         4 * 60 * 60 * 1000 / portTICK_RATE_MS,
    //         /* The timers will auto-reload themselves when they expire. */
    //         pdTRUE,
    //         /* The ID is used to store a count of the number of times the timer has expired, which is initialised to 0. */
    //         (void *) 0,
    //         /* Each timer calls the same callback when it expires. */
    //         onWatchdog
    // );

    // xTimerStart(xWatchdogTimer, 0);

    //console_loop();
}