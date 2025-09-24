#ifndef MQTT_FEATURES_H
#define MQTT_FEATURES_H

// MQTT feature detection and configuration
#ifdef ENABLE_MQTT
    #define HAVE_MQTT_SUPPORT 1
    #include <mosquitto.h>
#else
    #define HAVE_MQTT_SUPPORT 0
    // Define stub types when MQTT is disabled to avoid compilation errors
    typedef void* mosquitto;
#endif

// MQTT configuration defaults
#define MQTT_DEFAULT_HOST "localhost"
#define MQTT_DEFAULT_PORT 1883
#define MQTT_DEFAULT_TOPIC "meshtastic/adsb/watch"
#define MQTT_DEFAULT_QOS 0
#define MQTT_DEFAULT_RETAIN false
#define MQTT_DEFAULT_KEEPALIVE 60
#define MQTT_RECONNECT_DELAY_MIN 1
#define MQTT_RECONNECT_DELAY_MAX 60
#define MQTT_ERROR_LOG_INTERVAL 30

// Watchlist configuration defaults
#define WATCHLIST_DEFAULT_FILE "/etc/readsb/watchlist.json"
#define WATCHLIST_DEFAULT_COOLDOWN 300

#endif // MQTT_FEATURES_H