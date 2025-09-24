// Part of readsb, a Mode-S/ADSB/TIS message decoder.
//
// watchlist.c: watchlist functionality for MQTT alerting
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
#include <ctype.h>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>
#include <limits.h>

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "watchlist.h"
#include "mqtt_client.h"
#include "util.h"

// Forward declaration
char *strdup(const char *s);

#ifndef TEST_BUILD
// Simple strdup implementation for compatibility
char *strdup(const char *s) {
    size_t len = strlen(s) + 1;
    char *dup = malloc(len);
    if (dup) {
        memcpy(dup, s, len);
    }
    return dup;
}
#endif

// Mock readsb dependencies for now - these will be properly integrated later
#define cmalloc(size) malloc(size)
#define sfree(x) do { free(x); x = NULL; } while (0)

// Include enhanced error handling
#include "watchlist_error_handling.h"

// Rate limiting for error logs
static time_t last_memory_error_log = 0;
static time_t last_file_error_log = 0;
static time_t last_parse_error_log = 0;
#define ERROR_LOG_INTERVAL 30

// Forward declaration of addrHash function when integrated with readsb
#ifdef TEST_BUILD
// Simple hash function for testing
static inline uint32_t addrHash(uint32_t addr, uint32_t bits) {
    uint32_t hash = addr;
    hash ^= (hash >> 16);
    hash ^= (hash >> 8);
    return hash & ((1U << bits) - 1);
}
#else
#include "fasthash.h"
// Forward declaration of addrHash function from aircraft.h
static inline uint32_t addrHash(uint32_t addr, uint32_t bits) {
    const uint64_t m = 0x880355f21e6d1965ULL;
    const uint64_t seed = 0x30732349f7810465ULL;
    uint64_t h = seed ^ (4 * m);

    uint64_t v = addr;
    h ^= mix_fasthash(v);
    h *= m;
    h = mix_fasthash(h);

    // collapse to required bit width while retaining as much info as possible
    uint64_t res = h ^ (h >> 32);

    if (bits < 16)
        res ^= (res >> 16);
    if (bits < 8)
        res ^= (res >> 8);

    return res & ((1U << bits) - 1);
}
#endif

#ifndef ENABLE_MQTT
// Stub implementations for MQTT functions when MQTT is disabled
bool mqtt_client_init(mqtt_client_t *client, const watchlist_config_t *config) {
    if (!client) return false;
    (void)config; // Suppress unused parameter warning
    memset(client, 0, sizeof(mqtt_client_t));
    client->enabled = false;
    return true;
}

void mqtt_client_process(mqtt_client_t *client) {
    // No-op when MQTT is disabled
    (void)client;
}

void mqtt_client_cleanup(mqtt_client_t *client) {
    // No-op when MQTT is disabled
    (void)client;
}

bool mqtt_client_connect(mqtt_client_t *client) {
    // No-op when MQTT is disabled
    (void)client;
    return false;
}

bool mqtt_client_publish_alert(mqtt_client_t *client, const char *json_payload) {
    // No-op when MQTT is disabled
    (void)client;
    (void)json_payload;
    return false;
}
#endif

// Hash function for ICAO addresses using the same pattern as existing code
static uint32_t watchlist_hash(uint32_t addr, uint32_t bits) {
    return addrHash(addr, bits);
}

// Initialize watchlist configuration with default values
bool watchlist_init(watchlist_config_t *config) {
    if (!config) {
        return false;
    }
    
    memset(config, 0, sizeof(watchlist_config_t));
    
    // Set default values
    config->file_path = NULL;
    config->mqtt_host = NULL;
    config->mqtt_port = 1883;
    config->mqtt_username = NULL;
    config->mqtt_password = NULL;
    config->mqtt_topic = strdup("meshtastic/adsb/watch");
    config->mqtt_qos = 0;
    config->mqtt_retain = false;
    config->cooldown_seconds = 300;
    config->last_mtime = 0;
    
    if (!config->mqtt_topic) {
        watchlist_cleanup(config);
        return false;
    }
    
    // Initialize hash table
    config->hash_bits = WATCHLIST_MIN_BITS;
    config->hash_buckets = 1ULL << config->hash_bits;
    config->occupied = 0;
    
    size_t table_size = config->hash_buckets * sizeof(uint32_t);
    config->icao_table = cmalloc(table_size);
    if (!config->icao_table) {
        time_t now = time(NULL);
        if (now - last_memory_error_log >= ERROR_LOG_INTERVAL) {
            log_with_timestamp("Watchlist error: Failed to allocate ICAO hash table memory (%zu bytes)", table_size);
            last_memory_error_log = now;
        }
        watchlist_cleanup(config);
        return false;
    }
    
    // Initialize all entries to empty
    memset(config->icao_table, 0xFF, table_size);
    
    return true;
}

