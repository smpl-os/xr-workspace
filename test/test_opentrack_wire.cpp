// Real wire-format test for OpenTrackSink: bind a UDP listener, send a known
// 30° yaw quaternion, verify we receive 48 bytes = 6 doubles with yaw≈30.
#include "../src/sink_opentrack.h"

#include <arpa/inet.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

int main() {
    const uint16_t port = 24242;

    int rx = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (bind(rx, (sockaddr *)&a, sizeof(a)) < 0) { perror("bind"); return 1; }

    HeadTrackConfig c;
    c.enabled = true; c.host = "127.0.0.1"; c.port = port; c.rate_hz = 0.f;

    OpenTrackSink sink;
    sink.configure(c);
    if (!sink.open()) { fprintf(stderr, "open failed\n"); return 1; }

    // 30° yaw about Y: w=cos(15°), y=sin(15°).
    const double t = 15.0 * M_PI / 180.0;
    Quat q{0.f, (float)sin(t), 0.f, (float)cos(t)};
    sink.send(q);

    double pkt[6] = {0};
    ssize_t n = recv(rx, pkt, sizeof(pkt), 0);
    if (n != (ssize_t)sizeof(pkt)) { fprintf(stderr, "bad size %zd (want 48)\n", n); return 1; }

    printf("recv %zd bytes: x=%.2f y=%.2f z=%.2f yaw=%.2f pitch=%.2f roll=%.2f\n",
           n, pkt[0], pkt[1], pkt[2], pkt[3], pkt[4], pkt[5]);

    int ok = 1;
    if (fabs(pkt[3] - 30.0) > 0.5) { printf("FAIL yaw expected ~30\n"); ok = 0; }
    if (fabs(pkt[4]) > 0.5)        { printf("FAIL pitch expected ~0\n"); ok = 0; }
    if (fabs(pkt[5]) > 0.5)        { printf("FAIL roll expected ~0\n");  ok = 0; }
    if (fabs(pkt[0]) + fabs(pkt[1]) + fabs(pkt[2]) > 0.001) { printf("FAIL translation should be 0\n"); ok = 0; }

    sink.close();
    close(rx);
    printf(ok ? "PASS\n" : "FAIL\n");
    return ok ? 0 : 1;
}
