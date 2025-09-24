// Part of readsb, a Mode-S/ADSB/TIS message decoder.
//
// watchlist_error_handling.h: Enhanced error handling and graceful degradation for watchlist MQTT alerting
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

#ifndef WATCHLIST_ERROR_HANDLING_H
#define WATCHLIST_ERROR_HANDLING_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>

// Forward declarations and type definitions for standalone compilation
#ifndef TEST_BUILD
struct watchlist_config;
struct mqtt_client;
struct alert_publisher;
typedef struct watchlist_config watchlist_config_t;
typedef struct mqtt_client mqtt_client_t;
typedef struct alert_publisher alert_publisher_t;
#else
// Minimal type definitions for testing
typedef struct {
    char *file_path;
    char *mqtt_host;
    int mqtt_port;
    char *mqtt_username;
    char *mqtt_password;
    char *mqtt_topic;
    int mqtt_qos;
    bool mqtt_retain;
    int cooldown_seconds;
    time_t last_mtime;
    uint32_t hash_bits;
    uint32_t hash_buckets;
    uint32_t *icao_table;
    uint32_t occupied;
} watchlist_config_t;

typedef struct {
    char *host;
    int port;
    char *username;
    char *password;
    char *topic;
    int qos;
    bool retain;
    bool connected;
    bool enabled;
    time_t last_error_log;
    int reconnect_delay;
    int max_reconnect_delay;
    time_t last_reconnect_attempt;
    void *mosq; // mosquitto client pointer
} mqtt_client_t;

typedef struct {
    uint32_t cooldown_hash_bits;
    uint32_t cooldown_hash_buckets;
    void *cooldown_table;
    uint32_t cooldown_occupied;
    int cooldown_seconds;
    time_t current_cycle_time;
    uint32_t cycle_hash_bits;
    uint32_t cycle_hash_buckets;
    uint32_t *current_cycle_published;
    uint32_t cycle_occupied;
    uint32_t total_alerts_published;
    uint32_t alerts_suppressed_cooldown;
    uint32_t alerts_suppressed_duplicate;
} alert_publisher_t;
#endif

// Enhanced memory allocation with error handling
void* watchlist_safe_malloc(size_t size, const char* context);

// Enhanced error logging with rate limiting
void watchlist_log_error(const char* component, const char* operation, const char* error_msg);

// Component status checking functions
bool watchlist_is_enabled(void);
bool watchlist_mqtt_is_enabled(void);
bool watchlist_alert_publisher_is_enabled(void);

// Functionality control functions
void watchlist_disable_functionality(const char* component);
void watchlist_try_recovery(void);

// Error statistics and observability
void watchlist_get_error_stats(uint32_t* total_errors, uint32_t* consecutive_failures, 
                              time_t* last_error_time, bool* components_enabled);
void watchlist_log_error_statistics(void);

// Enhanced initialization functions with comprehensive error handling
bool watchlist_init_safe(watchlist_config_t *config);
bool mqtt_client_init_safe(mqtt_client_t *client, const watchlist_config_t *config);
bool alert_publisher_init_safe(alert_publisher_t *publisher, int cooldown_seconds);

// Safe operation wrapper
bool watchlist_operation_safe(const char* operation, bool (*operation_func)(void*), void* context);

// Enhanced cleanup functions with comprehensive resource management
void watchlist_cleanup_safe(watchlist_config_t *config);
void mqtt_client_cleanup_safe(mqtt_client_t *client);
void alert_publisher_cleanup_safe(alert_publisher_t *publisher);

#endif // WATCHLIST_ERROR_HANDLING_H