// Clean up watchlist configuration and free memory
void watchlist_cleanup(watchlist_config_t *config) {
    if (!config) {
        return;
    }
    
    sfree(config->file_path);
    sfree(config->mqtt_host);
    sfree(config->mqtt_username);
    sfree(config->mqtt_password);
    sfree(config->mqtt_topic);
    sfree(config->icao_table);
    
    memset(config, 0, sizeof(watchlist_config_t));
}

// Normalize ICAO address string to uint32_t
uint32_t watchlist_normalize_icao(const char *icao_str) {
    if (!icao_str || strlen(icao_str) == 0) {
        return 0;
    }
    
    char normalized[16];
    const char *src = icao_str;
    char *dst = normalized;
    
    // Skip "0x" prefix if present
    if (src[0] == '0' && (src[1] == 'x' || src[1] == 'X')) {
        src += 2;
    }
    
    // Copy hex digits only, converting to uppercase
    while (*src && dst < normalized + sizeof(normalized) - 1) {
        if (isxdigit(*src)) {
            *dst++ = toupper(*src);
        } else {
            // Invalid character found, return 0
            return 0;
        }
        src++;
    }
    *dst = '\0';
    
    // Must have at least one hex digit
    if (dst == normalized) {
        return 0;
    }
    
    // Convert to uint32_t
    char *endptr;
    uint32_t icao = (uint32_t)strtoul(normalized, &endptr, 16);
    
    // Validate conversion - endptr should point to end of string
    if (*endptr != '\0') {
        return 0; // Invalid ICAO
    }
    
    // Allow ICAO 0 as it might be valid in some contexts
    return icao;
}

// Resize hash table to accommodate more entries
void watchlist_resize(watchlist_config_t *config, uint32_t new_bits) {
    if (!config || new_bits < WATCHLIST_MIN_BITS || new_bits > WATCHLIST_MAX_BITS) {
        return;
    }
    
    uint32_t old_buckets = config->hash_buckets;
    uint32_t *old_table = config->icao_table;
    
    // Update configuration
    config->hash_bits = new_bits;
    config->hash_buckets = 1ULL << new_bits;
    config->occupied = 0;
    
    // Allocate new table
    size_t new_size = config->hash_buckets * sizeof(uint32_t);
    config->icao_table = cmalloc(new_size);
    if (!config->icao_table) {
        // Restore old configuration on failure
        config->hash_bits = __builtin_ctz(old_buckets);
        config->hash_buckets = old_buckets;
        config->icao_table = old_table;
        
        time_t now = time(NULL);
        if (now - last_memory_error_log >= ERROR_LOG_INTERVAL) {
            log_with_timestamp("Watchlist error: Failed to resize ICAO hash table to %u buckets (%zu bytes), keeping current size", 
                              1ULL << new_bits, new_size);
            last_memory_error_log = now;
        }
        return;
    }
    
    // Initialize new table
    memset(config->icao_table, 0xFF, new_size);
    
    // Rehash existing entries
    for (uint32_t i = 0; i < old_buckets; i++) {
        if (old_table[i] != WATCHLIST_EMPTY) {
            watchlist_add_icao(config, old_table[i]);
        }
    }
    
    // Free old table
    free(old_table);
}

