/*
 * webserver.c — epoll Reactor + 线程池 的 HTTP/1.1 静态文件服务器（v2）
 *
 * 并发模型：
 *   主线程（Reactor 线程）：epoll_wait 负责 accept 新连接、分发就绪事件、
 *                           每秒 tick 驱动时间轮清理空闲连接；
 *   工作线程（线程池）    ：在 EPOLLONESHOT 保护下独占连接，完成
 *                           recv → HTTP 解析 → sendfile 发送 → 重新挂回 epoll。
 *
 * 关键技术点：
 *   - epoll LT + EPOLLONESHOT：同一连接事件不会被多个线程同时处理；
 *   - 非阻塞 socket + 增量解析器：正确处理半包/粘包与 HTTP 管道化；
 *   - sendfile 零拷贝发送静态文件；send 循环处理短写（short write）；
 *   - Keep-Alive 长连接 + 时间轮超时回收；
 *   - URL 解码后再做目录遍历检查，防 %2e%2e 绕过；
 *   - 异步日志、信号优雅退出。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sendfile.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "log.h"
#include "threadpool.h"
#include "http_parser.h"
#include "timer_wheel.h"

#define MAX_EVENTS        1024
#define READ_BUFFER_SIZE  65536
#define KEEPALIVE_IDLE    15   /* Keep-Alive 空闲超时（秒） */
#define TIMER_SLOTS       60   /* 时间轮槽位（一圈 60 秒） */

/* 全局配置（可由命令行参数覆盖） */
static int   g_port     = 80;
static int   g_threads  = 0;    /* 0 = 按 CPU 核数 */
static char  g_docroot[256] = ".";

/* 连接上下文：连接生命周期内常驻，存放该连接的接收缓冲区（处理半包） */
typedef struct conn_ctx
{
    int fd;
    int epfd;
    timer_wheel_t *timer;
    char ip[INET6_ADDRSTRLEN];
    char rbuf[READ_BUFFER_SIZE];
    int  rlen;
} conn_ctx_t;

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

/* 根据文件扩展名返回 MIME 类型 */
static const char *get_mime_type(const char *path)
{
    const char *ext = strrchr(path, '.');
    if (ext == NULL)
    {
        return "application/octet-stream";
    }
    if (strcasecmp(ext, ".html") == 0 || strcasecmp(ext, ".htm") == 0) return "text/html; charset=utf-8";
    if (strcasecmp(ext, ".css")  == 0) return "text/css; charset=utf-8";
    if (strcasecmp(ext, ".js")   == 0) return "application/javascript";
    if (strcasecmp(ext, ".jpg")  == 0 || strcasecmp(ext, ".jpeg") == 0) return "image/jpeg";
    if (strcasecmp(ext, ".png")  == 0) return "image/png";
    if (strcasecmp(ext, ".gif")  == 0) return "image/gif";
    if (strcasecmp(ext, ".ico")  == 0) return "image/x-icon";
    if (strcasecmp(ext, ".svg")  == 0) return "image/svg+xml";
    return "application/octet-stream";
}

/* 循环发送，处理短写（short write）和 EINTR */
static int send_all(int fd, const char *buf, int len)
{
    int sent = 0;
    while (sent < len)
    {
        ssize_t n = send(fd, buf + sent, (size_t)(len - sent), MSG_NOSIGNAL);
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return -1;
        }
        sent += (int)n;
    }
    return 0;
}

/* sendfile 零拷贝发送整个文件，处理 EINTR 与部分发送 */
static int send_file_all(int sockfd, int filefd, off_t total)
{
    off_t remaining = total;
    while (remaining > 0)
    {
        ssize_t n = sendfile(sockfd, filefd, NULL, (size_t)remaining);
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return -1;
        }
        if (n == 0)
        {
            break;
        }
        remaining -= n;
    }
    return 0;
}

