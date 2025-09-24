// Part of readsb, a Mode-S/ADSB/TIS message decoder.
//
// mqtt_client.c: MQTT client management for watchlist alerting
//
// Copyright (c) 2024 readsb contributors
//
// This file is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// any later version.
//
// This file is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#include "mqtt_client.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

// Define sfree if not available
#ifndef sfree
#define sfree(x) do { if (x) { free(x); (x) = NULL; } } while(0)
#endif

// Include enhanced error handling
#include "watchlist_error_handling.h"

// Rate limiting for error logs
static time_t last_connection_error_log = 0;
static time_t last_publish_error_log = 0;
static time_t last_init_error_log = 0;

#ifdef ENABLE_MQTT

static void mqtt_client_log_error_rate_limited(mqtt_client_t *client, const char *message) {
    time_t now = time(NULL);
    if (now - client->last_error_log >= MQTT_ERROR_LOG_INTERVAL) {
        log_with_timestamp("MQTT error: %s", message);
        client->last_error_log = now;
    }
}

static void mqtt_client_schedule_reconnect(mqtt_client_t *client) {
    if (!client->enabled) {
        return;
    }
    
    time_t now = time(NULL);
    if (now - client->last_reconnect_attempt >= client->reconnect_delay) {
        client->last_reconnect_attempt = now;
        
        if (mqtt_client_connect(client)) {
            // Reset reconnect delay on successful connection
            client->reconnect_delay = MQTT_INITIAL_RECONNECT_DELAY;
        } else {
            // Exponential backoff with maximum limit
            client->reconnect_delay = client->reconnect_delay * 2;
            if (client->reconnect_delay > client->max_reconnect_delay) {
                client->reconnect_delay = client->max_reconnect_delay;
            }
        }
    }
}

void mqtt_client_connect_callback(struct mosquitto *mosq, void *userdata, int result) {
    mqtt_client_t *client = (mqtt_client_t *)userdata;
    
    if (result == 0) {
        client->connected = true;
        client->reconnect_delay = MQTT_INITIAL_RECONNECT_DELAY;
        log_with_timestamp("MQTT connected to %s:%d (topic: %s, qos: %d)", 
                          client->host, client->port, client->topic, client->qos);
    } else {
        client->connected = false;
        char error_msg[256];
        snprintf(error_msg, sizeof(error_msg), "Connection failed: %s", mosquitto_strerror(result));
        mqtt_client_log_error_rate_limited(client, error_msg);
    }
}

void mqtt_client_disconnect_callback(struct mosquitto *mosq, void *userdata, int result) {
    mqtt_client_t *client = (mqtt_client_t *)userdata;
    client->connected = false;
    
    if (result != 0) {
        char error_msg[256];
        snprintf(error_msg, sizeof(error_msg), "Unexpected disconnection: %s", mosquitto_strerror(result));
        mqtt_client_log_error_rate_limited(client, error_msg);
    } else {
        log_with_timestamp("MQTT disconnected from %s:%d", client->host, client->port);
    }
}

void mqtt_client_log_callback(struct mosquitto *mosq, void *userdata, int level, const char *str) {
    mqtt_client_t *client = (mqtt_client_t *)userdata;
    
    // Only log errors and warnings to prevent spam
    if (level == MOSQ_LOG_ERR || level == MOSQ_LOG_WARNING) {
        char error_msg[512];
        snprintf(error_msg, sizeof(error_msg), "libmosquitto: %s", str);
        mqtt_client_log_error_rate_limited(client, error_msg);
    }
}

#endif // ENABLE_MQTT