// Add ICAO address to hash table
void watchlist_add_icao(watchlist_config_t *config, uint32_t icao) {
    if (!config || !config->icao_table || icao == 0) {
        return;
    }
    
    uint32_t h, h0;
    h0 = h = watchlist_hash(icao, config->hash_bits);
    
    // Linear probing to find empty slot or existing entry
    while (config->icao_table[h] != WATCHLIST_EMPTY && config->icao_table[h] != icao) {
        h = (h + 1) & (config->hash_buckets - 1);
        if (h == h0) {
            // Table is full, resize if possible
            if (config->hash_bits < WATCHLIST_MAX_BITS) {
                watchlist_resize(config, config->hash_bits + 1);
                // Retry after resize
                watchlist_add_icao(config, icao);
            }
            return;
        }
    }
    
    // Add new entry
    if (config->icao_table[h] == WATCHLIST_EMPTY) {
        config->occupied++;
        config->icao_table[h] = icao;
    }
    
    // Resize if table is getting full
    if (config->occupied > config->hash_buckets / 3 && config->hash_bits < WATCHLIST_MAX_BITS) {
        watchlist_resize(config, config->hash_bits + 1);
    }
}

// Check if ICAO address is in watchlist
bool watchlist_contains(watchlist_config_t *config, uint32_t icao) {
    if (!config || !config->icao_table || icao == 0) {
        return false;
    }
    
    uint32_t h, h0;
    h0 = h = watchlist_hash(icao, config->hash_bits);
    
    // Linear probing to find entry
    while (config->icao_table[h] != WATCHLIST_EMPTY && config->icao_table[h] != icao) {
        h = (h + 1) & (config->hash_buckets - 1);
        if (h == h0) {
            break; // Full loop, not found
        }
    }
    
    return config->icao_table[h] == icao;
}

// JSON parsing state
typedef struct {
    const char *json;
    const char *pos;
    int line;
    int column;
    char error_msg[256];
} json_parser_t;

// JSON parsing helper functions
static void json_skip_whitespace(json_parser_t *parser) {
    while (*parser->pos && isspace(*parser->pos)) {
        if (*parser->pos == '\n') {
            parser->line++;
            parser->column = 1;
        } else {
            parser->column++;
        }
        parser->pos++;
    }
}

static bool json_expect_char(json_parser_t *parser, char expected) {
    json_skip_whitespace(parser);
    if (*parser->pos != expected) {
        snprintf(parser->error_msg, sizeof(parser->error_msg),
                "Expected '%c' at line %d, column %d, got '%c'",
                expected, parser->line, parser->column, *parser->pos);
        return false;
    }
    parser->pos++;
    parser->column++;
    return true;
}

static bool json_parse_string(json_parser_t *parser, char *buffer, size_t buffer_size) {
    if (!json_expect_char(parser, '"')) {
        return false;
    }
    
    size_t len = 0;
    while (*parser->pos && *parser->pos != '"' && len < buffer_size - 1) {
        if (*parser->pos == '\\') {
            parser->pos++;
            parser->column++;
            if (!*parser->pos) {
                snprintf(parser->error_msg, sizeof(parser->error_msg),
                        "Unterminated escape sequence at line %d, column %d",
                        parser->line, parser->column);
                return false;
            }
            // Handle basic escape sequences
            switch (*parser->pos) {
                case '"': buffer[len++] = '"'; break;
                case '\\': buffer[len++] = '\\'; break;
                case '/': buffer[len++] = '/'; break;
                case 'n': buffer[len++] = '\n'; break;
                case 'r': buffer[len++] = '\r'; break;
                case 't': buffer[len++] = '\t'; break;
                default:
                    snprintf(parser->error_msg, sizeof(parser->error_msg),
                            "Invalid escape sequence '\\%c' at line %d, column %d",
                            *parser->pos, parser->line, parser->column);
                    return false;
            }
        } else {
            buffer[len++] = *parser->pos;
        }
        parser->pos++;
        parser->column++;
    }
    
    if (*parser->pos != '"') {
        snprintf(parser->error_msg, sizeof(parser->error_msg),
                "Unterminated string at line %d, column %d",
                parser->line, parser->column);
        return false;
    }
    
    buffer[len] = '\0';
    parser->pos++;
    parser->column++;
    return true;
}