/* 发送一次完整 HTTP 响应；返回 1 表示连接可复用，0 表示应关闭 */
static int serve_request(conn_ctx_t *ctx, const http_request_t *req)
{
    const char *url = req->path;
    if (url[0] == '/' && url[1] == '\0')
    {
        url = "/index.html"; /* 默认首页 */
    }

    char fspath[768];
    snprintf(fspath, sizeof(fspath), "%s%s", g_docroot, url);

    int status = 200;
    int filefd = -1;
    struct stat st;

    /* 仅支持 GET，其他方法按 404 处理（与 v1 行为一致） */
    if (strcmp(req->method, "GET") != 0)
    {
        status = 404;
    }
    else
    {
        filefd = open(fspath, O_RDONLY);
        if (filefd < 0 || fstat(filefd, &st) != 0 || !S_ISREG(st.st_mode))
        {
            if (filefd >= 0)
            {
                close(filefd);
            }
            filefd = -1;
            status = 404;
        }
    }

    off_t size = 0;
    const char *mime = "application/octet-stream";

    if (status == 200)
    {
        size = st.st_size;
        mime = get_mime_type(fspath);
    }
    else
    {
        /* 404：返回自定义 my404.html */
        char p404[768];
        snprintf(p404, sizeof(p404), "%s/my404.html", g_docroot);
        filefd = open(p404, O_RDONLY);
        if (filefd >= 0 && fstat(filefd, &st) == 0)
        {
            size = st.st_size;
        }
        else
        {
            if (filefd >= 0)
            {
                close(filefd);
            }
            filefd = -1;
        }
        mime = "text/html; charset=utf-8";
    }

    int keep = req->keep_alive;
    char head[512];
    int hlen = snprintf(head, sizeof(head),
                        "HTTP/1.1 %d %s\r\n"
                        "Server: myweb_msx/2.0\r\n"
                        "Content-Type: %s\r\n"
                        "Content-Length: %ld\r\n"
                        "Connection: %s\r\n"
                        "\r\n",
                        status, (status == 200) ? "OK" : "Not Found",
                        mime, (long)size, keep ? "keep-alive" : "close");

    if (send_all(ctx->fd, head, hlen) < 0)
    {
        keep = 0;
    }
    if (filefd >= 0)
    {
        if (send_file_all(ctx->fd, filefd, size) < 0)
        {
            keep = 0;
        }
        close(filefd);
    }

    log_info("%s %s %s -> %d (%ld bytes)",
             ctx->ip, req->method, url, status, (long)size);
    return keep;
}

/* 时间轮到期回调：关闭空闲 Keep-Alive 连接 */
static void on_conn_timeout(int fd, void *arg)
{
    conn_ctx_t *ctx = (conn_ctx_t *)arg;
    epoll_ctl(ctx->epfd, EPOLL_CTL_DEL, fd, NULL);
    close(fd);
    log_info("%s connection timeout, closed", ctx->ip);
    free(ctx);
}

/* 关闭连接：摘除定时器、移出 epoll、释放上下文 */
static void close_conn(conn_ctx_t *ctx)
{
    timer_wheel_remove(ctx->timer, ctx->fd);
    epoll_ctl(ctx->epfd, EPOLL_CTL_DEL, ctx->fd, NULL);
    close(ctx->fd);
    free(ctx);
}

/* 处理完请求后重新挂回 epoll，并重置空闲定时器 */
static void rearm_conn(conn_ctx_t *ctx)
{
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN | EPOLLONESHOT | EPOLLRDHUP;
    ev.data.ptr = ctx;
    epoll_ctl(ctx->epfd, EPOLL_CTL_MOD, ctx->fd, &ev);
    timer_wheel_add(ctx->timer, ctx->fd, KEEPALIVE_IDLE, on_conn_timeout, ctx);
}

