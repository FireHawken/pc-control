#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <windows.h>
#include <powrprof.h>
#include <MQTTClient.h>

#define VERSION "1.1.0"
#define TOPIC_PREFIX "pc-control"
#define DEFAULT_PORT "1883"
#define CLIENT_ID_PREFIX "pc-control-"
#define QOS 1
#define LOG_FILE "pc-control.log"
#define RECONNECT_DELAY_BASE_MS 1000
#define RECONNECT_DELAY_MAX_MS 30000
#define MAX_HOSTNAME_LEN 256
#define MAX_TOPIC_LEN 512

#define STATUS_ONLINE "online"
#define STATUS_OFFLINE "offline"

static volatile int running = 1;
static volatile int connected = 0;
static char topic_sleep[MAX_TOPIC_LEN];
static char topic_monitor_off[MAX_TOPIC_LEN];
static char topic_status[MAX_TOPIC_LEN];
static char topic_version[MAX_TOPIC_LEN];
static char client_id[MAX_HOSTNAME_LEN + 32];

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
    SetSuspendState(FALSE, FALSE, FALSE);
}

static void do_monitor_off(void) {
    log_action("MONITOR_OFF command received - turning off monitor");
    SendMessage(HWND_BROADCAST, WM_SYSCOMMAND, SC_MONITORPOWER, 2);
}

static int message_arrived(void *context, char *topic, int topic_len, MQTTClient_message *msg) {
    (void)context;
    (void)topic_len;
    (void)msg;

    if (strcmp(topic, topic_sleep) == 0) {
        do_sleep();
    } else if (strcmp(topic, topic_monitor_off) == 0) {
        do_monitor_off();
    }

    MQTTClient_freeMessage(&msg);
    MQTTClient_free(topic);
    return 1;
}

static void connection_lost(void *context, char *cause) {
    (void)context;
    connected = 0;
    fprintf(stderr, "Connection lost: %s\n", cause ? cause : "unknown");
    log_action("MQTT connection lost");
}

static BOOL WINAPI console_handler(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT) {
        running = 0;
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

    connected = 1;
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
        if (strcmp(argv[i], "--hide") == 0 || strcmp(argv[i], "-h") == 0) {
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

    while (running) {
        if (!connected) {
            rc = try_connect(client, &conn_opts, address);
            if (rc != MQTTCLIENT_SUCCESS) {
                fprintf(stderr, "Connection attempt failed (%d), retrying in %d ms...\n", rc, reconnect_delay);

                int slept = 0;
                while (running && slept < reconnect_delay) {
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
    if (connected) {
        publish_retained(client, topic_status, STATUS_OFFLINE);
        MQTTClient_disconnect(client, 1000);
    }
    MQTTClient_destroy(&client);

    printf("Goodbye.\n");
    return 0;
}