static bool json_parse_number(json_parser_t *parser, int *value) {
    json_skip_whitespace(parser);
    char *endptr;
    long num = strtol(parser->pos, &endptr, 10);
    
    if (endptr == parser->pos) {
        snprintf(parser->error_msg, sizeof(parser->error_msg),
                "Expected number at line %d, column %d",
                parser->line, parser->column);
        return false;
    }
    
    if (num < INT_MIN || num > INT_MAX) {
        snprintf(parser->error_msg, sizeof(parser->error_msg),
                "Number out of range at line %d, column %d",
                parser->line, parser->column);
        return false;
    }
    
    parser->column += (endptr - parser->pos);
    parser->pos = endptr;
    *value = (int)num;
    return true;
}

static bool json_parse_mqtt_object(json_parser_t *parser, watchlist_config_t *config) {
    if (!json_expect_char(parser, '{')) {
        return false;
    }
    
    json_skip_whitespace(parser);
    if (*parser->pos == '}') {
        parser->pos++;
        parser->column++;
        return true; // Empty object
    }
    
    while (true) {
        char key[64];
        if (!json_parse_string(parser, key, sizeof(key))) {
            return false;
        }
        
        if (!json_expect_char(parser, ':')) {
            return false;
        }
        
        if (strcmp(key, "host") == 0) {
            char host[256];
            if (!json_parse_string(parser, host, sizeof(host))) {
                return false;
            }
            sfree(config->mqtt_host);
            config->mqtt_host = strdup(host);
        } else if (strcmp(key, "port") == 0) {
            if (!json_parse_number(parser, &config->mqtt_port)) {
                return false;
            }
        } else if (strcmp(key, "username") == 0) {
            char username[256];
            if (!json_parse_string(parser, username, sizeof(username))) {
                return false;
            }
            sfree(config->mqtt_username);
            config->mqtt_username = strdup(username);
        } else if (strcmp(key, "password") == 0) {
            char password[256];
            if (!json_parse_string(parser, password, sizeof(password))) {
                return false;
            }
            sfree(config->mqtt_password);
            config->mqtt_password = strdup(password);
        } else if (strcmp(key, "topic") == 0) {
            char topic[256];
            if (!json_parse_string(parser, topic, sizeof(topic))) {
                return false;
            }
            sfree(config->mqtt_topic);
            config->mqtt_topic = strdup(topic);
        } else if (strcmp(key, "qos") == 0) {
            if (!json_parse_number(parser, &config->mqtt_qos)) {
                return false;
            }
            if (config->mqtt_qos < 0 || config->mqtt_qos > 2) {
                snprintf(parser->error_msg, sizeof(parser->error_msg),
                        "Invalid QoS value %d (must be 0, 1, or 2) at line %d, column %d",
                        config->mqtt_qos, parser->line, parser->column);
                return false;
            }
        } else if (strcmp(key, "retain") == 0) {
            json_skip_whitespace(parser);
            if (strncmp(parser->pos, "true", 4) == 0) {
                config->mqtt_retain = true;
                parser->pos += 4;
                parser->column += 4;
            } else if (strncmp(parser->pos, "false", 5) == 0) {
                config->mqtt_retain = false;
                parser->pos += 5;
                parser->column += 5;
            } else {
                snprintf(parser->error_msg, sizeof(parser->error_msg),
                        "Expected boolean value at line %d, column %d",
                        parser->line, parser->column);
                return false;
            }
        } else {
            // Skip unknown key-value pairs
            json_skip_whitespace(parser);
            if (*parser->pos == '"') {
                char dummy[256];
                if (!json_parse_string(parser, dummy, sizeof(dummy))) {
                    return false;
                }
            } else if (isdigit(*parser->pos) || *parser->pos == '-') {
                int dummy;
                if (!json_parse_number(parser, &dummy)) {
                    return false;
                }
            } else {
                snprintf(parser->error_msg, sizeof(parser->error_msg),
                        "Unsupported value type for key '%s' at line %d, column %d",
                        key, parser->line, parser->column);
                return false;
            }
        }
        
        json_skip_whitespace(parser);
        if (*parser->pos == '}') {
            parser->pos++;
            parser->column++;
            break;
        } else if (*parser->pos == ',') {
            parser->pos++;
            parser->column++;
        } else {
            snprintf(parser->error_msg, sizeof(parser->error_msg),
                    "Expected ',' or '}' at line %d, column %d",
                    parser->line, parser->column);
            return false;
        }
    }
    
    return true;
}

