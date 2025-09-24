// Part of readsb, a Mode-S/ADSB/TIS message decoder.
//
// alert_publisher.h: alert publisher with cooldown tracking for watchlist MQTT alerting
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

#ifndef ALERT_PUBLISHER_H
#define ALERT_PUBLISHER_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include "mqtt_client.h"

// Forward declarations
struct aircraft;
struct watchlist_config;

// Cooldown tracking entry
typedef struct {
    uint32_t icao;
    time_t last_published;
} alert_cooldown_t;

// Alert publisher structure
typedef struct {
    // Cooldown tracking hash table
    uint32_t cooldown_hash_bits;
    uint32_t cooldown_hash_buckets;
    alert_cooldown_t *cooldown_table;
    uint32_t cooldown_occupied;
    
    // Configuration
    int cooldown_seconds;
    
    // Current cycle tracking to prevent duplicates within same update cycle
    time_t current_cycle_time;
    uint32_t cycle_hash_bits;
    uint32_t cycle_hash_buckets;
    uint32_t *current_cycle_published;  // Hash set of ICAOs published this cycle
    uint32_t cycle_occupied;
    
    // Statistics
    uint32_t total_alerts_published;
    uint32_t alerts_suppressed_cooldown;
    uint32_t alerts_suppressed_duplicate;
} alert_publisher_t;

// Hash table constants
#define ALERT_COOLDOWN_EMPTY 0xFFFFFFFF
#define ALERT_COOLDOWN_MIN_BITS 8
#define ALERT_COOLDOWN_MAX_BITS 16

#define ALERT_CYCLE_EMPTY 0xFFFFFFFF
#define ALERT_CYCLE_MIN_BITS 6
#define ALERT_CYCLE_MAX_BITS 12

// JSON buffer size for alert messages
#define ALERT_JSON_BUFFER_SIZE 1024

// Function declarations
bool alert_publisher_init(alert_publisher_t *publisher, int cooldown_seconds);
void alert_publisher_cleanup(alert_publisher_t *publisher);

// Main processing function
void alert_publisher_check_aircraft(alert_publisher_t *publisher, 
                                   struct watchlist_config *watchlist,
                                   mqtt_client_t *mqtt_client);

// Core alert logic
bool alert_publisher_should_publish(alert_publisher_t *publisher, uint32_t icao, time_t now);
char* alert_publisher_create_json(const struct aircraft *a, time_t now);
bool alert_publisher_publish_alert(alert_publisher_t *publisher, 
                                  mqtt_client_t *mqtt_client,
                                  const struct aircraft *a, 
                                  time_t now);

// Cycle management
void alert_publisher_start_cycle(alert_publisher_t *publisher, time_t now);
void alert_publisher_end_cycle(alert_publisher_t *publisher);

// Internal hash table management
void alert_publisher_resize_cooldown_table(alert_publisher_t *publisher, uint32_t new_bits);
void alert_publisher_resize_cycle_table(alert_publisher_t *publisher, uint32_t new_bits);
void alert_publisher_add_cooldown(alert_publisher_t *publisher, uint32_t icao, time_t timestamp);
bool alert_publisher_add_to_cycle(alert_publisher_t *publisher, uint32_t icao);

// Utility functions
const char* alert_publisher_get_source_string(const struct aircraft *a);
bool alert_publisher_is_valid_field(double value);
bool alert_publisher_is_valid_int_field(int32_t value);

// Observability functions
void alert_publisher_log_statistics(alert_publisher_t *publisher);

#endif // ALERT_PUBLISHER_H