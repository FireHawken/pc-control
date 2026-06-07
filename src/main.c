#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <windows.h>
#include <powrprof.h>
#include <MQTTClient.h>

#include "version.h"

#define TOPIC_PREFIX "pc-control"
#define DEFAULT_PORT "1883"
#define CLIENT_ID_PREFIX "pc-control-"
#define QOS 1
#define LOG_FILE "pc-control.log"
#define RECONNECT_DELAY_BASE_MS 1000
#define RECONNECT_DELAY_MAX_MS 30000
#define MONITOR_POWER_TIMEOUT_MS 2000
#define MAX_HOSTNAME_LEN 256
#define MAX_TOPIC_LEN 512

#define STATUS_ONLINE "online"
#define STATUS_OFFLINE "offline"

#define COMMAND_NONE 0
#define COMMAND_SLEEP 0x1
#define COMMAND_MONITOR_OFF 0x2

static volatile LONG running = 1;
static volatile LONG connected = 0;
static volatile LONG pending_commands = COMMAND_NONE;
static char topic_sleep[MAX_TOPIC_LEN];
static char topic_monitor_off[MAX_TOPIC_LEN];
static char topic_status[MAX_TOPIC_LEN];
static char topic_version[MAX_TOPIC_LEN];
static char client_id[MAX_HOSTNAME_LEN + 32];

static int read_flag(volatile LONG *flag) {
    return InterlockedCompareExchange(flag, 0, 0) != 0;
}

static void write_flag(volatile LONG *flag, LONG value) {
    InterlockedExchange(flag, value);
}

static int is_running(void) {
    return read_flag(&running);
}

static void request_stop(void) {
    write_flag(&running, 0);
}

static int is_connected(void) {
    return read_flag(&connected);
}

static void set_connected(LONG value) {
    write_flag(&connected, value);
}

static void log_action(const char *action) {
    FILE *f = fopen(LOG_FILE, "a");
    if (f) {
        time_t now = time(NULL);
        char timebuf[64];
        strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", localtime(&now));
        fprintf(f, "[%s] %s\n", timebuf, action);
        fclose(f);
    }
}

static void log_windows_error(const char *action, DWORD error_code) {
    char msg[256];

    if (error_code == ERROR_SUCCESS) {
        snprintf(msg, sizeof(msg), "%s failed or timed out", action);
    } else {
        snprintf(msg, sizeof(msg), "%s failed with GetLastError=%lu",
                 action, (unsigned long)error_code);
    }

    log_action(msg);
}

static void log_startup(void) {
    char msg[128];
    snprintf(msg, sizeof(msg), "pc-control v%s started", VERSION);
    log_action(msg);
}

static void sanitize_hostname(char *dest, const char *src, size_t dest_size) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j < dest_size - 1; i++) {
        char c = src[i];
        if (isalnum((unsigned char)c) || c == '-' || c == '_') {
            dest[j++] = (char)tolower((unsigned char)c);
        } else if (c == ' ' || c == '.') {
            dest[j++] = '-';
        }
        /* Skip other special characters */
    }
    dest[j] = '\0';
}

static int get_system_hostname(char *buf, size_t buf_size) {
    DWORD size = (DWORD)buf_size;
    if (!GetComputerNameA(buf, &size)) {
        return -1;
    }
    return 0;
}

static void do_sleep(void) {
    log_action("SLEEP command received - entering sleep mode");
    if (!SetSuspendState(FALSE, FALSE, FALSE)) {
        log_windows_error("SetSuspendState", GetLastError());
    }
}

static void do_monitor_off(void) {
    DWORD_PTR result = 0;

    log_action("MONITOR_OFF command received - turning off monitor");
    SetLastError(ERROR_SUCCESS);
    if (SendMessageTimeout(HWND_BROADCAST, WM_SYSCOMMAND, SC_MONITORPOWER, (LPARAM)2,
                           SMTO_ABORTIFHUNG, MONITOR_POWER_TIMEOUT_MS, &result) == 0) {
        log_windows_error("SendMessageTimeout(SC_MONITORPOWER)", GetLastError());
    }
}