static bool json_parse_watchlist_array(json_parser_t *parser, watchlist_config_t *config) {
    if (!json_expect_char(parser, '[')) {
        return false;
    }
    
    json_skip_whitespace(parser);
    if (*parser->pos == ']') {
        parser->pos++;
        parser->column++;
        return true; // Empty array
    }
    
    int parsed_count = 0;
    while (true) {
        char icao_str[16];
        if (!json_parse_string(parser, icao_str, sizeof(icao_str))) {
            return false;
        }
        
        uint32_t icao = watchlist_normalize_icao(icao_str);
        if (icao == 0) {
            snprintf(parser->error_msg, sizeof(parser->error_msg),
                    "Invalid ICAO address '%s' at line %d, column %d",
                    icao_str, parser->line, parser->column);
            return false;
        }
        
        watchlist_add_icao(config, icao);
        parsed_count++;
        
        json_skip_whitespace(parser);
        if (*parser->pos == ']') {
            parser->pos++;
            parser->column++;
            break;
        } else if (*parser->pos == ',') {
            parser->pos++;
            parser->column++;
        } else {
            snprintf(parser->error_msg, sizeof(parser->error_msg),
                    "Expected ',' or ']' at line %d, column %d",
                    parser->line, parser->column);
            return false;
        }
    }
    
    return true;
}

// Enhanced JSON parser with proper error handling and validation
static bool parse_json_watchlist(const char *json_content, watchlist_config_t *config) {
    if (!json_content || !config) {
        return false;
    }
    
    json_parser_t parser = {
        .json = json_content,
        .pos = json_content,
        .line = 1,
        .column = 1,
        .error_msg = {0}
    };
    
    if (!json_expect_char(&parser, '{')) {
        fprintf(stderr, "JSON parse error: %s\n", parser.error_msg);
        return false;
    }
    
    json_skip_whitespace(&parser);
    if (*parser.pos == '}') {
        parser.pos++;
        return true; // Empty object
    }
    
    while (true) {
        char key[64];
        if (!json_parse_string(&parser, key, sizeof(key))) {
            fprintf(stderr, "JSON parse error: %s\n", parser.error_msg);
            return false;
        }
        
        if (!json_expect_char(&parser, ':')) {
            fprintf(stderr, "JSON parse error: %s\n", parser.error_msg);
            return false;
        }
        
        if (strcmp(key, "watchlist") == 0) {
            if (!json_parse_watchlist_array(&parser, config)) {
                fprintf(stderr, "JSON parse error: %s\n", parser.error_msg);
                return false;
            }
        } else if (strcmp(key, "mqtt") == 0) {
            if (!json_parse_mqtt_object(&parser, config)) {
                fprintf(stderr, "JSON parse error: %s\n", parser.error_msg);
                return false;
            }
        } else if (strcmp(key, "cooldown_seconds") == 0) {
            if (!json_parse_number(&parser, &config->cooldown_seconds)) {
                fprintf(stderr, "JSON parse error: %s\n", parser.error_msg);
                return false;
            }
            if (config->cooldown_seconds < 0) {
                fprintf(stderr, "JSON validation error: cooldown_seconds must be non-negative\n");
                return false;
            }
        } else {
            // Skip unknown top-level keys
            log_with_timestamp("Watchlist JSON warning: Unknown key '%s' ignored", key);
            json_skip_whitespace(&parser);
            
            // Skip the value (simplified - handles strings, numbers, objects, arrays)
            if (*parser.pos == '"') {
                char dummy[256];
                if (!json_parse_string(&parser, dummy, sizeof(dummy))) {
                    fprintf(stderr, "JSON parse error: %s\n", parser.error_msg);
                    return false;
                }
            } else if (isdigit(*parser.pos) || *parser.pos == '-') {
                int dummy;
                if (!json_parse_number(&parser, &dummy)) {
                    fprintf(stderr, "JSON parse error: %s\n", parser.error_msg);
                    return false;
                }
            } else if (*parser.pos == '{') {
                // Skip object - simplified implementation
                int brace_count = 1;
                parser.pos++;
                parser.column++;
                while (*parser.pos && brace_count > 0) {
                    if (*parser.pos == '{') brace_count++;
                    else if (*parser.pos == '}') brace_count--;
                    else if (*parser.pos == '\n') {
                        parser.line++;
                        parser.column = 1;
                        parser.pos++;
                        continue;
                    }
                    parser.pos++;
                    parser.column++;
                }
            } else if (*parser.pos == '[') {
                // Skip array - simplified implementation
                int bracket_count = 1;
                parser.pos++;
                parser.column++;
                while (*parser.pos && bracket_count > 0) {
                    if (*parser.pos == '[') bracket_count++;
                    else if (*parser.pos == ']') bracket_count--;
                    else if (*parser.pos == '\n') {
                        parser.line++;
                        parser.column = 1;
                        parser.pos++;
                        continue;
                    }
                    parser.pos++;
                    parser.column++;
                }
            }
        }
        
        json_skip_whitespace(&parser);
        if (*parser.pos == '}') {
            parser.pos++;
            break;
        } else if (*parser.pos == ',') {
            parser.pos++;
            parser.column++;
        } else {
            snprintf(parser.error_msg, sizeof(parser.error_msg),
                    "Expected ',' or '}' at line %d, column %d",
                    parser.line, parser.column);
            fprintf(stderr, "JSON parse error: %s\n", parser.error_msg);
            return false;
        }
    }
    
    return true;
}

