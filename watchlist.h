// Part of readsb, a Mode-S/ADSB/TIS message decoder.
//
// watchlist.h: watchlist functionality for MQTT alerting
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

#ifndef WATCHLIST_H
#define WATCHLIST_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>

// Watchlist configuration structure
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
    
    // Hash table for ICAO addresses
    uint32_t hash_bits;
    uint32_t hash_buckets;
    uint32_t *icao_table;
    uint32_t occupied;
} watchlist_config_t;

// Hash table constants
#define WATCHLIST_EMPTY 0xFFFFFFFF
#define WATCHLIST_MIN_BITS 8
#define WATCHLIST_MAX_BITS 20

// Function declarations
bool watchlist_init(watchlist_config_t *config);
bool watchlist_load(watchlist_config_t *config);
bool watchlist_needs_reload(watchlist_config_t *config);
bool watchlist_reload_safe(watchlist_config_t *config);
bool watchlist_contains(watchlist_config_t *config, uint32_t icao);
void watchlist_cleanup(watchlist_config_t *config);
uint32_t watchlist_normalize_icao(const char *icao_str);

// Internal functions
void watchlist_resize(watchlist_config_t *config, uint32_t new_bits);
void watchlist_add_icao(watchlist_config_t *config, uint32_t icao);

#endif // WATCHLIST_H