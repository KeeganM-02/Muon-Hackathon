#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdint.h>
#include <errno.h>
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include <sys/time.h>
#include "esp_log.h"
#include <esp_http_server.h>

/* This bench network has no direct route to the internet: everything is
 * proxied through 192.168.49.1:8228. Two consequences.
 *
 * 1. Traffic that generates CSI has to be *local*. Sending NTP to a public
 *    server put packets on the air, but nothing ever came back through the
 *    proxy, so the receiver saw no reply frames -- and CSI is computed on
 *    received frames. Spamming UDP at the proxy address keeps the exchange on
 *    the link we care about.
 * 2. Anything that wants the internet has to go through the proxy. The probe
 *    below is a plain HTTP GET with an absolute URI, which is exactly what a
 *    proxy expects; esp_http_client handles that when proxy_host/proxy_port
 *    are set. HTTPS would need a CONNECT tunnel, which the client also does,
 *    but plain HTTP is enough to answer "is there a path out?".
 *
 * Keep CONFIG_PACKET_RATE at 1-2 Hz: one or two datagrams a second is plenty
 * of frames for a demo, and the proxy is not a ping target. */
#define PROXY_HOST "192.168.49.1"
#define PROXY_PORT 8228
#define PROBE_URL  "http://connectivitycheck.gstatic.com/generate_204"
#define PROBE_PACKET_SIZE 48

static const char *SOCK_TAG = "sockets";

static void build_probe_packet(uint8_t *packet) {
    memset(packet, 0, PROBE_PACKET_SIZE);
    memcpy(packet, "CSI-PROBE", 9);
}

/* One HTTP GET sent *through* the proxy.
 *
 * esp_http_client in this IDF (v6.1) has no proxy_host/proxy_port fields, so
 * the proxy request is written by hand: connect a TCP socket to the proxy and
 * send an absolute-URI request line, which is what a proxy expects for plain
 * HTTP. HTTPS would need a CONNECT tunnel -- add that here when something
 * actually needs TLS.
 *
 * Returns the HTTP status code, or -1 if the proxy could not be reached. */
static int proxy_http_get(const char *host, const char *path) {
    struct sockaddr_in proxy;
    memset(&proxy, 0, sizeof(proxy));
    proxy.sin_family = AF_INET;
    proxy.sin_port = htons(PROXY_PORT);
    if (inet_aton(PROXY_HOST, &proxy.sin_addr) == 0) {
        ESP_LOGE(SOCK_TAG, "proxy: bad address " PROXY_HOST);
        return -1;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        ESP_LOGE(SOCK_TAG, "proxy: socket() failed: %s", strerror(errno));
        return -1;
    }

    struct timeval tv = {};
    tv.tv_sec = 5;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(fd, (const struct sockaddr *) &proxy, sizeof(proxy)) != 0) {
        ESP_LOGW(SOCK_TAG, "proxy: connect %s:%d failed: %s",
                 PROXY_HOST, PROXY_PORT, strerror(errno));
        close(fd);
        return -1;
    }

    char request[256];
    int n = snprintf(request, sizeof(request),
                     "GET http://%s%s HTTP/1.1\r\n"
                     "Host: %s\r\n"
                     "User-Agent: fwog-csi-probe\r\n"
                     "Connection: close\r\n\r\n",
                     host, path, host);
    if (send(fd, request, n, 0) != n) {
        ESP_LOGW(SOCK_TAG, "proxy: send failed: %s", strerror(errno));
        close(fd);
        return -1;
    }

    char reply[256] = {0};
    int got = recv(fd, reply, sizeof(reply) - 1, 0);
    close(fd);
    if (got <= 0) {
        ESP_LOGW(SOCK_TAG, "proxy: no reply from %s:%d", PROXY_HOST, PROXY_PORT);
        return -1;
    }

    /* "HTTP/1.1 204 No Content" -> 204 */
    int status = -1;
    const char *sp = strchr(reply, ' ');
    if (sp != NULL) {
        status = atoi(sp + 1);
    }
    ESP_LOGI(SOCK_TAG, "proxy probe %s -> HTTP %d (%s)", PROXY_HOST, status, reply);
    return status;
}

int proxy_internet_probe(void) {
    return proxy_http_get("connectivitycheck.gstatic.com", "/generate_204");
}

void socket_transmitter_sta_loop(bool (*is_wifi_connected)()) {
    int socket_fd = -1;
    bool probed = false;
    while (1) {
        close(socket_fd);
        struct sockaddr_in caddr;
        memset(&caddr, 0, sizeof(caddr));
        caddr.sin_family = AF_INET;
        caddr.sin_port = htons(PROXY_PORT);
        while (!is_wifi_connected()) {
            // wait until connected to AP
            printf("wifi not connected. waiting...\n");
            vTaskDelay(1000 / portTICK_PERIOD_MS);
        }
        printf("initial wifi connection established.\n");
        if (inet_aton(PROXY_HOST, &caddr.sin_addr) == 0) {
            printf("ERROR: inet_aton\n");
            continue;
        }

        if (!probed) {
            probed = true;
            proxy_internet_probe();
        }

        socket_fd = socket(PF_INET, SOCK_DGRAM, 0);
        if (socket_fd == -1) {
            printf("ERROR: Socket creation error [%s]\n", strerror(errno));
            continue;
        }
        if (connect(socket_fd, (const struct sockaddr *) &caddr, sizeof(struct sockaddr)) == -1) {
            printf("ERROR: socket connection error [%s]\n", strerror(errno));
            continue;
        }

        printf("sending UDP probes to %s:%d.\n", PROXY_HOST, PROXY_PORT);
        uint8_t packet[PROBE_PACKET_SIZE];
        build_probe_packet(packet);
        double lag = 0.0;
        while (1) {
            double start_time = get_steady_clock_timestamp();
            if (!is_wifi_connected()) {
                printf("ERROR: wifi is not connected\n");
                break;
            }

            if (sendto(socket_fd, packet, PROBE_PACKET_SIZE, 0, (const struct sockaddr *) &caddr, sizeof(caddr)) !=
                PROBE_PACKET_SIZE) {
                vTaskDelay(1);
                continue;
            }

#if defined CONFIG_PACKET_RATE && (CONFIG_PACKET_RATE > 0)
            double wait_duration = (1000.0 / CONFIG_PACKET_RATE) - lag;
            int w = floor(wait_duration);
            vTaskDelay(w);
#else
            vTaskDelay(10); // This limits TX to approximately 100 per second.
#endif
            double end_time = get_steady_clock_timestamp();
            lag = end_time - start_time;
        }
    }
}