// Validate configuration after loading
static bool watchlist_validate_config(watchlist_config_t *config) {
    if (!config) {
        return false;
    }
    
    // Validate cooldown_seconds
    if (config->cooldown_seconds < 0) {
        fprintf(stderr, "Configuration error: cooldown_seconds must be non-negative (got %d)\n", 
                config->cooldown_seconds);
        return false;
    }
    
    // Validate MQTT port
    if (config->mqtt_port <= 0 || config->mqtt_port > 65535) {
        fprintf(stderr, "Configuration error: MQTT port must be between 1-65535 (got %d)\n", 
                config->mqtt_port);
        return false;
    }
    
    // Validate MQTT QoS
    if (config->mqtt_qos < 0 || config->mqtt_qos > 2) {
        fprintf(stderr, "Configuration error: MQTT QoS must be 0, 1, or 2 (got %d)\n", 
                config->mqtt_qos);
        return false;
    }
    
    // Validate MQTT topic
    if (config->mqtt_topic && strlen(config->mqtt_topic) == 0) {
        fprintf(stderr, "Configuration error: MQTT topic cannot be empty\n");
        return false;
    }
    
    // Check for reasonable watchlist size
    if (config->occupied > 100000) {
        log_with_timestamp("Watchlist warning: Large watchlist size (%u entries) may impact performance", 
                config->occupied);
    }
    
    return true;
}

// Check if file needs to be reloaded based on modification time
bool watchlist_needs_reload(watchlist_config_t *config) {
    if (!config || !config->file_path) {
        return false;
    }
    
    struct stat file_stat;
    if (stat(config->file_path, &file_stat) != 0) {
        return false; // File doesn't exist or can't be accessed
    }
    
    return file_stat.st_mtime > config->last_mtime;
}

