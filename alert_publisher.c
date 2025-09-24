// Part of readsb, a Mode-S/ADSB/TIS message decoder.
//
// alert_publisher.c: alert publisher with cooldown tracking for watchlist MQTT alerting
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

#ifndef TEST_BUILD
#include "readsb.h"
#include "alert_publisher.h"
#include "watchlist.h"
#include "mqtt_client.h"
#include "aircraft.h"
#include "util.h"
#else
// Test build - types defined in test file
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>

// Use readsb's memory management and utility functions
#ifndef sfree
#define sfree(x) do { if (x) { free(x); (x) = NULL; } } while(0)
#endif

// Include enhanced error handling
#include "watchlist_error_handling.h"

// Hash function for ICAO addresses using the same pattern as existing code

// Rate limiting for error logs
static time_t last_memory_error_log = 0;
static time_t last_json_error_log = 0;
static time_t last_mqtt_error_log = 0;
static time_t last_hash_error_log = 0;
static time_t last_validation_error_log = 0;
#define ERROR_LOG_INTERVAL 30
static uint32_t alert_hash(uint32_t addr, uint32_t bits) {
    return addrHash(addr, bits);
}

// Initialize alert publisher with default values
bool alert_publisher_init(alert_publisher_t *publisher, int cooldown_seconds) {
    if (!publisher) {
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
    publisher->cooldown_table = cmalloc(cooldown_table_size);
    if (!publisher->cooldown_table) {
        time_t now = time(NULL);
        if (now - last_memory_error_log >= ERROR_LOG_INTERVAL) {
            log_with_timestamp("Alert publisher error: Failed to allocate cooldown table memory (%zu bytes)", cooldown_table_size);
            last_memory_error_log = now;
        }
        alert_publisher_cleanup(publisher);
        watchlist_disable_functionality("alert_publisher");
        return false;
    }
    
    // Initialize all cooldown entries to empty
    for (uint32_t i = 0; i < publisher->cooldown_hash_buckets; i++) {
        publisher->cooldown_table[i].icao = ALERT_COOLDOWN_EMPTY;
        publisher->cooldown_table[i].last_published = 0;
    }    

    // Initialize cycle hash table
    publisher->cycle_hash_bits = ALERT_CYCLE_MIN_BITS;
    publisher->cycle_hash_buckets = 1ULL << publisher->cycle_hash_bits;
    publisher->cycle_occupied = 0;
    
    size_t cycle_table_size = publisher->cycle_hash_buckets * sizeof(uint32_t);
    publisher->current_cycle_published = cmalloc(cycle_table_size);
    if (!publisher->current_cycle_published) {
        time_t now = time(NULL);
        if (now - last_memory_error_log >= ERROR_LOG_INTERVAL) {
            log_with_timestamp("Alert publisher error: Failed to allocate cycle tracking table memory (%zu bytes)", cycle_table_size);
            last_memory_error_log = now;
        }
        alert_publisher_cleanup(publisher);
        watchlist_disable_functionality("alert_publisher");
        return false;
    }
    
    // Initialize all cycle entries to empty
    memset(publisher->current_cycle_published, 0xFF, cycle_table_size);
    
    // Initialize statistics
    publisher->total_alerts_published = 0;
    publisher->alerts_suppressed_cooldown = 0;
    publisher->alerts_suppressed_duplicate = 0;
    publisher->current_cycle_time = 0;
    
    // Log successful initialization
    log_with_timestamp("Alert publisher initialized: cooldown=%ds, cooldown_table=%u buckets, cycle_table=%u buckets", 
                      cooldown_seconds, publisher->cooldown_hash_buckets, publisher->cycle_hash_buckets);
    
    return true;
}

// Clean up alert publisher and free memory
void alert_publisher_cleanup(alert_publisher_t *publisher) {
    if (!publisher) {
        return;
    }
    
    sfree(publisher->cooldown_table);
    sfree(publisher->current_cycle_published);
    
    memset(publisher, 0, sizeof(alert_publisher_t));
}

// Resize cooldown hash table to accommodate more entries
void alert_publisher_resize_cooldown_table(alert_publisher_t *publisher, uint32_t new_bits) {
    if (!publisher || new_bits < ALERT_COOLDOWN_MIN_BITS || new_bits > ALERT_COOLDOWN_MAX_BITS) {
        return;
    }
    
    uint32_t old_buckets = publisher->cooldown_hash_buckets;
    alert_cooldown_t *old_table = publisher->cooldown_table;
    
    // Update configuration
    publisher->cooldown_hash_bits = new_bits;
    publisher->cooldown_hash_buckets = 1ULL << new_bits;
    publisher->cooldown_occupied = 0;
    
    // Allocate new table
    size_t new_size = publisher->cooldown_hash_buckets * sizeof(alert_cooldown_t);
    publisher->cooldown_table = cmalloc(new_size);
    if (!publisher->cooldown_table) {
        // Restore old configuration on failure
        publisher->cooldown_hash_bits = __builtin_ctz(old_buckets);
        publisher->cooldown_hash_buckets = old_buckets;
        publisher->cooldown_table = old_table;
        
        time_t now = time(NULL);
        if (now - last_hash_error_log >= ERROR_LOG_INTERVAL) {
            log_with_timestamp("Alert publisher error: Failed to resize cooldown table to %u buckets (%zu bytes), keeping current size", 
                              1ULL << new_bits, new_size);
            last_hash_error_log = now;
        }
        return;
    }
    
    // Initialize new table
    for (uint32_t i = 0; i < publisher->cooldown_hash_buckets; i++) {
        publisher->cooldown_table[i].icao = ALERT_COOLDOWN_EMPTY;
        publisher->cooldown_table[i].last_published = 0;
    }
    
    // Rehash existing entries
    for (uint32_t i = 0; i < old_buckets; i++) {
        if (old_table[i].icao != ALERT_COOLDOWN_EMPTY) {
            alert_publisher_add_cooldown(publisher, old_table[i].icao, old_table[i].last_published);
        }
    }
    
    // Free old table
    free(old_table);
}

// Resize cycle hash table to accommodate more entries
void alert_publisher_resize_cycle_table(alert_publisher_t *publisher, uint32_t new_bits) {
    if (!publisher || new_bits < ALERT_CYCLE_MIN_BITS || new_bits > ALERT_CYCLE_MAX_BITS) {
        return;
    }
    
    uint32_t old_buckets = publisher->cycle_hash_buckets;
    uint32_t *old_table = publisher->current_cycle_published;
    
    // Update configuration
    publisher->cycle_hash_bits = new_bits;
    publisher->cycle_hash_buckets = 1ULL << new_bits;
    publisher->cycle_occupied = 0;
    
    // Allocate new table
    size_t new_size = publisher->cycle_hash_buckets * sizeof(uint32_t);
    publisher->current_cycle_published = cmalloc(new_size);
    if (!publisher->current_cycle_published) {
        // Restore old configuration on failure
        publisher->cycle_hash_bits = __builtin_ctz(old_buckets);
        publisher->cycle_hash_buckets = old_buckets;
        publisher->current_cycle_published = old_table;
        
        time_t now = time(NULL);
        if (now - last_hash_error_log >= ERROR_LOG_INTERVAL) {
            log_with_timestamp("Alert publisher error: Failed to resize cycle table to %u buckets (%zu bytes), keeping current size", 
                              1ULL << new_bits, new_size);
            last_hash_error_log = now;
        }
        return;
    }
    
    // Initialize new table
    memset(publisher->current_cycle_published, 0xFF, new_size);
    
    // Rehash existing entries
    for (uint32_t i = 0; i < old_buckets; i++) {
        if (old_table[i] != ALERT_CYCLE_EMPTY) {
            alert_publisher_add_to_cycle(publisher, old_table[i]);
        }
    }
    
    // Free old table
    free(old_table);
}

// Add ICAO address to cooldown table with timestamp
void alert_publisher_add_cooldown(alert_publisher_t *publisher, uint32_t icao, time_t timestamp) {
    if (!publisher || !publisher->cooldown_table || icao == 0) {
        return;
    }
    
    uint32_t h, h0;
    h0 = h = alert_hash(icao, publisher->cooldown_hash_bits);
    
    // Linear probing to find empty slot or existing entry
    while (publisher->cooldown_table[h].icao != ALERT_COOLDOWN_EMPTY && 
           publisher->cooldown_table[h].icao != icao) {
        h = (h + 1) & (publisher->cooldown_hash_buckets - 1);
        if (h == h0) {
            // Table is full, resize if possible
            if (publisher->cooldown_hash_bits < ALERT_COOLDOWN_MAX_BITS) {
                alert_publisher_resize_cooldown_table(publisher, publisher->cooldown_hash_bits + 1);
                // Retry after resize
                alert_publisher_add_cooldown(publisher, icao, timestamp);
            }
            return;
        }
    }
    
    // Add new entry or update existing
    if (publisher->cooldown_table[h].icao == ALERT_COOLDOWN_EMPTY) {
        publisher->cooldown_occupied++;
    }
    publisher->cooldown_table[h].icao = icao;
    publisher->cooldown_table[h].last_published = timestamp;
    
    // Resize if table is getting full
    if (publisher->cooldown_occupied > publisher->cooldown_hash_buckets / 2 && 
        publisher->cooldown_hash_bits < ALERT_COOLDOWN_MAX_BITS) {
        alert_publisher_resize_cooldown_table(publisher, publisher->cooldown_hash_bits + 1);
    }
}

// Add ICAO address to current cycle table
bool alert_publisher_add_to_cycle(alert_publisher_t *publisher, uint32_t icao) {
    if (!publisher || !publisher->current_cycle_published || icao == 0) {
        return false;
    }
    
    uint32_t h, h0;
    h0 = h = alert_hash(icao, publisher->cycle_hash_bits);
    
    // Linear probing to find empty slot or existing entry
    while (publisher->current_cycle_published[h] != ALERT_CYCLE_EMPTY && 
           publisher->current_cycle_published[h] != icao) {
        h = (h + 1) & (publisher->cycle_hash_buckets - 1);
        if (h == h0) {
            // Table is full, resize if possible
            if (publisher->cycle_hash_bits < ALERT_CYCLE_MAX_BITS) {
                alert_publisher_resize_cycle_table(publisher, publisher->cycle_hash_bits + 1);
                // Retry after resize
                return alert_publisher_add_to_cycle(publisher, icao);
            }
            return false; // Table full and can't resize
        }
    }
    
    // Check if already exists
    if (publisher->current_cycle_published[h] == icao) {
        return false; // Already published this cycle
    }
    
    // Add new entry
    publisher->current_cycle_published[h] = icao;
    publisher->cycle_occupied++;
    
    // Resize if table is getting full
    if (publisher->cycle_occupied > publisher->cycle_hash_buckets / 2 && 
        publisher->cycle_hash_bits < ALERT_CYCLE_MAX_BITS) {
        alert_publisher_resize_cycle_table(publisher, publisher->cycle_hash_bits + 1);
    }
    
    return true;
}

// Start a new processing cycle
void alert_publisher_start_cycle(alert_publisher_t *publisher, time_t now) {
    if (!publisher) {
        return;
    }
    
    publisher->current_cycle_time = now;
    publisher->cycle_occupied = 0;
    
    // Clear cycle table
    if (publisher->current_cycle_published) {
        memset(publisher->current_cycle_published, 0xFF, 
               publisher->cycle_hash_buckets * sizeof(uint32_t));
    }
}

// End the current processing cycle
void alert_publisher_end_cycle(alert_publisher_t *publisher) {
    if (!publisher) {
        return;
    }
    
    publisher->current_cycle_time = 0;
}

// Check if an ICAO should be published based on cooldown logic
bool alert_publisher_should_publish(alert_publisher_t *publisher, uint32_t icao, time_t now) {
    if (!publisher || !publisher->cooldown_table || icao == 0) {
        return false;
    }
    
    // Check if already published this cycle
    if (publisher->current_cycle_published) {
        uint32_t h, h0;
        h0 = h = alert_hash(icao, publisher->cycle_hash_bits);
        
        while (publisher->current_cycle_published[h] != ALERT_CYCLE_EMPTY && 
               publisher->current_cycle_published[h] != icao) {
            h = (h + 1) & (publisher->cycle_hash_buckets - 1);
            if (h == h0) {
                break; // Full loop, not found
            }
        }
        
        if (publisher->current_cycle_published[h] == icao) {
            publisher->alerts_suppressed_duplicate++;
            return false; // Already published this cycle
        }
    }
    
    // Check cooldown period
    uint32_t h, h0;
    h0 = h = alert_hash(icao, publisher->cooldown_hash_bits);
    
    while (publisher->cooldown_table[h].icao != ALERT_COOLDOWN_EMPTY && 
           publisher->cooldown_table[h].icao != icao) {
        h = (h + 1) & (publisher->cooldown_hash_buckets - 1);
        if (h == h0) {
            break; // Full loop, not found
        }
    }
    
    if (publisher->cooldown_table[h].icao == icao) {
        // Found existing entry, check cooldown
        time_t elapsed = now - publisher->cooldown_table[h].last_published;
        if (elapsed < publisher->cooldown_seconds) {
            publisher->alerts_suppressed_cooldown++;
            return false; // Still in cooldown period
        }
    }
    
    return true; // OK to publish
}

// Utility function to check if a double value is valid (not NaN or infinite)
bool alert_publisher_is_valid_field(double value) {
    return !isnan(value) && !isinf(value);
}

// Utility function to check if an int32_t value is valid (not sentinel values)
bool alert_publisher_is_valid_int_field(int32_t value) {
    return value != INVALID_ALTITUDE && value != 0;
}

// Get source string for aircraft data
const char* alert_publisher_get_source_string(const struct aircraft *a) {
    if (!a) {
        return "unknown";
    }
    
    // Map addrtype to source string based on readsb conventions
    switch (a->addrtype) {
        case ADDR_ADSB_ICAO:
        case ADDR_ADSB_ICAO_NT:
            return "adsb_icao";
        case ADDR_ADSR_ICAO:
            return "adsr_icao";
        case ADDR_TISB_ICAO:
            return "tisb_icao";
        case ADDR_JAERO:
            return "jaero";
        case ADDR_MLAT:
            return "mlat";
        case ADDR_MODE_S:
            return "mode_s";
        case ADDR_ADSB_OTHER:
            return "adsb_other";
        case ADDR_ADSR_OTHER:
            return "adsr_other";
        case ADDR_TISB_TRACKFILE:
            return "tisb_trackfile";
        case ADDR_TISB_OTHER:
            return "tisb_other";
        case ADDR_MODE_A:
            return "mode_a";
        default:
            return "unknown";
    }
}

// Create compact JSON alert message for aircraft
char* alert_publisher_create_json(const struct aircraft *a, time_t now) {
    if (!a) {
        time_t current_time = time(NULL);
        if (current_time - last_validation_error_log >= ERROR_LOG_INTERVAL) {
            log_with_timestamp("Alert publisher error: NULL aircraft provided to JSON creation");
            last_validation_error_log = current_time;
        }
        return NULL;
    }
    
    // Validate ICAO address
    uint32_t icao = a->addr & 0xFFFFFF;
    if (icao == 0) {
        time_t current_time = time(NULL);
        if (current_time - last_validation_error_log >= ERROR_LOG_INTERVAL) {
            log_with_timestamp("Alert publisher error: Invalid ICAO address (0) in aircraft data");
            last_validation_error_log = current_time;
        }
        return NULL;
    }
    
    char *json_buffer = cmalloc(ALERT_JSON_BUFFER_SIZE);
    if (!json_buffer) {
        time_t now = time(NULL);
        if (now - last_memory_error_log >= ERROR_LOG_INTERVAL) {
            log_with_timestamp("Alert publisher error: Failed to allocate JSON buffer memory (%d bytes)", ALERT_JSON_BUFFER_SIZE);
            last_memory_error_log = now;
        }
        return NULL;
    }
    
    char *p = json_buffer;
    char *end = json_buffer + ALERT_JSON_BUFFER_SIZE - 1;
    
    // Start JSON object
    p += snprintf(p, end - p, "{");
    
    // ICAO address (always included) - use uppercase hex format
    p += snprintf(p, end - p, "\"icao\":\"%06X\"", a->addr & 0xFFFFFF);
    
    // Callsign (if valid and not empty)
    if (trackDataValid(&a->callsign_valid) && a->callsign[0] != '\0') {
        // Trim trailing spaces and validate printable characters
        char trimmed_callsign[17] = {0}; // callsign is 16 chars max
        int len = 0;
        for (int i = 0; i < 16 && a->callsign[i] != '\0'; i++) {
            if (a->callsign[i] >= 32 && a->callsign[i] <= 126) {
                trimmed_callsign[len++] = a->callsign[i];
            }
        }
        // Remove trailing spaces
        while (len > 0 && trimmed_callsign[len-1] == ' ') {
            len--;
        }
        trimmed_callsign[len] = '\0';
        
        if (len > 0) {
            p += snprintf(p, end - p, ",\"callsign\":\"%s\"", trimmed_callsign);
        }
    }
    
    // Barometric altitude (if valid) - check both validity and reasonable range
    if (trackDataValid(&a->baro_alt_valid) && a->baro_alt != INVALID_ALTITUDE) {
        p += snprintf(p, end - p, ",\"alt_baro\":%d", a->baro_alt);
    }
    
    // Ground speed (if valid and reasonable)
    if (trackDataValid(&a->gs_valid) && alert_publisher_is_valid_field(a->gs) && a->gs >= 0 && a->gs <= 2000) {
        p += snprintf(p, end - p, ",\"gs\":%.1f", a->gs);
    }
    
    // Position (if both lat and lon are valid and reliable)
    if (trackDataValid(&a->pos_reliable_valid) && 
        alert_publisher_is_valid_field(a->latReliable) && 
        alert_publisher_is_valid_field(a->lonReliable) &&
        a->latReliable >= -90.0 && a->latReliable <= 90.0 &&
        a->lonReliable >= -180.0 && a->lonReliable <= 180.0) {
        p += snprintf(p, end - p, ",\"lat\":%.6f,\"lon\":%.6f", a->latReliable, a->lonReliable);
    }
    
    // Track (if valid and in range)
    if (trackDataValid(&a->track_valid) && alert_publisher_is_valid_field(a->track) && 
        a->track >= 0 && a->track < 360) {
        p += snprintf(p, end - p, ",\"track\":%.1f", a->track);
    }
    
    // Time since last seen (always included) - convert from milliseconds
    int64_t now_millis = now * 1000LL;
    double seen_seconds = (now_millis - a->seen) / 1000.0;
    if (seen_seconds >= 0 && seen_seconds < 3600) { // Cap at 1 hour for sanity
        p += snprintf(p, end - p, ",\"seen\":%.1f", seen_seconds);
    } else {
        p += snprintf(p, end - p, ",\"seen\":0.0");
    }
    
    // Source (always included)
    const char *source = alert_publisher_get_source_string(a);
    p += snprintf(p, end - p, ",\"src\":\"%s\"", source);
    
    // Timestamp (always included)
    p += snprintf(p, end - p, ",\"ts\":%ld", (long)now);
    
    // End JSON object
    p += snprintf(p, end - p, "}");
    
    // Ensure null termination
    *end = '\0';
    
    return json_buffer;
}

// Publish alert for a specific aircraft
bool alert_publisher_publish_alert(alert_publisher_t *publisher, 
                                  mqtt_client_t *mqtt_client,
                                  const struct aircraft *a, 
                                  time_t now) {
    if (!publisher || !mqtt_client || !a) {
        return false;
    }
    
    uint32_t icao = a->addr & 0xFFFFFF;
    
    // Check if we should publish this alert
    if (!alert_publisher_should_publish(publisher, icao, now)) {
        return false;
    }
    
    // Create JSON message
    char *json_payload = alert_publisher_create_json(a, now);
    if (!json_payload) {
        time_t current_time = time(NULL);
        if (current_time - last_json_error_log >= ERROR_LOG_INTERVAL) {
            log_with_timestamp("Alert publisher error: Failed to create JSON payload for ICAO %06X", icao);
            last_json_error_log = current_time;
        }
        return false;
    }
    
    // Publish to MQTT
    bool success = mqtt_client_publish_alert(mqtt_client, json_payload);
    
    if (success) {
        // Update cooldown tracking
        alert_publisher_add_cooldown(publisher, icao, now);
        
        // Add to current cycle to prevent duplicates
        alert_publisher_add_to_cycle(publisher, icao);
        
        // Update statistics
        publisher->total_alerts_published++;
        
        // Log the alert with comprehensive details
        log_with_timestamp("ALERT: %06X %s %.6f,%.6f alt=%d gs=%.1f track=%.1f seen=%.1fs src=%s", 
                icao, 
                a->callsign[0] ? a->callsign : "N/A",
                trackDataValid(&a->pos_reliable_valid) ? a->latReliable : 0.0,
                trackDataValid(&a->pos_reliable_valid) ? a->lonReliable : 0.0,
                trackDataValid(&a->baro_alt_valid) ? a->baro_alt : 0,
                trackDataValid(&a->gs_valid) ? a->gs : 0.0,
                trackDataValid(&a->track_valid) ? a->track : 0.0,
                (now - a->seen) / 1000.0,
                alert_publisher_get_source_string(a));
    } else {
        // Log MQTT publish failure (rate limited)
        time_t current_time = time(NULL);
        if (current_time - last_mqtt_error_log >= ERROR_LOG_INTERVAL) {
            log_with_timestamp("Alert publisher error: Failed to publish MQTT alert for ICAO %06X", icao);
            last_mqtt_error_log = current_time;
        }
    }
    
    // Free JSON buffer
    free(json_payload);
    
    return success;
}

// Main function to check aircraft table and publish alerts
void alert_publisher_check_aircraft(alert_publisher_t *publisher, 
                                   struct watchlist_config *watchlist,
                                   mqtt_client_t *mqtt_client) {
    if (!publisher || !watchlist || !mqtt_client) {
        return;
    }
    
    // This function will be integrated with the main aircraft table iteration
    // For now, it's a placeholder that shows the intended interface
    
    time_t now = time(NULL);
    
    // Start new processing cycle
    alert_publisher_start_cycle(publisher, now);
    
    // TODO: Integrate with main aircraft table iteration
    // This will be done in task 7 when integrating with json_out.c
    // 
    // The integration will look like:
    // for (int j = 0; j < Modes.acBuckets; j++) {
    //     for (struct aircraft *a = Modes.aircraft[j]; a; a = a->next) {
    //         if (watchlist_contains(watchlist, a->addr & 0xFFFFFF)) {
    //             alert_publisher_publish_alert(publisher, mqtt_client, a, now);
    //         }
    //     }
    // }
    
    // End processing cycle
    alert_publisher_end_cycle(publisher);
}

// Log statistics for observability
void alert_publisher_log_statistics(alert_publisher_t *publisher) {
    if (!publisher) {
        return;
    }
    
    log_with_timestamp("Alert publisher stats: published=%u, suppressed_cooldown=%u, suppressed_duplicate=%u, cooldown_entries=%u", 
                      publisher->total_alerts_published,
                      publisher->alerts_suppressed_cooldown,
                      publisher->alerts_suppressed_duplicate,
                      publisher->cooldown_occupied);
}