static void queue_command(LONG command) {
    InterlockedOr(&pending_commands, command);
}

static void process_pending_commands(void) {
    LONG commands = InterlockedExchange(&pending_commands, COMMAND_NONE);

    if ((commands & COMMAND_SLEEP) != 0) {
        do_sleep();
    } else if ((commands & COMMAND_MONITOR_OFF) != 0) {
        do_monitor_off();
    }
}

static int payload_token_equals(const char *payload, size_t payload_len, const char *token) {
    size_t token_len = strlen(token);
    if (payload_len != token_len) {
        return 0;
    }

    for (size_t i = 0; i < payload_len; i++) {
        if (tolower((unsigned char)payload[i]) != token[i]) {
            return 0;
        }
    }

    return 1;
}

static int is_valid_command_payload(const MQTTClient_message *msg) {
    const char *payload;
    size_t start = 0;
    size_t end;

    if (msg == NULL || msg->payload == NULL || msg->payloadlen <= 0) {
        return 0;
    }

    payload = (const char *)msg->payload;
    end = (size_t)msg->payloadlen;

    while (start < end && isspace((unsigned char)payload[start])) {
        start++;
    }
    while (end > start && isspace((unsigned char)payload[end - 1])) {
        end--;
    }

    return payload_token_equals(payload + start, end - start, "1") ||
           payload_token_equals(payload + start, end - start, "true") ||
           payload_token_equals(payload + start, end - start, "on") ||
           payload_token_equals(payload + start, end - start, "yes");
}

static int should_accept_command_message(const MQTTClient_message *msg) {
    return msg != NULL && !msg->retained && is_valid_command_payload(msg);
}

static int message_arrived(void *context, char *topic, int topic_len, MQTTClient_message *msg) {
    (void)context;
    (void)topic_len;

    if (should_accept_command_message(msg)) {
        if (strcmp(topic, topic_sleep) == 0) {
            queue_command(COMMAND_SLEEP);
        } else if (strcmp(topic, topic_monitor_off) == 0) {
            queue_command(COMMAND_MONITOR_OFF);
        }
    }

    MQTTClient_freeMessage(&msg);
    MQTTClient_free(topic);
    return 1;
}

static void connection_lost(void *context, char *cause) {
    (void)context;
    set_connected(0);
#ifndef HIDDEN_BUILD
    fprintf(stderr, "Connection lost: %s\n", cause ? cause : "unknown");
#endif
    log_action("MQTT connection lost");
}

static BOOL WINAPI console_handler(DWORD signal) {
    switch (signal) {
    case CTRL_C_EVENT:
        log_action("Received CTRL_C signal");
        request_stop();
        return TRUE;
    case CTRL_BREAK_EVENT:
        log_action("Received CTRL_BREAK signal");
        request_stop();
        return TRUE;
    case CTRL_CLOSE_EVENT:
        log_action("Console window closed");
        request_stop();
        return TRUE;
    case CTRL_LOGOFF_EVENT:
        log_action("User logoff detected");
        request_stop();
        return TRUE;
    case CTRL_SHUTDOWN_EVENT:
        log_action("System shutdown detected");
        request_stop();
        return TRUE;
    }
    return FALSE;
}

static int publish_retained(MQTTClient client, const char *topic, const char *payload) {
    MQTTClient_message msg = MQTTClient_message_initializer;
    msg.payload = (void *)payload;
    msg.payloadlen = (int)strlen(payload);
    msg.qos = QOS;
    msg.retained = 1;
    return MQTTClient_publishMessage(client, topic, &msg, NULL);
}

static int subscribe_topics(MQTTClient client) {
    int rc;
    if ((rc = MQTTClient_subscribe(client, topic_sleep, QOS)) != MQTTCLIENT_SUCCESS) {
        fprintf(stderr, "Failed to subscribe to %s: %d\n", topic_sleep, rc);
        return rc;
    }
    if ((rc = MQTTClient_subscribe(client, topic_monitor_off, QOS)) != MQTTCLIENT_SUCCESS) {
        fprintf(stderr, "Failed to subscribe to %s: %d\n", topic_monitor_off, rc);
        return rc;
    }
    return MQTTCLIENT_SUCCESS;
}

