/*
 * myweb.c — 多线程 HTTP/1.1 静态文件服务器
 *
 * 功能：多线程并发处理、GET 请求解析、MIME 类型映射、
 *       自定义 404 页面、目录遍历防护、访问日志
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>

#define PORT      80      /* 监听端口，80 端口需要 sudo 权限 */
#define DOC_ROOT  "."     /* 网站根目录：在仓库目录下运行即可 */

/* 传给线程的客户端信息 */
typedef struct
{
    int  fd;             /* 客户端套接字 */
    char ip[16];         /* 客户端 IP（IPv4 点分十进制） */
} client_info_t;

/* 创建监听套接字，成功返回 sockfd，失败返回 -1 */
int creat_socket()
{
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1)
    {
        return -1;
    }

    /* 允许服务器重启后立即重新绑定端口，避免 Address already in use */
    int reuse = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in saddr;
    memset(&saddr, 0, sizeof(saddr));
    saddr.sin_family      = AF_INET;
    saddr.sin_port        = htons(PORT);
    saddr.sin_addr.s_addr = htonl(INADDR_ANY); /* 监听所有网卡，可通过本机 IP 访问 */

    int res = bind(sockfd, (struct sockaddr *)&saddr, sizeof(saddr));
    if (res == -1)
    {
        return -1;
    }

    res = listen(sockfd, 10);
    if (res == -1)
    {
        return -1;
    }

    return sockfd;
}

/* 根据文件扩展名返回 MIME 类型 */
const char *get_mime_type(const char *path)
{
    const char *ext = strrchr(path, '.');
    if (ext == NULL)
    {
        return "application/octet-stream";
    }

    if (strcmp(ext, ".html") == 0 || strcmp(ext, ".htm") == 0) return "text/html";
    if (strcmp(ext, ".css")  == 0) return "text/css";
    if (strcmp(ext, ".js")   == 0) return "application/javascript";
    if (strcmp(ext, ".jpg")  == 0 || strcmp(ext, ".jpeg") == 0) return "image/jpeg";
    if (strcmp(ext, ".png")  == 0) return "image/png";
    if (strcmp(ext, ".gif")  == 0) return "image/gif";
    if (strcmp(ext, ".ico")  == 0) return "image/x-icon";

    return "application/octet-stream";
}

/* 访问日志：时间 客户端IP 请求路径 状态码 */
void log_access(const char *ip, const char *path, int status)
{
    char timestr[64] = {0};
    time_t now = time(NULL);
    struct tm tmval;
    localtime_r(&now, &tmval); /* 线程安全版本 */
    strftime(timestr, sizeof(timestr), "%Y-%m-%d %H:%M:%S", &tmval);

    printf("[%s] %s GET %s -> %d\n", timestr, ip, path, status);
    fflush(stdout);
}

/* 发送 404 响应：优先返回自定义 my404.html，失败则回退内置页面 */
void send_404(int fd)
{
    char path[512] = {0};
    snprintf(path, sizeof(path), "%s/my404.html", DOC_ROOT);

    int fdfile = open(path, O_RDONLY);
    if (fdfile == -1)
    {
        /* 内置的简单 404 页面（兜底） */
        const char *body = "<html><body><h1>404 Not Found</h1></body></html>";
        char head[512] = {0};
        snprintf(head, sizeof(head),
                 "HTTP/1.1 404 Not Found\r\n"
                 "Server: myweb_msx\r\n"
                 "Content-Type: text/html\r\n"
                 "Content-Length: %zu\r\n"
                 "Connection: close\r\n"
                 "\r\n"
                 "%s", strlen(body), body);
        send(fd, head, strlen(head), 0);
        return;
    }

    int size = (int)lseek(fdfile, 0, SEEK_END); /* 求文件长度 */
    lseek(fdfile, 0, SEEK_SET);

    char head[512] = {0};
    snprintf(head, sizeof(head),
             "HTTP/1.1 404 Not Found\r\n"
             "Server: myweb_msx\r\n"
             "Content-Type: text/html\r\n"
             "Content-Length: %d\r\n"
             "Connection: close\r\n"
             "\r\n", size);
    send(fd, head, strlen(head), 0);

    char data[4096] = {0};
    int num = 0;
    while ((num = read(fdfile, data, sizeof(data))) > 0)
    {
        send(fd, data, num, 0);
    }
    close(fdfile);
}