// Load watchlist from JSON file with comprehensive error handling
bool watchlist_load(watchlist_config_t *config) {
    if (!config) {
        fprintf(stderr, "Watchlist error: NULL configuration provided\n");
        return false;
    }
    
    if (!config->file_path) {
        fprintf(stderr, "Watchlist error: No file path specified\n");
        return false;
    }
    
    // Check if file exists and get modification time
    struct stat file_stat;
    if (stat(config->file_path, &file_stat) != 0) {
        // File doesn't exist - log warning once and continue
        static bool logged_missing = false;
        static char last_missing_path[512] = {0};
        
        if (!logged_missing || strcmp(last_missing_path, config->file_path) != 0) {
            log_with_timestamp("Watchlist warning: File not found: %s (continuing without watchlist)", 
                    config->file_path);
            logged_missing = true;
            strncpy(last_missing_path, config->file_path, sizeof(last_missing_path) - 1);
            last_missing_path[sizeof(last_missing_path) - 1] = '\0';
        }
        return false;
    }
    
    // Check if file has been modified
    if (file_stat.st_mtime <= config->last_mtime) {
        return true; // No changes, current configuration is valid
    }
    
    // Validate file size
    if (file_stat.st_size <= 0) {
        fprintf(stderr, "Watchlist error: Empty file: %s\n", config->file_path);
        return false;
    }
    
    if (file_stat.st_size > 10 * 1024 * 1024) { // Limit to 10MB
        fprintf(stderr, "Watchlist error: File too large: %s (%ld bytes, max 10MB)\n", 
                config->file_path, file_stat.st_size);
        return false;
    }
    
    // Open and read file
    FILE *fp = fopen(config->file_path, "r");
    if (!fp) {
        fprintf(stderr, "Watchlist error: Failed to open file: %s (%s)\n", 
                config->file_path, strerror(errno));
        return false;
    }
    
    // Allocate buffer for file content
    char *content = cmalloc(file_stat.st_size + 1);
    if (!content) {
        time_t now = time(NULL);
        if (now - last_memory_error_log >= ERROR_LOG_INTERVAL) {
            log_with_timestamp("Watchlist error: Failed to allocate memory for file content (%ld bytes)", 
                              file_stat.st_size);
            last_memory_error_log = now;
        }
        fclose(fp);
        return false;
    }
    
    // Read file content
    size_t bytes_read = fread(content, 1, file_stat.st_size, fp);
    fclose(fp);
    
    if (bytes_read != (size_t)file_stat.st_size) {
        fprintf(stderr, "Watchlist error: Failed to read complete file: %s (read %zu of %ld bytes)\n", 
                config->file_path, bytes_read, file_stat.st_size);
        free(content);
        return false;
    }
    
    content[bytes_read] = '\0';
    
    // Backup current state in case parsing fails
    uint32_t old_occupied = config->occupied;
    uint32_t *old_table = NULL;
    
    if (config->icao_table && config->occupied > 0) {
        size_t table_size = config->hash_buckets * sizeof(uint32_t);
        old_table = cmalloc(table_size);
        if (old_table) {
            memcpy(old_table, config->icao_table, table_size);
        } else {
            time_t now = time(NULL);
            if (now - last_memory_error_log >= ERROR_LOG_INTERVAL) {
                log_with_timestamp("Watchlist warning: Failed to backup current configuration during reload, proceeding without backup");
                last_memory_error_log = now;
            }
        }
    }
    
    // Clear existing watchlist for fresh parsing
    config->occupied = 0;
    if (config->icao_table) {
        memset(config->icao_table, 0xFF, config->hash_buckets * sizeof(uint32_t));
    }
    
    // Parse JSON content
    bool success = parse_json_watchlist(content, config);
    free(content);
    
    if (success) {
        // Validate the parsed configuration
        success = watchlist_validate_config(config);
    }
    
    if (success) {
        config->last_mtime = file_stat.st_mtime;
        log_with_timestamp("Watchlist loaded: %u aircraft from %s (cooldown: %ds, MQTT: %s:%d topic=%s qos=%d)", 
                config->occupied, config->file_path, config->cooldown_seconds,
                config->mqtt_host ? config->mqtt_host : "disabled", config->mqtt_port,
                config->mqtt_topic ? config->mqtt_topic : "N/A", config->mqtt_qos);
        
        // Free backup table
        if (old_table) {
            free(old_table);
        }
    } else {
        fprintf(stderr, "Watchlist error: Failed to parse or validate file: %s\n", config->file_path);
        
        // Restore previous state if backup exists
        if (old_table && config->icao_table) {
            config->occupied = old_occupied;
            memcpy(config->icao_table, old_table, config->hash_buckets * sizeof(uint32_t));
            free(old_table);
            log_with_timestamp("Watchlist: Restored previous configuration (%u aircraft)", old_occupied);
        } else {
            // No backup available, clear everything
            config->occupied = 0;
            if (config->icao_table) {
                memset(config->icao_table, 0xFF, config->hash_buckets * sizeof(uint32_t));
            }
        }
    }
    
    return success;
}