static int publish_birth_messages(MQTTClient client) {
    int rc;
    if ((rc = publish_retained(client, topic_status, STATUS_ONLINE)) != MQTTCLIENT_SUCCESS) {
        fprintf(stderr, "Failed to publish status: %d\n", rc);
        return rc;
    }
    if ((rc = publish_retained(client, topic_version, VERSION)) != MQTTCLIENT_SUCCESS) {
        fprintf(stderr, "Failed to publish version: %d\n", rc);
        return rc;
    }
    return MQTTCLIENT_SUCCESS;
}

static int try_connect(MQTTClient client, MQTTClient_connectOptions *conn_opts, const char *address) {
    int rc = MQTTClient_connect(client, conn_opts);
    if (rc != MQTTCLIENT_SUCCESS) {
        return rc;
    }

    rc = publish_birth_messages(client);
    if (rc != MQTTCLIENT_SUCCESS) {
        MQTTClient_disconnect(client, 100);
        return rc;
    }

    rc = subscribe_topics(client);
    if (rc != MQTTCLIENT_SUCCESS) {
        MQTTClient_disconnect(client, 100);
        return rc;
    }

    set_connected(1);
    log_action("Connected to MQTT broker");
    printf("Connected to %s\n", address);
    printf("Status: %s -> %s\n", topic_status, STATUS_ONLINE);
    printf("Subscribed to:\n  %s\n  %s\n", topic_sleep, topic_monitor_off);
    printf("Waiting for commands...\n");
    return MQTTCLIENT_SUCCESS;
}

static void print_usage(const char *prog) {
    fprintf(stderr, "Usage: %s [--hide] <broker_ip> <username> <password> [port] [hostname]\n", prog);
    fprintf(stderr, "\n");
    fprintf(stderr, "Arguments:\n");
    fprintf(stderr, "  broker_ip  MQTT broker IP address\n");
    fprintf(stderr, "  username   MQTT username\n");
    fprintf(stderr, "  password   MQTT password\n");
    fprintf(stderr, "  port       MQTT broker port (default: %s)\n", DEFAULT_PORT);
    fprintf(stderr, "  hostname   Device name for topics (default: system hostname)\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  --hide     Hide console window (for autostart)\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Topics:\n");
    fprintf(stderr, "  %s/<hostname>/sleep        - Sleep command\n", TOPIC_PREFIX);
    fprintf(stderr, "  %s/<hostname>/monitor-off  - Monitor off command\n", TOPIC_PREFIX);
    fprintf(stderr, "  %s/<hostname>/status       - Online/offline status (retained)\n", TOPIC_PREFIX);
    fprintf(stderr, "  %s/<hostname>/version      - Version info (retained)\n", TOPIC_PREFIX);
}

static int is_flag(const char *arg) {
    return arg[0] == '-';
}

