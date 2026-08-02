#pragma once

#include "esp_eth.h"
#include "esp_netif.h"

class WiredNetwork {
public:
    using ReadyCallback = void (*)(void *context);

    WiredNetwork();
    bool start(ReadyCallback callback, void *context);

private:
    esp_eth_handle_t _ethHandle;
    esp_netif_t *_netif;
    esp_eth_netif_glue_handle_t _glue;
    ReadyCallback _readyCallback;
    void *_callbackContext;

    static void ethEvent(void *arg, esp_event_base_t base,
                         int32_t eventId, void *eventData);
    static void ipEvent(void *arg, esp_event_base_t base,
                        int32_t eventId, void *eventData);
};
