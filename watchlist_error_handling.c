// Part of readsb, a Mode-S/ADSB/TIS message decoder.
//
// watchlist_error_handling.c: Enhanced error handling and graceful degradation for watchlist MQTT alerting
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <stdarg.h>

// Define _GNU_SOURCE for strdup
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

// Forward declarations for readsb integration
void log_with_timestamp(const char *format, ...) __attribute__ ((format(printf, 1, 2)));

// Include watchlist headers
#include "watchlist_error_handling.h"

#ifndef TEST_BUILD
#include "watchlist.h"
#include "alert_publisher.h"
#include "mqtt_client.h"
#endif

// Constants for test build
#ifdef TEST_BUILD
#define WATCHLIST_MIN_BITS 8
#define ALERT_COOLDOWN_MIN_BITS 8
#define ALERT_CYCLE_MIN_BITS 6
#define ALERT_COOLDOWN_EMPTY 0xFFFFFFFF

// Simple strdup implementation for test build
char *strdup(const char *s) {
    size_t len = strlen(s) + 1;
    char *dup = malloc(len);
    if (dup) {
        memcpy(dup, s, len);
    }
    return dup;
}

// Stub cleanup functions for test build
void watchlist_cleanup(watchlist_config_t *config) {
    watchlist_cleanup_safe(config);
}

void alert_publisher_cleanup(alert_publisher_t *publisher) {
    alert_publisher_cleanup_safe(publisher);
}

// Alert cooldown structure for test build
typedef struct {
    uint32_t icao;
    time_t last_published;
} alert_cooldown_t;
#endif

// Global error state tracking for graceful degradation
static struct {
    bool watchlist_enabled;
    bool mqtt_enabled;
    bool alert_publisher_enabled;
    time_t last_critical_error;
    uint32_t consecutive_failures;
    uint32_t total_errors;
    char last_error_message[512];
} watchlist_error_state = {
    .watchlist_enabled = true,
    .mqtt_enabled = true,
    .alert_publisher_enabled = true,
    .last_critical_error = 0,
    .consecutive_failures = 0,
    .total_errors = 0,
    .last_error_message = {0}
};

// Error recovery thresholds
#define MAX_CONSECUTIVE_FAILURES 5
#define CRITICAL_ERROR_COOLDOWN 300  // 5 minutes
#define ERROR_LOG_INTERVAL 30        // 30 seconds

// Enhanced memory allocation with fallback strategies
void* watchlist_safe_malloc(size_t size, const char* context) {
    void* ptr = malloc(size);
    if (!ptr) {
        time_t now = time(NULL);
        if (now - watchlist_error_state.last_critical_error >= ERROR_LOG_INTERVAL) {
            log_with_timestamp("Watchlist critical error: Memory allocation failed for %s (%zu bytes)", 
                              context, size);
            watchlist_error_state.last_critical_error = now;
        }
        watchlist_error_state.consecutive_failures++;
        watchlist_error_state.total_errors++;
        
        // If we've had too many consecutive failures, disable watchlist functionality
        if (watchlist_error_state.consecutive_failures >= MAX_CONSECUTIVE_FAILURES) {
            log_with_timestamp("Watchlist error: Too many consecutive failures (%u), disabling watchlist functionality", 
                              watchlist_error_state.consecutive_failures);
            watchlist_error_state.watchlist_enabled = false;
        }
        return NULL;
    }
    
    // Reset consecutive failures on successful allocation
    if (watchlist_error_state.consecutive_failures > 0) {
        watchlist_error_state.consecutive_failures = 0;
    }
    
    return ptr;
}

// Enhanced error logging with rate limiting and context
void watchlist_log_error(const char* component, const char* operation, const char* error_msg) {
    time_t now = time(NULL);
    
    // Rate limit identical error messages
    if (strcmp(watchlist_error_state.last_error_message, error_msg) == 0 &&
        now - watchlist_error_state.last_critical_error < ERROR_LOG_INTERVAL) {
        return;
    }
    
    log_with_timestamp("Watchlist %s error in %s: %s", component, operation, error_msg);
    
    strncpy(watchlist_error_state.last_error_message, error_msg, 
            sizeof(watchlist_error_state.last_error_message) - 1);
    watchlist_error_state.last_error_message[sizeof(watchlist_error_state.last_error_message) - 1] = '\0';
    watchlist_error_state.last_critical_error = now;
    watchlist_error_state.total_errors++;
}

// Check if watchlist functionality should be enabled
bool watchlist_is_enabled(void) {
    return watchlist_error_state.watchlist_enabled;
}

// Check if MQTT functionality should be enabled
bool watchlist_mqtt_is_enabled(void) {
    return watchlist_error_state.mqtt_enabled;
}