int main(int argc, char *argv[]) {
    int hide_console = 0;
    int pos_argc = 0;
    char *pos_argv[6];  /* Max positional args */

    /* Parse arguments: separate flags from positional args */
    for (int i = 1; i < argc && pos_argc < 6; i++) {
        if (strcmp(argv[i], "--hide") == 0) {
            hide_console = 1;
        } else if (is_flag(argv[i])) {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        } else {
            pos_argv[pos_argc++] = argv[i];
        }
    }

    if (pos_argc < 3 || pos_argc > 5) {
        print_usage(argv[0]);
        return 1;
    }

    /* Hide console window if requested */
    if (hide_console) {
        HWND hwnd = GetConsoleWindow();
        if (hwnd != NULL) {
            ShowWindow(hwnd, SW_HIDE);
        }
    }

    const char *broker_ip = pos_argv[0];
    const char *username = pos_argv[1];
    const char *password = pos_argv[2];
    const char *port = (pos_argc >= 4) ? pos_argv[3] : DEFAULT_PORT;

    char hostname_raw[MAX_HOSTNAME_LEN];
    char hostname[MAX_HOSTNAME_LEN];

    if (pos_argc >= 5) {
        strncpy(hostname_raw, pos_argv[4], sizeof(hostname_raw) - 1);
        hostname_raw[sizeof(hostname_raw) - 1] = '\0';
    } else {
        if (get_system_hostname(hostname_raw, sizeof(hostname_raw)) != 0) {
            fprintf(stderr, "Failed to get system hostname\n");
            return 1;
        }
    }

    sanitize_hostname(hostname, hostname_raw, sizeof(hostname));
    if (strlen(hostname) == 0) {
        fprintf(stderr, "Invalid hostname\n");
        return 1;
    }

    /* Build topics and client ID */
    snprintf(topic_sleep, sizeof(topic_sleep), "%s/%s/sleep", TOPIC_PREFIX, hostname);
    snprintf(topic_monitor_off, sizeof(topic_monitor_off), "%s/%s/monitor-off", TOPIC_PREFIX, hostname);
    snprintf(topic_status, sizeof(topic_status), "%s/%s/status", TOPIC_PREFIX, hostname);
    snprintf(topic_version, sizeof(topic_version), "%s/%s/version", TOPIC_PREFIX, hostname);
    snprintf(client_id, sizeof(client_id), "%s%s", CLIENT_ID_PREFIX, hostname);

    char address[256];
    snprintf(address, sizeof(address), "tcp://%s:%s", broker_ip, port);

    printf("pc-control v%s\n", VERSION);
    printf("Device: %s\n", hostname);
    log_startup();

    MQTTClient client;
    MQTTClient_connectOptions conn_opts = MQTTClient_connectOptions_initializer;
    MQTTClient_willOptions will_opts = MQTTClient_willOptions_initializer;
    int rc;

    if ((rc = MQTTClient_create(&client, address, client_id,
                                 MQTTCLIENT_PERSISTENCE_NONE, NULL)) != MQTTCLIENT_SUCCESS) {
        fprintf(stderr, "Failed to create MQTT client: %d\n", rc);
        return 1;
    }

    if ((rc = MQTTClient_setCallbacks(client, NULL, connection_lost,
                                       message_arrived, NULL)) != MQTTCLIENT_SUCCESS) {
        fprintf(stderr, "Failed to set callbacks: %d\n", rc);
        MQTTClient_destroy(&client);
        return 1;
    }

    /* Configure Last Will and Testament */
    will_opts.topicName = topic_status;
    will_opts.message = STATUS_OFFLINE;
    will_opts.qos = QOS;
    will_opts.retained = 1;

    conn_opts.keepAliveInterval = 20;
    conn_opts.cleansession = 1;
    conn_opts.username = username;
    conn_opts.password = password;
    conn_opts.connectTimeout = 10;
    conn_opts.will = &will_opts;

    SetConsoleCtrlHandler(console_handler, TRUE);
    printf("Press Ctrl+C to exit.\n\n");

    int reconnect_delay = RECONNECT_DELAY_BASE_MS;

    while (is_running()) {
        process_pending_commands();

        if (!is_connected()) {
            rc = try_connect(client, &conn_opts, address);
            if (rc != MQTTCLIENT_SUCCESS) {
                fprintf(stderr, "Connection attempt failed (%d), retrying in %d ms...\n", rc, reconnect_delay);

                int slept = 0;
                while (is_running() && slept < reconnect_delay) {
                    Sleep(100);
                    slept += 100;
                }

                reconnect_delay *= 2;
                if (reconnect_delay > RECONNECT_DELAY_MAX_MS) {
                    reconnect_delay = RECONNECT_DELAY_MAX_MS;
                }
                continue;
            }
            reconnect_delay = RECONNECT_DELAY_BASE_MS;
        }
        Sleep(100);
    }

    log_action("Shutting down");

    /* Publish offline status on graceful shutdown */
    if (is_connected()) {
        publish_retained(client, topic_status, STATUS_OFFLINE);
        MQTTClient_disconnect(client, 1000);
    }
    MQTTClient_destroy(&client);

    printf("Goodbye.\n");
    return 0;
}
