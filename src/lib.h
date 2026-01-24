#ifndef LIB_H_
#define LIB_H_

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <errno.h>
#include <signal.h>
#include <readline/readline.h>
#include <readline/history.h>

#define DEFAULT_PORT 6969
#define BUFFER_SIZE 1024
#define MAX_CLIENTS 10

struct client_info {
    int sock;
    struct sockaddr_in address;
    bool active;
};

static struct client_info clients[MAX_CLIENTS];
static pthread_mutex_t clients_mu = PTHREAD_MUTEX_INITIALIZER;
static volatile sig_atomic_t server_running = 0;
static char *ip = NULL;
static int port = DEFAULT_PORT;

static void init_runtime(void) {
    static int inited = 0;
    if (inited) return;
    inited = 1;
    signal(SIGPIPE, SIG_IGN);
    for (int i = 0; i < MAX_CLIENTS; ++i) clients[i].sock = -1;
}

static int parse_port_str(const char *s, int defv) {
    if (!s || !*s) return defv;
    char *end = NULL;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno || end == s || *end) return defv;
    if (v < 1 || v > 65535) return defv;
    return (int)v;
}

static void parse_ip_port(char *arg, char **out_ip, int *out_port) {
    if (!out_ip || !out_port) return;
    *out_ip = NULL;
    *out_port = DEFAULT_PORT;
    if (!arg || !*arg) return;
    int colons = 0;
    for (char *p = arg; *p; ++p) if (*p == ':') colons++;
    if (colons == 1) {
        char *c = strchr(arg, ':');
        *c = 0;
        *out_ip = arg;
        *out_port = parse_port_str(c + 1, DEFAULT_PORT);
    } else {
        *out_ip = arg;
        *out_port = DEFAULT_PORT;
    }
}

static size_t sanitize_text(char *dst, size_t dsz, const char *src, size_t n) {
    if (!dst || !dsz) return 0;
    size_t j = 0;
    for (size_t i = 0; i < n && j + 1 < dsz; ++i) {
        unsigned char c = (unsigned char)src[i];
        if (c == 0x1b) { dst[j++] = '?'; continue; }
        if (c < 0x20 && c != '\n' && c != '\r' && c != '\t') { dst[j++] = '?'; continue; }
        if (c == 0x7f) { dst[j++] = '?'; continue; }
        dst[j++] = (char)c;
    }
    dst[j] = 0;
    return j;
}

static int send_full(int fd, const char *buf, size_t len) {
    if (fd < 0 || !buf) return -1;
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(fd, buf + off, len - off,
#ifdef MSG_NOSIGNAL
                         MSG_NOSIGNAL
#else
                         0
#endif
        );
        if (n > 0) { off += (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        return -1;
    }
    return 0;
}

static void send_to_all_clients(const char *message, size_t len, int sender_sock) {
    int fds[MAX_CLIENTS], k = 0;
    pthread_mutex_lock(&clients_mu);
    for (int i = 0; i < MAX_CLIENTS; ++i)
        if (clients[i].active && clients[i].sock >= 0 && clients[i].sock != sender_sock)
            fds[k++] = clients[i].sock;
    pthread_mutex_unlock(&clients_mu);
    for (int i = 0; i < k; ++i) send_full(fds[i], message, len);
}

static void cleanup_clients(void) {
    pthread_mutex_lock(&clients_mu);
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (clients[i].active && clients[i].sock >= 0) {
            int fd = clients[i].sock;
            clients[i].sock = -1;
            clients[i].active = false;
            shutdown(fd, SHUT_RDWR);
            close(fd);
        }
    }
    pthread_mutex_unlock(&clients_mu);
}

static void print_ip(void) {
    init_runtime();
    FILE *fp;
    char public_ip[BUFFER_SIZE] = {0};
    if ((fp = popen("curl -fsS --max-time 2 ifconfig.me 2>/dev/null", "r")) != NULL) {
        if (fgets(public_ip, sizeof(public_ip), fp) != NULL) public_ip[strcspn(public_ip, "\r\n")] = 0;
        pclose(fp);
    }

    int sock;
    struct sockaddr_in serv = {.sin_family = AF_INET, .sin_port = htons(80)};
    struct sockaddr_in name;
    socklen_t namelen = sizeof(name);
    char local_ip[BUFFER_SIZE] = {0};

    if ((sock = socket(AF_INET, SOCK_DGRAM, 0)) < 0) { perror("socket"); return; }
    inet_pton(AF_INET, "8.8.8.8", &serv.sin_addr);
    if (connect(sock, (struct sockaddr *)&serv, sizeof(serv)) < 0) { perror("connect"); close(sock); return; }
    if (getsockname(sock, (struct sockaddr *)&name, &namelen) < 0) { perror("getsockname"); close(sock); return; }
    close(sock);
    if (!inet_ntop(AF_INET, &name.sin_addr, local_ip, sizeof(local_ip))) { perror("inet_ntop"); return; }

    if (public_ip[0]) printf("Public IP: %s, Local IP: %s -> ./chat -c %s:%d\n", public_ip, local_ip, local_ip, port);
    else printf("Local IP: %s -> ./chat -c %s:%d\n", local_ip, local_ip, port);
}