bool mqtt_client_init(mqtt_client_t *client, const watchlist_config_t *config) {
    if (!client || !config) {
        return false;
    }
    
    // Initialize structure
    memset(client, 0, sizeof(mqtt_client_t));
    
    // Check if MQTT is enabled and configured
    if (!config->mqtt_host || strlen(config->mqtt_host) == 0) {
        client->enabled = false;
        return true; // Not an error, just disabled
    }
    
#ifndef ENABLE_MQTT
    log_with_timestamp("MQTT requested but not compiled with MQTT support");
    client->enabled = false;
    return true; // Not an error, just not available
#else
    
    client->enabled = true;
    
    // Copy configuration with error checking
    client->host = strdup(config->mqtt_host);
    if (!client->host) {
        time_t now = time(NULL);
        if (now - last_init_error_log >= MQTT_ERROR_LOG_INTERVAL) {
            log_with_timestamp("MQTT error: Failed to allocate memory for host string");
            last_init_error_log = now;
        }
        mqtt_client_cleanup(client);
        watchlist_disable_functionality("mqtt");
        return false;
    }
    
    client->port = config->mqtt_port;
    
    if (config->mqtt_username) {
        client->username = strdup(config->mqtt_username);
        if (!client->username) {
            time_t now = time(NULL);
            if (now - last_init_error_log >= MQTT_ERROR_LOG_INTERVAL) {
                log_with_timestamp("MQTT error: Failed to allocate memory for username string");
                last_init_error_log = now;
            }
            mqtt_client_cleanup(client);
            watchlist_disable_functionality("mqtt");
            return false;
        }
    }
    
    if (config->mqtt_password) {
        client->password = strdup(config->mqtt_password);
        if (!client->password) {
            time_t now = time(NULL);
            if (now - last_init_error_log >= MQTT_ERROR_LOG_INTERVAL) {
                log_with_timestamp("MQTT error: Failed to allocate memory for password string");
                last_init_error_log = now;
            }
            mqtt_client_cleanup(client);
            watchlist_disable_functionality("mqtt");
            return false;
        }
    }
    
    client->topic = strdup(config->mqtt_topic);
    if (!client->topic) {
        time_t now = time(NULL);
        if (now - last_init_error_log >= MQTT_ERROR_LOG_INTERVAL) {
            log_with_timestamp("MQTT error: Failed to allocate memory for topic string");
            last_init_error_log = now;
        }
        mqtt_client_cleanup(client);
        watchlist_disable_functionality("mqtt");
        return false;
    }
    
    client->qos = config->mqtt_qos;
    client->retain = config->mqtt_retain;
    
    // Initialize reconnection parameters
    client->connected = false;
    client->reconnect_delay = MQTT_INITIAL_RECONNECT_DELAY;
    client->max_reconnect_delay = MQTT_MAX_RECONNECT_DELAY;
    client->last_error_log = 0;
    client->last_reconnect_attempt = 0;
    
    // Initialize mosquitto library (safe to call multiple times)
    int lib_init_result = mosquitto_lib_init();
    if (lib_init_result != MOSQ_ERR_SUCCESS) {
        time_t now = time(NULL);
        if (now - last_init_error_log >= MQTT_ERROR_LOG_INTERVAL) {
            log_with_timestamp("MQTT error: Failed to initialize mosquitto library: %s", 
                              mosquitto_strerror(lib_init_result));
            last_init_error_log = now;
        }
        mqtt_client_cleanup(client);
        watchlist_disable_functionality("mqtt");
        return false;
    }
    
    // Create mosquitto client instance
    client->mosq = mosquitto_new(NULL, true, client);
    if (!client->mosq) {
        time_t now = time(NULL);
        if (now - last_init_error_log >= MQTT_ERROR_LOG_INTERVAL) {
            log_with_timestamp("MQTT error: Failed to create mosquitto client instance");
            last_init_error_log = now;
        }
        mqtt_client_cleanup(client);
        watchlist_disable_functionality("mqtt");
        return false;
    }
    
    // Set callbacks
    mosquitto_connect_callback_set(client->mosq, mqtt_client_connect_callback);
    mosquitto_disconnect_callback_set(client->mosq, mqtt_client_disconnect_callback);
    mosquitto_log_callback_set(client->mosq, mqtt_client_log_callback);
    
    // Set authentication if provided
    if (client->username && client->password) {
        int rc = mosquitto_username_pw_set(client->mosq, client->username, client->password);
        if (rc != MOSQ_ERR_SUCCESS) {
            time_t now = time(NULL);
            if (now - last_init_error_log >= MQTT_ERROR_LOG_INTERVAL) {
                log_with_timestamp("MQTT error: Failed to set credentials: %s", mosquitto_strerror(rc));
                last_init_error_log = now;
            }
            mqtt_client_cleanup(client);
            watchlist_disable_functionality("mqtt");
            return false;
        }
    }
    
    // Log successful initialization
    log_with_timestamp("MQTT client initialized: %s:%d topic=%s qos=%d retain=%s", 
                      client->host, client->port, client->topic, client->qos, 
                      client->retain ? "true" : "false");
    
    return true;
    
#endif // ENABLE_MQTT
}