/* 工作线程入口：独占处理一个连接上的可读事件 */
static void conn_worker(void *arg)
{
    conn_ctx_t *ctx = (conn_ctx_t *)arg;
    int fd = ctx->fd;

    /* 连接开始处理：取消空闲超时定时器 */
    timer_wheel_remove(ctx->timer, fd);

    /* 1. 尽可能读完当前可读数据（非阻塞 socket，读到 EAGAIN 为止） */
    int peer_closed = 0;
    for (;;)
    {
        ssize_t n = recv(fd, ctx->rbuf + ctx->rlen,
                         sizeof(ctx->rbuf) - (size_t)ctx->rlen, 0);
        if (n > 0)
        {
            ctx->rlen += (int)n;
        }
        else if (n == 0)
        {
            peer_closed = 1; /* 对端发送 FIN：先处理完缓冲区剩余请求再关闭 */
            break;
        }
        else
        {
            if (errno == EINTR)
            {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                break;
            }
            close_conn(ctx);
            return;
        }
    }

    /* 2. 循环处理缓冲区中的所有完整请求（支持 HTTP pipelining） */
    for (;;)
    {
        if (ctx->rlen == 0)
        {
            break; /* 缓冲区已空，等待下一次 epoll 通知 */
        }

        http_request_t req;
        parse_result_t pr = http_parse(ctx->rbuf, ctx->rlen, &req);

        if (pr == PARSE_INCOMPLETE)
        {
            if (ctx->rlen >= (int)sizeof(ctx->rbuf))
            {
                /* 请求头超过缓冲区上限，判定为异常，直接关闭 */
                log_warn("%s request header too large, closing", ctx->ip);
                close_conn(ctx);
                return;
            }
            /* 请求头还没收全：重新监听，等剩余数据到达 */
            if (!peer_closed)
            {
                rearm_conn(ctx);
            }
            else
            {
                close_conn(ctx);
            }
            return;
        }

        if (pr == PARSE_ERROR)
        {
            /* 畸形请求或目录遍历攻击：返回 404 并关闭连接 */
            log_warn("%s bad request or path traversal attempt: %.100s",
                     ctx->ip, ctx->rbuf);
            http_request_t fake;
            memset(&fake, 0, sizeof(fake));
            strcpy(fake.method, "GET");
            strcpy(fake.path, "/__forbidden__");
            fake.keep_alive = 0;
            serve_request(ctx, &fake);
            close_conn(ctx);
            return;
        }

        /* 3. 请求完整：提供服务（发送响应头 + sendfile 文件体） */
        int keep = serve_request(ctx, &req);

        /* 4. 移除已消费的请求头字节，剩余的是管道化的下一个请求 */
        int remain = ctx->rlen - req.header_len;
        if (remain > 0)
        {
            memmove(ctx->rbuf, ctx->rbuf + req.header_len, (size_t)remain);
        }
        ctx->rlen = remain;

        if (!keep)
        {
            close_conn(ctx);
            return;
        }
    }

    /* 5. 缓冲区已空：对端已关闭则收尾，否则重新挂回 epoll 等待下一个请求 */
    if (peer_closed)
    {
        close_conn(ctx);
    }
    else
    {
        rearm_conn(ctx);
    }
}

/* 哨兵指针：用于在 epoll event 中区分监听套接字事件与连接事件
 * （监听事件 data.ptr 指向它，连接事件 data.ptr 指向 conn_ctx_t） */
static char g_listen_marker;

/* 创建监听套接字（非阻塞），返回 fd */
static int create_listen_socket(int port)
{
    int lfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (lfd < 0)
    {
        return -1;
    }

    int reuse = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        close(lfd);
        return -1;
    }
    if (listen(lfd, 1024) < 0)
    {
        close(lfd);
        return -1;
    }
    return lfd;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "用法: %s [-p 端口] [-t 线程数] [-d 网站根目录] [-l 日志文件]\n"
            "  -p  监听端口（默认 80，80 端口需要 root 权限）\n"
            "  -t  工作线程数（默认 = CPU 核数）\n"
            "  -d  网站静态文件根目录（默认当前目录）\n"
            "  -l  访问日志文件路径（默认仅输出到终端）\n",
            prog);
}