static int check_server_running(int p) {
    int sock;
    struct sockaddr_in serv_addr;
    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) return 0;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons((uint16_t)p);
    serv_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) { close(sock); return 0; }
    close(sock);
    return 1;
}

static int localcmd(char *buffer, int sock) {
    if (!buffer) return 1;
    if (!strcmp(buffer, "exit")) {
        printf("Exiting...\n");
        if (sock >= 0) { shutdown(sock, SHUT_RDWR); close(sock); }
        exit(0);
    }
    if (!strcmp(buffer, "ip")) { print_ip(); return 1; }
    if (!strcmp(buffer, "help")) {
        printf("Available commands:\n  help      - Display this help message\n  exit      - Disconnect and exit the program\n  ip        - Shows your ip and command to connect if is a server\n");
        return 1;
    }
    return 0;
}

struct recv_ctx {
    int sock;
    struct sockaddr_in addr;
    struct client_info *slot;
    bool is_server;
};

static void slot_deactivate(struct client_info *slot) {
    if (!slot) return;
    pthread_mutex_lock(&clients_mu);
    slot->active = false;
    slot->sock = -1;
    pthread_mutex_unlock(&clients_mu);
}

static void *receive_messages(void *arg) {
    struct recv_ctx *ctx = (struct recv_ctx *)arg;
    int sock = ctx->sock;
    struct sockaddr_in addr = ctx->addr;
    struct client_info *slot = ctx->slot;
    bool is_server = ctx->is_server;

    char raw[BUFFER_SIZE];
    char safe[BUFFER_SIZE * 2];
    bool connected = false;

    for (;;) {
        ssize_t n = read(sock, raw, sizeof(raw));
        if (n <= 0) break;

        if (is_server && !connected) {
            char ipbuf[INET_ADDRSTRLEN] = {0};
            inet_ntop(AF_INET, &addr.sin_addr, ipbuf, sizeof(ipbuf));
            printf("\x1b[A\nClient %s:%d Connected.\n", ipbuf[0] ? ipbuf : "?", ntohs(addr.sin_port));
            connected = true;
        }

        size_t sl = sanitize_text(safe, sizeof(safe), raw, (size_t)n);

        if (is_server) printf("\x1b[2K\r%s\nServer❯ ", safe);
        else {
            const char *u = getlogin();
            if (!u || !*u) u = "Client";
            printf("\x1b[2K\r%s\n%s❯ ", safe, u);
        }
        fflush(stdout);

        if (is_server) send_to_all_clients(safe, sl, sock);
    }

    if (is_server && connected) {
        char ipbuf[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &addr.sin_addr, ipbuf, sizeof(ipbuf));
        printf("\x1b[A\nClient %s:%d Disconnected.\nServer❯ ", ipbuf[0] ? ipbuf : "?", ntohs(addr.sin_port));
        fflush(stdout);
    }

    if (slot) slot_deactivate(slot);
    shutdown(sock, SHUT_RDWR);
    close(sock);
    free(ctx);
    return NULL;
}

static void *server_input(void *arg) {
    (void)arg;
    char *buffer;
    while (true) {
        buffer = readline("Server❯ ");
        if (!buffer) break;
        if (*buffer) {
            add_history(buffer);
            if (!localcmd(buffer, -1)) {
                char msg[BUFFER_SIZE + 64];
                int n = snprintf(msg, sizeof(msg), "Server❯ %s", buffer);
                if (n > 0) send_to_all_clients(msg, (size_t)n, -1);
            }
        }
        free(buffer);
    }
    return NULL;
}