// Check if alert publisher functionality should be enabled
bool watchlist_alert_publisher_is_enabled(void) {
    return watchlist_error_state.alert_publisher_enabled;
}

// Disable specific functionality after critical errors
void watchlist_disable_functionality(const char* component) {
    if (strcmp(component, "watchlist") == 0) {
        watchlist_error_state.watchlist_enabled = false;
        log_with_timestamp("Watchlist functionality disabled due to critical errors");
    } else if (strcmp(component, "mqtt") == 0) {
        watchlist_error_state.mqtt_enabled = false;
        log_with_timestamp("MQTT functionality disabled due to critical errors");
    } else if (strcmp(component, "alert_publisher") == 0) {
        watchlist_error_state.alert_publisher_enabled = false;
        log_with_timestamp("Alert publisher functionality disabled due to critical errors");
    }
}

// Attempt to re-enable functionality after cooldown period
void watchlist_try_recovery(void) {
    time_t now = time(NULL);
    
    if (now - watchlist_error_state.last_critical_error >= CRITICAL_ERROR_COOLDOWN) {
        if (!watchlist_error_state.watchlist_enabled || 
            !watchlist_error_state.mqtt_enabled || 
            !watchlist_error_state.alert_publisher_enabled) {
            
            log_with_timestamp("Watchlist attempting recovery after %d second cooldown", 
                              CRITICAL_ERROR_COOLDOWN);
            
            // Reset error counters
            watchlist_error_state.consecutive_failures = 0;
            
            // Re-enable functionality (will be disabled again if errors persist)
            watchlist_error_state.watchlist_enabled = true;
            watchlist_error_state.mqtt_enabled = true;
            watchlist_error_state.alert_publisher_enabled = true;
        }
    }
}

// Get error statistics for observability
void watchlist_get_error_stats(uint32_t* total_errors, uint32_t* consecutive_failures, 
                              time_t* last_error_time, bool* components_enabled) {
    if (total_errors) *total_errors = watchlist_error_state.total_errors;
    if (consecutive_failures) *consecutive_failures = watchlist_error_state.consecutive_failures;
    if (last_error_time) *last_error_time = watchlist_error_state.last_critical_error;
    if (components_enabled) {
        components_enabled[0] = watchlist_error_state.watchlist_enabled;
        components_enabled[1] = watchlist_error_state.mqtt_enabled;
        components_enabled[2] = watchlist_error_state.alert_publisher_enabled;
    }
}

// Enhanced watchlist initialization with comprehensive error handling
bool watchlist_init_safe(watchlist_config_t *config) {
    if (!config) {
        watchlist_log_error("watchlist", "init", "NULL configuration provided");
        return false;
    }
    
    if (!watchlist_is_enabled()) {
        log_with_timestamp("Watchlist initialization skipped - functionality disabled due to previous errors");
        return false;
    }
    
    // Clear configuration
    memset(config, 0, sizeof(watchlist_config_t));
    
    // Set default values with error checking
    config->mqtt_topic = strdup("meshtastic/adsb/watch");
    if (!config->mqtt_topic) {
        watchlist_log_error("watchlist", "init", "Failed to allocate memory for default MQTT topic");
        watchlist_cleanup(config);
        return false;
    }
    
    config->mqtt_port = 1883;
    config->mqtt_qos = 0;
    config->mqtt_retain = false;
    config->cooldown_seconds = 300;
    config->last_mtime = 0;
    
    // Initialize hash table with error handling
    config->hash_bits = WATCHLIST_MIN_BITS;
    config->hash_buckets = 1ULL << config->hash_bits;
    config->occupied = 0;
    
    size_t table_size = config->hash_buckets * sizeof(uint32_t);
    config->icao_table = watchlist_safe_malloc(table_size, "ICAO hash table");
    if (!config->icao_table) {
        watchlist_cleanup(config);
        return false;
    }
    
    // Initialize all entries to empty
    memset(config->icao_table, 0xFF, table_size);
    
    log_with_timestamp("Watchlist initialized successfully with %u buckets", config->hash_buckets);
    return true;
}