/* 读取整个请求，解析后返回文件内容 */
void *worker(void *arg)
{
    client_info_t *info = (client_info_t *)arg;
    int fd = info->fd;

    /* 1. 接收 HTTP 请求 */
    char buff[1024] = {0};
    int n = recv(fd, buff, sizeof(buff) - 1, 0);
    if (n <= 0)
    {
        close(fd);
        free(info);
        return NULL;
    }

    /* 2. 解析请求行：方法 URI 协议版本 */
    char method[16] = {0};
    char uri[256]   = {0};
    sscanf(buff, "%15s %255s", method, uri);

    /* 3. 仅支持 GET 方法，其他方法返回 404 */
    if (strcmp(method, "GET") != 0)
    {
        send_404(fd);
        log_access(info->ip, uri, 404);
        close(fd);
        free(info);
        return NULL;
    }

    /* 去掉 URI 中的查询字符串（? 之后的字段） */
    char *q = strchr(uri, '?');
    if (q != NULL)
    {
        *q = '\0';
    }

    /* 4. 安全防护：拒绝包含 .. 的路径，防止目录遍历攻击 */
    if (strstr(uri, "..") != NULL)
    {
        send_404(fd);
        log_access(info->ip, uri, 404);
        close(fd);
        free(info);
        return NULL;
    }

    /* 5. 请求根路径时返回默认首页 */
    if (strcmp(uri, "/") == 0)
    {
        strcpy(uri, "/index.html");
    }

    /* 6. 将 URI 映射为本地文件路径 */
    char path[512] = {0};
    snprintf(path, sizeof(path), "%s%s", DOC_ROOT, uri);

    int fdfile = open(path, O_RDONLY);
    if (fdfile == -1)
    {
        /* 7. 资源不存在：返回自定义 404 页面 */
        send_404(fd);
        log_access(info->ip, uri, 404);
    }
    else
    {
        int size = (int)lseek(fdfile, 0, SEEK_END); /* 求文件长度 */
        lseek(fdfile, 0, SEEK_SET);

        /* 发送响应头：状态行 + Server + Content-Type + Content-Length */
        char head[512] = {0};
        snprintf(head, sizeof(head),
                 "HTTP/1.1 200 OK\r\n"
                 "Server: myweb_msx\r\n"
                 "Content-Type: %s\r\n"
                 "Content-Length: %d\r\n"
                 "Connection: close\r\n"
                 "\r\n", get_mime_type(path), size);
        send(fd, head, strlen(head), 0);

        /* 循环读取文件内容发送给客户端 */
        char data[4096] = {0};
        int num = 0;
        while ((num = read(fdfile, data, sizeof(data))) > 0)
        {
            send(fd, data, num, 0);
        }
        close(fdfile);

        log_access(info->ip, uri, 200);
    }

    close(fd); /* 短连接：发送完即关闭 */
    free(info);
    return NULL;
}

int main()
{
    /* 忽略 SIGPIPE：客户端提前断开时，send 不会导致进程退出 */
    signal(SIGPIPE, SIG_IGN);

    int sockfd = creat_socket();
    if (sockfd == -1)
    {
        perror("服务器启动失败（若使用 80 端口，请尝试 sudo ./myweb）");
        exit(1);
    }

    printf("myweb 服务器已启动，监听端口 %d ...\n", PORT);
    printf("访问地址：http://127.0.0.1\n");

    struct sockaddr_in caddr;
    socklen_t len = 0;

    while (1)
    {
        len = sizeof(caddr);
        int c = accept(sockfd, (struct sockaddr *)&caddr, &len);
        if (c < 0)
        {
            continue;
        }

        /* 打包客户端信息，交给子线程处理 */
        client_info_t *info = (client_info_t *)malloc(sizeof(client_info_t));
        if (info == NULL)
        {
            close(c);
            continue;
        }
        info->fd = c;
        strncpy(info->ip, inet_ntoa(caddr.sin_addr), sizeof(info->ip) - 1);
        info->ip[sizeof(info->ip) - 1] = '\0';

        /* 为每个连接创建独立线程，并设置为分离状态，主线程无需回收资源 */
        pthread_t tid;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&tid, &attr, worker, info) != 0)
        {
            free(info);
            close(c);
            continue;
        }
        pthread_attr_destroy(&attr);
    }

    close(sockfd);
    exit(0);
}