static void client_input(int sock) {
    char *buffer;
    const char *login_name = getlogin();
    char name[256];
    if (login_name && *login_name) snprintf(name, sizeof(name), "%s❯ ", login_name);
    else snprintf(name, sizeof(name), "Client❯ ");

    while (true) {
        buffer = readline(name);
        if (!buffer) break;
        if (*buffer) {
            add_history(buffer);
            if (!localcmd(buffer, sock)) {
                char msg[BUFFER_SIZE + 300];
                int n = snprintf(msg, sizeof(msg), "%s%s", name, buffer);
                if (n > 0) send_full(sock, msg, (size_t)n);
            }
        }
        free(buffer);
    }
}

static int set_reuse_opts(int fd) {
    int opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) return -1;
#ifdef SO_REUSEPORT
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0) return -1;
#endif
    return 0;
}

static void run_server(const char *bind_ip, int p) {
    init_runtime();
    port = p;

    struct sockaddr_in address;
    int server_fd;

    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) { perror("socket"); exit(EXIT_FAILURE); }
    if (set_reuse_opts(server_fd) < 0) { perror("setsockopt"); close(server_fd); exit(EXIT_FAILURE); }

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)p);
    address.sin_addr.s_addr = INADDR_ANY;

    if (bind_ip && *bind_ip && strcmp(bind_ip, "0.0.0.0") != 0) {
        if (inet_pton(AF_INET, bind_ip, &address.sin_addr) != 1) { perror("inet_pton"); close(server_fd); exit(EXIT_FAILURE); }
    }

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) { perror("bind"); close(server_fd); exit(EXIT_FAILURE); }
    if (listen(server_fd, MAX_CLIENTS) < 0) { perror("listen"); close(server_fd); exit(EXIT_FAILURE); }

    print_ip();
    printf("Server listening on %s:%d\n", (bind_ip && *bind_ip) ? bind_ip : "0.0.0.0", p);

    server_running = 1;

    pthread_t input_thread_id;
    if (pthread_create(&input_thread_id, NULL, server_input, NULL) != 0) { perror("pthread_create"); close(server_fd); exit(EXIT_FAILURE); }
    pthread_detach(input_thread_id);

    while (true) {
        struct sockaddr_in cliaddr;
        socklen_t addrlen = sizeof(cliaddr);
        int new_socket = accept(server_fd, (struct sockaddr *)&cliaddr, &addrlen);
        if (new_socket < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            cleanup_clients();
            close(server_fd);
            exit(EXIT_FAILURE);
        }

        int idx = -1;
        pthread_mutex_lock(&clients_mu);
        for (int i = 0; i < MAX_CLIENTS; ++i) if (!clients[i].active) { idx = i; break; }
        if (idx >= 0) {
            clients[idx].sock = new_socket;
            clients[idx].address = cliaddr;
            clients[idx].active = true;
        }
        pthread_mutex_unlock(&clients_mu);

        if (idx < 0) { printf("Too many clients. Connection rejected.\n"); close(new_socket); continue; }

        struct recv_ctx *ctx = calloc(1, sizeof(*ctx));
        if (!ctx) {
            slot_deactivate(&clients[idx]);
            close(new_socket);
            continue;
        }
        ctx->sock = new_socket;
        ctx->addr = cliaddr;
        ctx->slot = &clients[idx];
        ctx->is_server = true;

        pthread_t thread_id;
        if (pthread_create(&thread_id, NULL, receive_messages, (void *)ctx) != 0) {
            perror("pthread_create");
            slot_deactivate(&clients[idx]);
            close(new_socket);
            free(ctx);
            continue;
        }
        pthread_detach(thread_id);
    }

    close(server_fd);
    server_running = 0;
    cleanup_clients();
}

static void run_client(char *server_ip, int p) {
    init_runtime();
    port = p;

    int sock;
    struct sockaddr_in serv_addr;

    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) { perror("socket"); exit(EXIT_FAILURE); }

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons((uint16_t)p);

    if (inet_pton(AF_INET, server_ip, &serv_addr.sin_addr) != 1) { perror("inet_pton"); close(sock); exit(EXIT_FAILURE); }
    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) { perror("connect"); close(sock); exit(EXIT_FAILURE); }

    printf("Connected to %s:%d.\n", server_ip, p);

    struct recv_ctx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) { close(sock); exit(EXIT_FAILURE); }
    ctx->sock = sock;
    ctx->addr = serv_addr;
    ctx->slot = NULL;
    ctx->is_server = false;

    pthread_t thread_id;
    if (pthread_create(&thread_id, NULL, receive_messages, (void *)ctx) != 0) { perror("pthread_create"); close(sock); free(ctx); exit(EXIT_FAILURE); }
    pthread_detach(thread_id);

    client_input(sock);
    shutdown(sock, SHUT_RDWR);
    close(sock);
}

#endif /* LIB_H_ */