// Enhanced MQTT client initialization with comprehensive error handling
bool mqtt_client_init_safe(mqtt_client_t *client, const watchlist_config_t *config) {
    if (!client || !config) {
        watchlist_log_error("mqtt", "init", "NULL client or configuration provided");
        return false;
    }
    
    if (!watchlist_mqtt_is_enabled()) {
        log_with_timestamp("MQTT initialization skipped - functionality disabled due to previous errors");
        client->enabled = false;
        return true;
    }
    
    // Initialize structure
    memset(client, 0, sizeof(mqtt_client_t));
    
    // Check if MQTT is configured
    if (!config->mqtt_host || strlen(config->mqtt_host) == 0) {
        client->enabled = false;
        log_with_timestamp("MQTT not configured - host not specified");
        return true;
    }
    
#ifndef ENABLE_MQTT
    watchlist_log_error("mqtt", "init", "MQTT requested but not compiled with MQTT support");
    client->enabled = false;
    return true;
#else
    
    client->enabled = true;
    
    // Copy configuration with error checking
    client->host = strdup(config->mqtt_host);
    if (!client->host) {
        watchlist_log_error("mqtt", "init", "Failed to allocate memory for MQTT host");
        mqtt_client_cleanup(client);
        return false;
    }
    
    client->port = config->mqtt_port;
    
    if (config->mqtt_username) {
        client->username = strdup(config->mqtt_username);
        if (!client->username) {
            watchlist_log_error("mqtt", "init", "Failed to allocate memory for MQTT username");
            mqtt_client_cleanup(client);
            return false;
        }
    }
    
    if (config->mqtt_password) {
        client->password = strdup(config->mqtt_password);
        if (!client->password) {
            watchlist_log_error("mqtt", "init", "Failed to allocate memory for MQTT password");
            mqtt_client_cleanup(client);
            return false;
        }
    }
    
    client->topic = strdup(config->mqtt_topic);
    if (!client->topic) {
        watchlist_log_error("mqtt", "init", "Failed to allocate memory for MQTT topic");
        mqtt_client_cleanup(client);
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
        watchlist_log_error("mqtt", "init", "Failed to initialize mosquitto library");
        mqtt_client_cleanup(client);
        return false;
    }
    
    // Create mosquitto client instance
    client->mosq = mosquitto_new(NULL, true, client);
    if (!client->mosq) {
        watchlist_log_error("mqtt", "init", "Failed to create mosquitto client instance");
        mqtt_client_cleanup(client);
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
            char error_msg[256];
            snprintf(error_msg, sizeof(error_msg), "Failed to set MQTT credentials: %s", 
                    mosquitto_strerror(rc));
            watchlist_log_error("mqtt", "init", error_msg);
            mqtt_client_cleanup(client);
            return false;
        }
    }
    
    log_with_timestamp("MQTT client initialized successfully: %s:%d topic=%s qos=%d", 
                      client->host, client->port, client->topic, client->qos);
    
    return true;
    
#endif // ENABLE_MQTT
}

// Enhanced alert publisher initialization with comprehensive error handling
bool alert_publisher_init_safe(alert_publisher_t *publisher, int cooldown_seconds) {
    if (!publisher) {
        watchlist_log_error("alert_publisher", "init", "NULL publisher provided");
        return false;
    }
    
    if (!watchlist_alert_publisher_is_enabled()) {
        log_with_timestamp("Alert publisher initialization skipped - functionality disabled due to previous errors");
        return false;
    }
    
    memset(publisher, 0, sizeof(alert_publisher_t));
    
    // Set configuration
    publisher->cooldown_seconds = cooldown_seconds > 0 ? cooldown_seconds : 300;
    
    // Initialize cooldown hash table
    publisher->cooldown_hash_bits = ALERT_COOLDOWN_MIN_BITS;
    publisher->cooldown_hash_buckets = 1ULL << publisher->cooldown_hash_bits;
    publisher->cooldown_occupied = 0;
    
    size_t cooldown_table_size = publisher->cooldown_hash_buckets * sizeof(alert_cooldown_t);
    publisher->cooldown_table = watchlist_safe_malloc(cooldown_table_size, "alert cooldown table");
    if (!publisher->cooldown_table) {
        alert_publisher_cleanup(publisher);
        return false;
    }
    
    // Initialize all cooldown entries to empty
    alert_cooldown_t *cooldown_table = (alert_cooldown_t *)publisher->cooldown_table;
    for (uint32_t i = 0; i < publisher->cooldown_hash_buckets; i++) {
        cooldown_table[i].icao = ALERT_COOLDOWN_EMPTY;
        cooldown_table[i].last_published = 0;
    }
    
    // Initialize cycle hash table
    publisher->cycle_hash_bits = ALERT_CYCLE_MIN_BITS;
    publisher->cycle_hash_buckets = 1ULL << publisher->cycle_hash_bits;
    publisher->cycle_occupied = 0;
    
    size_t cycle_table_size = publisher->cycle_hash_buckets * sizeof(uint32_t);
    publisher->current_cycle_published = watchlist_safe_malloc(cycle_table_size, "alert cycle table");
    if (!publisher->current_cycle_published) {
        alert_publisher_cleanup(publisher);
        return false;
    }
    
    // Initialize all cycle entries to empty
    memset(publisher->current_cycle_published, 0xFF, cycle_table_size);
    
    // Initialize statistics
    publisher->total_alerts_published = 0;
    publisher->alerts_suppressed_cooldown = 0;
    publisher->alerts_suppressed_duplicate = 0;
    publisher->current_cycle_time = 0;
    
    log_with_timestamp("Alert publisher initialized successfully: cooldown=%ds, tables=%u/%u buckets", 
                      cooldown_seconds, publisher->cooldown_hash_buckets, publisher->cycle_hash_buckets);
    
    return true;
}

