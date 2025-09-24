// Part of readsb, a Mode-S/ADSB/TIS message decoder.
//
// mqtt_client.h: MQTT client management for watchlist alerting
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

#ifndef MQTT_CLIENT_H
#define MQTT_CLIENT_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#ifdef ENABLE_MQTT
#include <mosquitto.h>
#endif

#include "watchlist.h"

// MQTT client structure
typedef struct {
#ifdef ENABLE_MQTT
    struct mosquitto *mosq;
#endif
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
} mqtt_client_t;

// Reconnection constants
#define MQTT_INITIAL_RECONNECT_DELAY 1
#define MQTT_MAX_RECONNECT_DELAY 60
#define MQTT_ERROR_LOG_INTERVAL 30

// Function declarations
bool mqtt_client_init(mqtt_client_t *client, const watchlist_config_t *config);
bool mqtt_client_connect(mqtt_client_t *client);
bool mqtt_client_publish_alert(mqtt_client_t *client, const char *json_payload);
void mqtt_client_process(mqtt_client_t *client);
void mqtt_client_cleanup(mqtt_client_t *client);

// Internal callback functions
#ifdef ENABLE_MQTT
void mqtt_client_connect_callback(struct mosquitto *mosq, void *userdata, int result);
void mqtt_client_disconnect_callback(struct mosquitto *mosq, void *userdata, int result);
void mqtt_client_log_callback(struct mosquitto *mosq, void *userdata, int level, const char *str);
#endif

#endif // MQTT_CLIENT_H