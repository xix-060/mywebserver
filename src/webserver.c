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
 *   - 发送缓冲暂满（EAGAIN）时保存发送现场，注册 EPOLLOUT 续传，
 *     不再误判为错误关闭连接；
 *   - sendfile 零拷贝发送静态文件（显式偏移支持断点续传）；
 *   - Keep-Alive 长连接 + 时间轮超时回收（tick 由单调时钟严格限定每秒一次，
 *     避免高负载下 epoll 频繁唤醒导致定时器飞转误杀活跃连接）；
 *   - 事件投递前先摘除定时器，消除超时回调与工作线程的 use-after-free；
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
#include <netinet/tcp.h>
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

/* 连接状态：读事件中处理请求；发送缓冲满时转为写状态，由 EPOLLOUT 续传 */
typedef enum
{
    CONN_READING = 0,
    CONN_WRITING = 1
} conn_state_t;

/* 连接上下文：连接生命周期内常驻，存放接收缓冲区（处理半包）
 * 以及响应发送现场（非阻塞发送遇到 EAGAIN 时保存，EPOLLOUT 后续传） */
typedef struct conn_ctx
{
    int fd;
    int epfd;
    timer_wheel_t *timer;
    char ip[INET6_ADDRSTRLEN];
    conn_state_t state;
    char rbuf[READ_BUFFER_SIZE];
    int  rlen;

    /* 当前响应的发送现场 */
    char headbuf[512];   /* 响应头 */
    int  headlen;
    int  headsent;
    int  filefd;         /* 待发送文件（-1 表示无文件体） */
    off_t filetotal;
    off_t filesent;
    int  cur_keep;       /* 当前响应结束后连接是否复用 */
    int  cur_header_len; /* 当前请求头长度，响应发完后才从 rbuf 摘除 */
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

/*
 * 增量发送响应（响应头 + 文件体）。
 * 返回值： 0 = 已全部发送完；1 = 发送缓冲暂满（EAGAIN），需等 EPOLLOUT 续传；
 *         -1 = 连接出错，应关闭。
 * 发送现场保存在 ctx 中，多次调用可断点续传。
 */
static int flush_response(conn_ctx_t *ctx)
{
    /* 1. 发送响应头 */
    while (ctx->headsent < ctx->headlen)
    {
        ssize_t n = send(ctx->fd, ctx->headbuf + ctx->headsent,
                         (size_t)(ctx->headlen - ctx->headsent), MSG_NOSIGNAL);
        if (n > 0)
        {
            ctx->headsent += (int)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
        {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            return 1; /* 等 EPOLLOUT */
        }
        return -1;
    }

    /* 2. sendfile 零拷贝发送文件体（显式偏移，断点续传安全） */
    while (ctx->filesent < ctx->filetotal && ctx->filefd >= 0)
    {
        off_t off = ctx->filesent;
        ssize_t n = sendfile(ctx->fd, ctx->filefd, &off,
                             (size_t)(ctx->filetotal - ctx->filesent));
        if (n > 0)
        {
            ctx->filesent += n;
            continue;
        }
        if (n < 0 && errno == EINTR)
        {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            return 1; /* 等 EPOLLOUT */
        }
        return -1;
    }
    return 0;
}

/*
 * 准备一次响应：定位文件、填充 ctx 中的响应头与发送现场（不做实际发送）。
 * 返回响应完成后连接是否可复用（Keep-Alive）。
 */
static int build_response(conn_ctx_t *ctx, const http_request_t *req)
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

    /* 支持 GET 和 HEAD（HEAD 只回响应头不回 body）；其他方法按 404 处理 */
    int is_head = (strcmp(req->method, "HEAD") == 0);
    if (strcmp(req->method, "GET") != 0 && !is_head)
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

    /* 初始化发送现场 */
    ctx->headlen = snprintf(ctx->headbuf, sizeof(ctx->headbuf),
                            "HTTP/1.1 %d %s\r\n"
                            "Server: myweb_msx/2.0\r\n"
                            "Content-Type: %s\r\n"
                            "Content-Length: %ld\r\n"
                            "Connection: %s\r\n"
                            "\r\n",
                            status, (status == 200) ? "OK" : "Not Found",
                            mime, (long)size,
                            req->keep_alive ? "keep-alive" : "close");
    ctx->headsent = 0;
    ctx->filetotal = size;
    ctx->filesent = 0;
    ctx->cur_keep = req->keep_alive;
    ctx->cur_header_len = 0; /* 由调用方在响应发送完成后填入 */

    if (is_head)
    {
        /* HEAD：只返回响应头（Content-Length 仍为文件大小），不发送 body */
        if (filefd >= 0)
        {
            close(filefd);
        }
        ctx->filefd = -1;
    }
    else
    {
        ctx->filefd = filefd; /* 可能为 -1（404 且自定义页缺失） */
    }

    log_info("%s %s %s -> %d (%ld bytes)",
             ctx->ip, req->method, url, status, (long)size);
    return ctx->cur_keep;
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

/* 关闭连接：兜底摘除定时器、移出 epoll、释放上下文。
 * 注意：定时器在事件投递前已由主线程摘除，这里的 remove 是防御性空操作；
 *       仅在 CONN_WRITING 等待 EPOLLOUT 期间被超时回调关闭的连接不会走到这里。 */
static void close_conn(conn_ctx_t *ctx)
{
    if (ctx->filefd >= 0)
    {
        close(ctx->filefd);
        ctx->filefd = -1;
    }
    timer_wheel_remove(ctx->timer, ctx->fd);
    epoll_ctl(ctx->epfd, EPOLL_CTL_DEL, ctx->fd, NULL);
    close(ctx->fd);
    free(ctx);
}

/* 重新监听读事件（等待下一个请求），并重置空闲超时。
 * 顺序必须是「先加定时器，再武装 epoll」：若先武装 epoll，主线程可能
 * 在定时器加入前就返回新事件并执行 remove（摘了个空），导致同一连接
 * 残留两个定时器节点，到期回调会 free 掉正在被其他线程处理的 ctx。 */
static void rearm_read(conn_ctx_t *ctx)
{
    timer_wheel_add(ctx->timer, ctx->fd, KEEPALIVE_IDLE, on_conn_timeout, ctx);
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN | EPOLLONESHOT | EPOLLRDHUP;
    ev.data.ptr = ctx;
    epoll_ctl(ctx->epfd, EPOLL_CTL_MOD, ctx->fd, &ev);
}

/* 重新监听写事件（发送缓冲暂满，等可写后续传）；顺序同理：先定时器后武装 */
static void rearm_write(conn_ctx_t *ctx)
{
    timer_wheel_add(ctx->timer, ctx->fd, KEEPALIVE_IDLE, on_conn_timeout, ctx);
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLOUT | EPOLLONESHOT;
    ev.data.ptr = ctx;
    epoll_ctl(ctx->epfd, EPOLL_CTL_MOD, ctx->fd, &ev);
}

/* 一个响应完整发送后的收尾：关闭文件、从 rbuf 摘除已消费请求头；
 * 返回 0 表示连接可继续处理下一请求，-1 表示连接应关闭 */
static int finish_response(conn_ctx_t *ctx)
{
    if (ctx->filefd >= 0)
    {
        close(ctx->filefd);
        ctx->filefd = -1;
    }
    int keep = ctx->cur_keep;
    int remain = ctx->rlen - ctx->cur_header_len;
    if (remain < 0)
    {
        remain = 0;
    }
    if (remain > 0)
    {
        memmove(ctx->rbuf, ctx->rbuf + ctx->cur_header_len, (size_t)remain);
    }
    ctx->rlen = remain;
    ctx->state = CONN_READING;
    return keep ? 0 : -1;
}

/* 工作线程入口：EPOLLONESHOT 保证独占连接，处理读/写就绪事件 */
static void conn_worker(void *arg)
{
    conn_ctx_t *ctx = (conn_ctx_t *)arg;
    int fd = ctx->fd;
    /* 定时器已在主线程投递事件前摘除，此处无需再操作定时器 */

    /* ===== 写续传：上次发送缓冲暂满，EPOLLOUT 触发后续传 ===== */
    if (ctx->state == CONN_WRITING)
    {
        int r = flush_response(ctx);
        if (r == 1)
        {
            rearm_write(ctx); /* 仍然不可写，继续等 */
            return;
        }
        if (r < 0)
        {
            close_conn(ctx);
            return;
        }
        if (finish_response(ctx) != 0) /* 响应发送完成 */
        {
            close_conn(ctx); /* 对端要求关闭或出错 */
            return;
        }
        /* 发送完成：落到下面的读流程，处理管道化残留/新数据 */
    }

    /* ===== 读流程 ===== */

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
            break; /* 缓冲区已空 */
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
            /* 请求头还没收全：等剩余数据到达；对端已关闭则收尾 */
            if (!peer_closed)
            {
                rearm_read(ctx);
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
            build_response(ctx, &fake);
            ctx->cur_header_len = ctx->rlen; /* 丢弃全部脏数据 */
            int r = flush_response(ctx);
            if (r == 1)
            {
                ctx->state = CONN_WRITING; /* 罕见：404 页很大时等 EPOLLOUT 后关闭 */
                rearm_write(ctx);
            }
            else
            {
                close_conn(ctx);
            }
            return;
        }

        /* 3. 构建并增量发送响应 */
        build_response(ctx, &req);
        ctx->cur_header_len = req.header_len;
        int r = flush_response(ctx);
        if (r == 1)
        {
            /* 发送缓冲暂满：保存现场，等 EPOLLOUT 续传（EPOLLONESHOT 独占） */
            ctx->state = CONN_WRITING;
            rearm_write(ctx);
            return;
        }
        if (r < 0 || finish_response(ctx) != 0)
        {
            close_conn(ctx);
            return;
        }
    }

    /* 4. 缓冲区已空：对端已关闭则收尾，否则重新挂回 epoll 等待下一请求 */
    if (peer_closed)
    {
        close_conn(ctx);
    }
    else
    {
        rearm_read(ctx);
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
            "用法: %s [-p 端口] [-t 线程数] [-d 网站根目录] [-l 日志文件] [-q]\n"
            "  -p  监听端口（默认 80，80 端口需要 root 权限）\n"
            "  -t  工作线程数（默认 = CPU 核数）\n"
            "  -d  网站静态文件根目录（默认当前目录）\n"
            "  -l  访问日志文件路径（默认仅输出到终端）\n"
            "  -q  静默模式：只记录 WARN/ERROR（压测时使用，避免 INFO 刷屏）\n",
            prog);
}

int main(int argc, char **argv)
{
    const char *logfile = NULL;
    int log_level = LOG_INFO;

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
        else if (strcmp(argv[i], "-q") == 0)
        {
            log_level = LOG_WARN;
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

    if (log_init(logfile, log_level) != 0)
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

    /* 时间轮节拍：tick 必须严格「每秒一次」。epoll_wait 在高负载下每秒会
     * 被事件唤醒成千上万次，若每次唤醒都 tick，时间轮会飞转、定时器瞬间
     * 到期误杀活跃连接——故用单调时钟限制节拍。 */
    struct timespec last_tick;
    clock_gettime(CLOCK_MONOTONIC, &last_tick);

    while (!g_stop)
    {
        /* epoll_wait 超时 1000ms：空闲时每秒唤醒一次驱动时间轮 */
        int n = epoll_wait(epfd, events, MAX_EVENTS, 1000);
        if (n < 0)
        {
            if (errno == EINTR)
            {
                /* 信号中断：由下方统一按真实时间判断是否 tick */
            }
            else
            {
                log_error("epoll_wait: %s", strerror(errno));
                break;
            }
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

                    /* 禁用 Nagle 算法：HTTP 响应头/文件尾包等小数据立即发送。
                     * 否则 Nagle 与对端 delayed ACK（最长40ms）互相等待，
                     * 会给每个请求引入约 40ms 的固定延迟 */
                    int nodelay = 1;
                    setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY,
                               &nodelay, sizeof(nodelay));

                    conn_ctx_t *ctx = (conn_ctx_t *)calloc(1, sizeof(conn_ctx_t));
                    if (ctx == NULL)
                    {
                        close(cfd);
                        continue;
                    }
                    ctx->fd = cfd;
                    ctx->epfd = epfd;
                    ctx->timer = timer;
                    ctx->state = CONN_READING;
                    ctx->filefd = -1;
                    inet_ntop(AF_INET, &caddr.sin_addr, ctx->ip, sizeof(ctx->ip));

                    /* 先挂超时定时器，再加入 epoll 武装（与 rearm 顺序一致，
                     * 保证事件可能返回时定时器必然存在） */
                    timer_wheel_add(timer, cfd, KEEPALIVE_IDLE,
                                    on_conn_timeout, ctx);

                    /* ONESHOT：事件触发后自动 disarm，保证同一连接不被多线程处理 */
                    struct epoll_event cev;
                    memset(&cev, 0, sizeof(cev));
                    cev.events = EPOLLIN | EPOLLONESHOT | EPOLLRDHUP;
                    cev.data.ptr = ctx;
                    epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &cev);
                }
            }
            else
            {
                /* 连接可读/可写：先摘除超时定时器，再投递给线程池。
                 * 这样工作线程独占连接期间时间轮绝不可能触发关闭，
                 * 彻底消除 "回调 free(ctx) 与工作线程并发" 的 use-after-free。 */
                conn_ctx_t *ctx = (conn_ctx_t *)events[i].data.ptr;
                timer_wheel_remove(timer, ctx->fd);
                if (threadpool_add(pool, conn_worker, ctx) != 0)
                {
                    log_error("threadpool add failed, closing fd=%d", ctx->fd);
                    close_conn(ctx);
                }
            }
        }

        /* 按真实流逝时间推进时间轮：最多每秒一格，空闲时 epoll_wait
         * 1 秒超时、繁忙时事件唤醒，两种情况节拍都准确 */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed_ms = (now.tv_sec - last_tick.tv_sec) * 1000
                        + (now.tv_nsec - last_tick.tv_nsec) / 1000000;
        if (elapsed_ms >= 1000)
        {
            timer_wheel_tick(timer);
            last_tick = now;
        }
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