// Safe wrapper for watchlist operations that handles errors gracefully
bool watchlist_operation_safe(const char* operation, bool (*operation_func)(void*), void* context) {
    if (!watchlist_is_enabled()) {
        return false;
    }
    
    // Attempt recovery if we're in cooldown period
    watchlist_try_recovery();
    
    if (!watchlist_is_enabled()) {
        return false;
    }
    
    bool result = operation_func(context);
    
    if (!result) {
        watchlist_error_state.consecutive_failures++;
        if (watchlist_error_state.consecutive_failures >= MAX_CONSECUTIVE_FAILURES) {
            watchlist_disable_functionality("watchlist");
        }
    } else {
        watchlist_error_state.consecutive_failures = 0;
    }
    
    return result;
}

// Enhanced cleanup with comprehensive resource management
void watchlist_cleanup_safe(watchlist_config_t *config) {
    if (!config) {
        return;
    }
    
    // Free all allocated strings safely
    if (config->file_path) {
        free(config->file_path);
        config->file_path = NULL;
    }
    
    if (config->mqtt_host) {
        free(config->mqtt_host);
        config->mqtt_host = NULL;
    }
    
    if (config->mqtt_username) {
        free(config->mqtt_username);
        config->mqtt_username = NULL;
    }
    
    if (config->mqtt_password) {
        free(config->mqtt_password);
        config->mqtt_password = NULL;
    }
    
    if (config->mqtt_topic) {
        free(config->mqtt_topic);
        config->mqtt_topic = NULL;
    }
    
    if (config->icao_table) {
        free(config->icao_table);
        config->icao_table = NULL;
    }
    
    // Clear the entire structure
    memset(config, 0, sizeof(watchlist_config_t));
}

// Enhanced MQTT cleanup with comprehensive resource management
void mqtt_client_cleanup_safe(mqtt_client_t *client) {
    if (!client) {
        return;
    }
    
#ifdef ENABLE_MQTT
    if (client->mosq) {
        // Stop the network loop first
        mosquitto_loop_stop(client->mosq, false);
        
        // Disconnect gracefully
        mosquitto_disconnect(client->mosq);
        
        // Destroy the client instance
        mosquitto_destroy(client->mosq);
        client->mosq = NULL;
    }
#endif
    
    // Free all allocated strings safely
    if (client->host) {
        free(client->host);
        client->host = NULL;
    }
    
    if (client->username) {
        free(client->username);
        client->username = NULL;
    }
    
    if (client->password) {
        free(client->password);
        client->password = NULL;
    }
    
    if (client->topic) {
        free(client->topic);
        client->topic = NULL;
    }
    
    // Clear the entire structure
    memset(client, 0, sizeof(mqtt_client_t));
}

// Enhanced alert publisher cleanup with comprehensive resource management
void alert_publisher_cleanup_safe(alert_publisher_t *publisher) {
    if (!publisher) {
        return;
    }
    
    if (publisher->cooldown_table) {
        free(publisher->cooldown_table);
        publisher->cooldown_table = NULL;
    }
    
    if (publisher->current_cycle_published) {
        free(publisher->current_cycle_published);
        publisher->current_cycle_published = NULL;
    }
    
    // Clear the entire structure
    memset(publisher, 0, sizeof(alert_publisher_t));
}

// Log comprehensive error statistics for observability
void watchlist_log_error_statistics(void) {
    uint32_t total_errors, consecutive_failures;
    time_t last_error_time;
    bool components_enabled[3];
    
    watchlist_get_error_stats(&total_errors, &consecutive_failures, &last_error_time, components_enabled);
    
    log_with_timestamp("Watchlist error statistics: total=%u, consecutive=%u, last_error=%ld seconds ago, enabled=[watchlist:%s, mqtt:%s, alerts:%s]",
                      total_errors, consecutive_failures, 
                      last_error_time > 0 ? time(NULL) - last_error_time : -1,
                      components_enabled[0] ? "yes" : "no",
                      components_enabled[1] ? "yes" : "no", 
                      components_enabled[2] ? "yes" : "no");
}