bool mqtt_client_connect(mqtt_client_t *client) {
    if (!client || !client->enabled) {
        return false;
    }
    
#ifndef ENABLE_MQTT
    return false;
#else
    
    if (client->connected) {
        return true;
    }
    
    if (!client->mosq) {
        return false;
    }
    
    int rc = mosquitto_connect_async(client->mosq, client->host, client->port, 60);
    if (rc != MOSQ_ERR_SUCCESS) {
        char error_msg[256];
        snprintf(error_msg, sizeof(error_msg), "Failed to initiate connection: %s", mosquitto_strerror(rc));
        mqtt_client_log_error_rate_limited(client, error_msg);
        return false;
    }
    
    // Start the network loop
    rc = mosquitto_loop_start(client->mosq);
    if (rc != MOSQ_ERR_SUCCESS) {
        char error_msg[256];
        snprintf(error_msg, sizeof(error_msg), "Failed to start network loop: %s", mosquitto_strerror(rc));
        mqtt_client_log_error_rate_limited(client, error_msg);
        return false;
    }
    
    return true;
    
#endif // ENABLE_MQTT
}

bool mqtt_client_publish_alert(mqtt_client_t *client, const char *json_payload) {
    if (!client || !client->enabled || !json_payload) {
        return false;
    }
    
    // Check if MQTT functionality is globally disabled
    if (!watchlist_mqtt_is_enabled()) {
        return false;
    }
    
#ifndef ENABLE_MQTT
    return false;
#else
    
    if (!client->connected || !client->mosq) {
        // Don't log this as an error - it's normal during connection attempts
        return false;
    }
    
    // Validate payload size
    int payload_len = strlen(json_payload);
    if (payload_len == 0) {
        time_t now = time(NULL);
        if (now - last_publish_error_log >= MQTT_ERROR_LOG_INTERVAL) {
            log_with_timestamp("MQTT error: Empty JSON payload provided for publishing");
            last_publish_error_log = now;
        }
        return false;
    }
    
    if (payload_len > 8192) { // Reasonable limit for MQTT messages
        time_t now = time(NULL);
        if (now - last_publish_error_log >= MQTT_ERROR_LOG_INTERVAL) {
            log_with_timestamp("MQTT error: JSON payload too large (%d bytes, max 8192)", payload_len);
            last_publish_error_log = now;
        }
        return false;
    }
    
    int rc = mosquitto_publish(client->mosq, NULL, client->topic, payload_len, 
                              json_payload, client->qos, client->retain);
    
    if (rc != MOSQ_ERR_SUCCESS) {
        time_t now = time(NULL);
        if (now - last_publish_error_log >= MQTT_ERROR_LOG_INTERVAL) {
            log_with_timestamp("MQTT error: Failed to publish message: %s", mosquitto_strerror(rc));
            last_publish_error_log = now;
        }
        
        // If we get certain errors, consider disabling MQTT temporarily
        if (rc == MOSQ_ERR_NO_CONN || rc == MOSQ_ERR_CONN_LOST) {
            client->connected = false;
        } else if (rc == MOSQ_ERR_NOMEM) {
            // Memory error - this is serious
            watchlist_disable_functionality("mqtt");
        }
        
        return false;
    }
    
    return true;
    
#endif // ENABLE_MQTT
}

void mqtt_client_process(mqtt_client_t *client) {
    if (!client || !client->enabled) {
        return;
    }
    
#ifdef ENABLE_MQTT
    // Handle reconnection if needed
    if (!client->connected && client->mosq) {
        mqtt_client_schedule_reconnect(client);
    }
#endif
}

void mqtt_client_cleanup(mqtt_client_t *client) {
    if (!client) {
        return;
    }
    
#ifdef ENABLE_MQTT
    if (client->mosq) {
        mosquitto_loop_stop(client->mosq, false);
        mosquitto_disconnect(client->mosq);
        mosquitto_destroy(client->mosq);
        client->mosq = NULL;
    }
#endif
    
    sfree(client->host);
    sfree(client->username);
    sfree(client->password);
    sfree(client->topic);
    
    memset(client, 0, sizeof(mqtt_client_t));
}