// Safe reload function that only updates watchlist entries and cooldown settings
// MQTT connection settings are preserved to avoid disrupting active connections
bool watchlist_reload_safe(watchlist_config_t *config) {
    if (!config) {
        fprintf(stderr, "Watchlist error: NULL configuration provided for safe reload\n");
        return false;
    }
    
    if (!config->file_path) {
        fprintf(stderr, "Watchlist error: No file path specified for safe reload\n");
        return false;
    }
    
    // Check if file exists and get modification time
    struct stat file_stat;
    if (stat(config->file_path, &file_stat) != 0) {
        log_with_timestamp("Watchlist warning: File not found during reload: %s (keeping current configuration)", 
                config->file_path);
        return false;
    }
    
    // Check if file has been modified
    if (file_stat.st_mtime <= config->last_mtime) {
        return true; // No changes, current configuration is valid
    }
    
    // Validate file size
    if (file_stat.st_size <= 0) {
        fprintf(stderr, "Watchlist error: Empty file during reload: %s\n", config->file_path);
        return false;
    }
    
    if (file_stat.st_size > 10 * 1024 * 1024) { // Limit to 10MB
        fprintf(stderr, "Watchlist error: File too large during reload: %s (%ld bytes, max 10MB)\n", 
                config->file_path, file_stat.st_size);
        return false;
    }
    
    // Create a temporary configuration to test parsing
    watchlist_config_t temp_config;
    memset(&temp_config, 0, sizeof(temp_config));
    
    // Initialize temporary config with current MQTT settings (preserve connections)
    temp_config.file_path = strdup(config->file_path);
    temp_config.mqtt_host = config->mqtt_host ? strdup(config->mqtt_host) : NULL;
    temp_config.mqtt_port = config->mqtt_port;
    temp_config.mqtt_username = config->mqtt_username ? strdup(config->mqtt_username) : NULL;
    temp_config.mqtt_password = config->mqtt_password ? strdup(config->mqtt_password) : NULL;
    temp_config.mqtt_topic = config->mqtt_topic ? strdup(config->mqtt_topic) : NULL;
    temp_config.mqtt_qos = config->mqtt_qos;
    temp_config.mqtt_retain = config->mqtt_retain;
    temp_config.cooldown_seconds = config->cooldown_seconds;
    temp_config.last_mtime = 0; // Force reload
    
    // Initialize hash table for temporary config
    temp_config.hash_bits = WATCHLIST_MIN_BITS;
    temp_config.hash_buckets = 1ULL << temp_config.hash_bits;
    temp_config.occupied = 0;
    
    size_t table_size = temp_config.hash_buckets * sizeof(uint32_t);
    temp_config.icao_table = cmalloc(table_size);
    if (!temp_config.icao_table) {
        watchlist_cleanup(&temp_config);
        return false;
    }
    memset(temp_config.icao_table, 0xFF, table_size);
    
    // Try to load the new configuration
    bool success = watchlist_load(&temp_config);
    
    if (success) {
        // Backup current watchlist data
        uint32_t old_occupied = config->occupied;
        uint32_t *old_table = config->icao_table;
        int old_cooldown = config->cooldown_seconds;
        
        // Replace watchlist data with new data
        config->occupied = temp_config.occupied;
        config->hash_bits = temp_config.hash_bits;
        config->hash_buckets = temp_config.hash_buckets;
        config->icao_table = temp_config.icao_table;
        config->cooldown_seconds = temp_config.cooldown_seconds;
        config->last_mtime = temp_config.last_mtime;
        
        // Prevent cleanup of transferred data
        temp_config.icao_table = NULL;
        
        // Free old table
        if (old_table) {
            free(old_table);
        }
        
        log_with_timestamp("Watchlist safely reloaded: %u aircraft (was %u), cooldown: %ds (was %ds)", 
                config->occupied, old_occupied, config->cooldown_seconds, old_cooldown);
        
        // Clean up temporary config (MQTT settings will be freed but that's OK since we copied them)
        watchlist_cleanup(&temp_config);
        
        return true;
    } else {
        fprintf(stderr, "Watchlist error: Failed to parse new configuration during safe reload, keeping current settings\n");
        watchlist_cleanup(&temp_config);
        return false;
    }
}