int main(int argc, char **argv)
{
    const char *logfile = NULL;

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc)
        {
            g_port = atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc)
        {
            g_threads = atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc)
        {
            snprintf(g_docroot, sizeof(g_docroot), "%s", argv[++i]);
        }
        else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc)
        {
            logfile = argv[++i];
        }
        else
        {
            usage(argv[0]);
            return 1;
        }
    }

    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    if (log_init(logfile, LOG_INFO) != 0)
    {
        fprintf(stderr, "日志初始化失败\n");
        return 1;
    }

    threadpool_t *pool = threadpool_create(g_threads);
    timer_wheel_t *timer = timer_wheel_create(TIMER_SLOTS);
    if (pool == NULL || timer == NULL)
    {
        fprintf(stderr, "线程池/定时器初始化失败\n");
        return 1;
    }

    int lfd = create_listen_socket(g_port);
    if (lfd < 0)
    {
        perror("服务器启动失败（80 端口请尝试 sudo）");
        return 1;
    }

    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0)
    {
        perror("epoll_create1");
        return 1;
    }

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.ptr = &g_listen_marker;
    epoll_ctl(epfd, EPOLL_CTL_ADD, lfd, &ev);

    log_info("myweb v2 启动: port=%d, docroot=%s, threads=%d(0=auto)",
             g_port, g_docroot, g_threads);

    struct epoll_event events[MAX_EVENTS];

    while (!g_stop)
    {
        /* epoll_wait 超时 1000ms：每秒唤醒一次驱动时间轮 */
        int n = epoll_wait(epfd, events, MAX_EVENTS, 1000);
        if (n < 0)
        {
            if (errno == EINTR)
            {
                timer_wheel_tick(timer); /* 信号中断也推进一次 */
                continue;
            }
            log_error("epoll_wait: %s", strerror(errno));
            break;
        }

        for (int i = 0; i < n; i++)
        {
            if (events[i].data.ptr == &g_listen_marker)
            {
                /* 新连接到达：循环 accept 直到 EAGAIN */
                for (;;)
                {
                    struct sockaddr_in caddr;
                    socklen_t clen = sizeof(caddr);
                    int cfd = accept4(lfd, (struct sockaddr *)&caddr, &clen,
                                      SOCK_NONBLOCK | SOCK_CLOEXEC);
                    if (cfd < 0)
                    {
                        if (errno == EINTR)
                        {
                            continue;
                        }
                        break; /* EAGAIN：本轮连接已全部取完 */
                    }

                    conn_ctx_t *ctx = (conn_ctx_t *)calloc(1, sizeof(conn_ctx_t));
                    if (ctx == NULL)
                    {
                        close(cfd);
                        continue;
                    }
                    ctx->fd = cfd;
                    ctx->epfd = epfd;
                    ctx->timer = timer;
                    inet_ntop(AF_INET, &caddr.sin_addr, ctx->ip, sizeof(ctx->ip));

                    /* ONESHOT：事件触发后自动 disarm，保证同一连接不被多线程处理 */
                    struct epoll_event cev;
                    memset(&cev, 0, sizeof(cev));
                    cev.events = EPOLLIN | EPOLLONESHOT | EPOLLRDHUP;
                    cev.data.ptr = ctx;
                    epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &cev);

                    /* 新连接也挂超时定时器，防空连接占用资源 */
                    timer_wheel_add(timer, cfd, KEEPALIVE_IDLE,
                                    on_conn_timeout, ctx);
                }
            }
            else
            {
                /* 连接可读：交给线程池处理 */
                conn_ctx_t *ctx = (conn_ctx_t *)events[i].data.ptr;
                if (threadpool_add(pool, conn_worker, ctx) != 0)
                {
                    log_error("threadpool add failed, closing fd=%d", ctx->fd);
                    close_conn(ctx);
                }
            }
        }

        /* 每秒推进一格时间轮，回收空闲 Keep-Alive 连接 */
        timer_wheel_tick(timer);
    }

    /* 优雅退出 */
    log_info("服务器正在关闭...");
    close(lfd);
    threadpool_destroy(pool);
    timer_wheel_destroy(timer);
    close(epfd);
    log_shutdown();
    return 0;
}
