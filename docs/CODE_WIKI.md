# Code Wiki — MyWebServer 项目技术文档

> **项目名称**：MyWebServer — epoll 多线程 HTTP 服务器 & 小花仙主题网站
> **语言/技术栈**：C11 / Linux epoll / POSIX Thread / sendfile / 时间轮
> **版本**：v2（当前版本），v1（legacy 保留对比）
> **作者**：[xix-060](https://github.com/xix-060)

---

## 目录

- [1. 项目概述](#1-项目概述)
- [2. 项目整体架构](#2-项目整体架构)
- [3. 目录结构与文件说明](#3-目录结构与文件说明)
- [4. 主要模块职责](#4-主要模块职责)
- [5. 关键数据结构](#5-关键数据结构)
- [6. 关键函数说明](#6-关键函数说明)
- [7. 模块间依赖关系](#7-模块间依赖关系)
- [8. 核心流程详解](#8-核心流程详解)
- [9. 并发与线程安全模型](#9-并发与线程安全模型)
- [10. 安全防护机制](#10-安全防护机制)
- [11. 性能优化与压测](#11-性能优化与压测)
- [12. 静态网站资源](#12-静态网站资源)
- [13. 项目运行方式](#13-项目运行方式)
- [14. 配置参数说明](#14-配置参数说明)
- [15. v1 与 v2 版本对比](#15-v1-与-v2-版本对比)
- [16. 待改进方向](#16-待改进方向)

---

## 1. 项目概述

本项目是一个基于 C 语言从零实现的高并发 HTTP/1.1 静态文件服务器，采用 **epoll Reactor + 线程池** 并发模型，支持 Keep-Alive 长连接、sendfile 零拷贝、增量 HTTP 解析、时间轮超时管理与异步日志。同时依托该服务器部署了一个小花仙动漫主题的静态网站。

项目经历了两个版本的演进：
- **v1**（`legacy/myweb_v1_thread.c`）：每连接一个 pthread 的基础多线程版本
- **v2**（`src/` 目录下模块化代码）：epoll + 线程池 + 非阻塞 I/O 的 Reactor 版本

### 核心能力一览

| 能力 | v1 | v2 |
|------|:--:|:--:|
| 并发模型 | 每连接一线程 | epoll Reactor + 固定线程池 |
| I/O 模式 | 阻塞 | 非阻塞 + EPOLLONESHOT |
| Keep-Alive | 不支持 | 支持（15s 超时回收） |
| 文件传输 | read + send | sendfile 零拷贝 |
| HTTP 解析 | sscanf 请求行 | 增量解析器（半包/粘包/pipelining） |
| 定时器 | 无 | 单轮时间轮 |
| 日志 | printf 同步 | 异步日志（后台线程批量落盘） |
| 安全防护 | `..` 检查 | URL 解码 + `..` 检查（防 `%2e%2e` 绕过） |
| 优雅退出 | 无 | SIGINT/SIGTERM 信号优雅退出 |
| 配置化 | 硬编码 | 命令行参数（端口/线程/目录/日志） |

---

## 2. 项目整体架构

### 2.1 并发模型全景图

```
              ┌───────────────────── 主线程（Reactor）─────────────────────┐
              │  epoll_wait                                               │
              │    ├─ 监听套接字可读 → accept4（非阻塞）→ 注册 EPOLLONESHOT │
              │    ├─ 连接可读     → 摘除定时器 → 投递任务到线程池          │
              │    └─ 每秒 tick    → 时间轮推进，回收空闲 Keep-Alive 连接   │
              └───────────────────────────┬───────────────────────────────┘
                                          │ 任务队列（mutex + cond）
                          ┌───────────────┼───────────────┐
                          ▼               ▼               ▼
                     工作线程 1       工作线程 2  ...  工作线程 N
                     recv → 解析 → build_response → flush_response → rearm
```

### 2.2 架构分层

```
┌───────────────────────────────────────────────────┐
│                   静态网站层                       │
│   index.html  collection.html  my404.html  images/ │
├───────────────────────────────────────────────────┤
│               webserver.c（主程序）                  │
│   epoll 事件循环 · accept · 事件分发 · 连接管理      │
├──────────┬──────────┬──────────┬──────────────────┤
│threadpool│http_parser│timer_wheel│     log         │
│  线程池  │ HTTP解析  │  时间轮   │  异步日志        │
└──────────┴──────────┴──────────┴──────────────────┘
│              Linux 内核 API                        │
│  epoll · sendfile · non-blocking socket · pthread  │
└───────────────────────────────────────────────────┘
```

### 2.3 请求处理流程

```
客户端连接
  │
  ▼
主线程 epoll_wait 返回
  │
  ├─ 监听套接字可读？
  │   ├─ accept4 → 创建 conn_ctx_t
  │   ├─ timer_wheel_add（挂超时定时器）
  │   └─ epoll_ctl ADD（EPOLLIN | EPOLLONESHOT）
  │
  └─ 连接套接字就绪？
      ├─ timer_wheel_remove（先摘定时器，消除竞态）
      ├─ threadpool_add（投递 conn_worker 到线程池）
      │
      └─ 工作线程执行 conn_worker：
          │
          ├─ CONN_WRITING 状态？→ flush_response 续传
          │
          └─ CONN_READING 状态：
              ├─ recv 循环读到 EAGAIN
              ├─ 循环解析处理所有完整请求（pipelining）
              │   ├─ http_parse → PARSE_INCOMPLETE → rearm_read 等剩余数据
              │   ├─ http_parse → PARSE_ERROR → 返回 404 并关闭
              │   └─ http_parse → PARSE_COMPLETE:
              │       ├─ build_response（定位文件、填充响应头）
              │       ├─ flush_response（增量发送响应头 + sendfile 文件体）
              │       │   ├─ 发送完毕 → finish_response → 处理下一请求
              │       │   ├─ EAGAIN → CONN_WRITING → rearm_write
              │       │   └─ 出错 → close_conn
              │       └─ finish_response（关闭文件 fd、摘除已消费请求头）
              │
              └─ 缓冲区空 → rearm_read（等待下一请求）或 close_conn
```

---

## 3. 目录结构与文件说明

```
mywebserver/
├── src/                           # v2 服务器源码（模块化）
│   ├── webserver.c                #   主程序：epoll Reactor 事件循环、连接管理（727行）
│   ├── threadpool.c               #   线程池实现（156行）
│   ├── threadpool.h               #   线程池头文件
│   ├── http_parser.c              #   增量式 HTTP 请求头解析器（138行）
│   ├── http_parser.h              #   解析器头文件
│   ├── timer_wheel.c              #   单轮时间轮定时器（201行）
│   ├── timer_wheel.h              #   时间轮头文件
│   ├── log.c                      #   异步日志实现（170行）
│   └── log.h                      #   日志头文件
├── legacy/
│   └── myweb_v1_thread.c          # v1：每连接一线程的基础版本（294行，演进对比用）
├── index.html                     # 网站主页（拉贝尔大陆介绍）
├── collection.html                # 花仙精灵王图鉴
├── my404.html                     # 自定义 404 页面（含倒计时跳转）
├── images/                        # 角色和精灵王图片（12 张 jpg）
│   ├── xiaanan.jpg                #   夏安安
│   ├── kukulu.jpg                 #   库库鲁
│   ├── qianhan.jpg                #   千韩
│   ├── chun.jpg                   #   椿（山茶花精灵王）
│   ├── shanmeng.jpg               #   山梦（瑞香花精灵王）
│   ├── hongyu.jpg                 #   红雨（桃花精灵王）
│   ├── margaret.jpg               #   玛格丽特（小雏菊精灵王）
│   ├── sumu.jpg                   #   苏睦（紫荆花精灵王）
│   ├── manzhushahua.jpg           #   曼珠沙华（彼岸花精灵王）
│   ├── junying.jpg                #   君影（铃兰精灵王）
│   ├── muhui.jpg                  #   穆回（勿忘我精灵王）
│   ├── yuji.jpg                   #   御姬（紫藤精灵王）
│   └── alice.jpg                  #   爱丽丝（鸢尾花精灵王）
├── docs/
│   ├── performance-notes.md       #   性能调优与并发问题排查笔记
│   └── 项目通俗讲解.md             #   大白话版项目讲解
├── Makefile                       # 构建脚本
├── benchmark.sh                   # wrk/ab 压测脚本
├── README.md                      # 项目说明
└── .gitignore                     # 忽略 myweb（二进制）、*.o、*.zip、*.log 等
```

---

## 4. 主要模块职责

### 4.1 webserver.c — 主程序（Reactor + 连接管理）

| 职责 | 说明 |
|------|------|
| 事件循环 | 主线程 `epoll_wait` 统一接收连接与分发事件 |
| 连接接受 | 监听套接字可读时循环 `accept4` 到 EAGAIN |
| 事件分发 | 连接就绪时先摘定时器再投递线程池任务 |
| 时间轮驱动 | 每秒（单调时钟限制）tick 一次，回收空闲连接 |
| 响应构建 | `build_response` 定位文件、填充响应头、初始化发送现场 |
| 增量发送 | `flush_response` 循环 send 响应头 + sendfile 文件体 |
| 状态机 | `CONN_READING` ↔ `CONN_WRITING` 两态切换 |
| 配置解析 | 命令行参数 `-p -t -d -l -q` |
| 信号处理 | SIGINT/SIGTERM 优雅退出，SIGPIPE 忽略 |

### 4.2 threadpool.c/.h — 固定大小线程池

| 职责 | 说明 |
|------|------|
| 线程管理 | 创建固定数量工作线程，默认按 CPU 核数 |
| 任务队列 | 无界链表队列，主线程追加到队尾 |
| 同步机制 | 互斥锁 + 条件变量（生产者-消费者模型） |
| 任务执行 | 工作线程从队头取任务，锁外执行业务逻辑 |
| 优雅关闭 | shutdown 标志 + broadcast 唤醒所有线程，等队列排空后 join |

### 4.3 http_parser.c/.h — 增量式 HTTP 请求解析器

| 职责 | 说明 |
|------|------|
| 完整性判断 | 定位 `\r\n\r\n` 判断请求头是否收全（处理半包） |
| 请求行解析 | sscanf 提取 METHOD / PATH / VERSION |
| URL 解码 | `%xx` 转义还原（在安全检查之前执行） |
| 安全检查 | 解码后检测 `..` 防止目录遍历绕过 |
| 查询串剥离 | 去掉 `?` 之后的查询参数 |
| 头部解析 | 逐行识别 `Connection: keep-alive/close` |
| 管道化支持 | 返回 `header_len` 供调用方移除已消费请求 |

### 4.4 timer_wheel.c/.h — 单轮时间轮定时器

| 职责 | 说明 |
|------|------|
| 超时管理 | Keep-Alive 空闲连接 15 秒自动回收 |
| 节拍驱动 | 主线程每秒 tick 推进一格 |
| 持锁互斥 | add/remove/tick 全程持锁，跨线程安全 |
| 去重保证 | add 前遍历所有槽摘除旧节点，保证同一 fd 至多一个节点 |
| 回调执行 | 到期回调在锁外执行（可能 close fd + free ctx） |

### 4.5 log.c/.h — 异步日志

| 职责 | 说明 |
|------|------|
| 异步写入 | 业务线程格式化后入队，后台线程批量落盘 |
| 双输出 | 同时写 stderr 和日志文件（可选） |
| 日志级别 | DEBUG / INFO / WARN / ERROR，可配置过滤 |
| 宏接口 | `log_debug/log_info/log_warn/log_error` 自动附加文件名和行号 |
| 优雅关闭 | shutdown 标志唤醒后台线程，刷盘后 join |

---

## 5. 关键数据结构

### 5.1 conn_ctx_t — 连接上下文（webserver.c）

连接生命周期内常驻，存放接收缓冲区与响应发送现场。

```c
typedef struct conn_ctx
{
    int fd;                         // 客户端套接字描述符
    int epfd;                       // epoll 实例 fd
    timer_wheel_t *timer;           // 时间轮指针
    char ip[INET6_ADDRSTRLEN];     // 客户端 IP
    conn_state_t state;             // CONN_READING 或 CONN_WRITING
    char rbuf[READ_BUFFER_SIZE];    // 接收缓冲区（65536 字节，处理半包）
    int  rlen;                      // rbuf 中已收数据长度

    // 当前响应的发送现场
    char headbuf[512];              // 响应头
    int  headlen;                   // 响应头总长度
    int  headsent;                  // 已发送的响应头字节数
    int  filefd;                     // 待发送文件 fd（-1 表示无文件体）
    off_t filetotal;                // 文件总大小
    off_t filesent;                 // 已发送文件字节数（断点续传偏移）
    int  cur_keep;                  // 当前响应结束后是否复用连接
    int  cur_header_len;            // 当前请求头长度，响应发完后从 rbuf 摘除
} conn_ctx_t;
```

### 5.2 conn_state_t — 连接状态枚举（webserver.c）

```c
typedef enum
{
    CONN_READING = 0,   // 读事件中处理请求
    CONN_WRITING = 1    // 发送缓冲暂满，等待 EPOLLOUT 续传
} conn_state_t;
```

### 5.3 http_request_t — HTTP 请求解析结果（http_parser.h）

```c
typedef struct
{
    char method[8];      // 请求方法，如 "GET"
    char path[512];      // URL 解码、去掉查询串后的请求路径
    int  keep_alive;     // 是否保持长连接
    int  header_len;     // 请求头总字节数（含 \r\n\r\n）
} http_request_t;
```

### 5.4 parse_result_t — 解析结果枚举（http_parser.h）

```c
typedef enum
{
    PARSE_INCOMPLETE = 0,  // 请求头未收全，等待更多数据
    PARSE_COMPLETE   = 1,  // 请求头解析完成
    PARSE_ERROR      = 2   // 请求格式错误或路径非法
} parse_result_t;
```

### 5.5 threadpool_t — 线程池结构（threadpool.c，内部结构体）

```c
struct threadpool_t
{
    pthread_mutex_t lock;       // 保护任务队列的互斥锁
    pthread_cond_t  notify;     // 唤醒空闲工作线程的条件变量
    pthread_t *threads;         // 工作线程数组
    task_t *head;               // 队头：工作线程取任务
    task_t *tail;               // 队尾：主线程放任务
    int thread_count;           // 工作线程数量
    int shutdown;               // 关闭标志
};
```

### 5.6 task_t — 任务节点（threadpool.c，内部结构体）

```c
typedef struct task
{
    void (*func)(void *);    // 任务函数指针
    void *arg;               // 任务参数
    struct task *next;       // 链表指针
} task_t;
```

### 5.7 timer_wheel_t — 时间轮结构（timer_wheel.c，内部结构体）

```c
struct timer_wheel_t
{
    timer_node_t **slots;     // 槽位数组，每个槽指向链表头
    int slot_count;           // 总槽位数（默认 60，一圈 60 秒）
    int cur_slot;             // 当前槽位
    pthread_mutex_t lock;     // 全局互斥锁
};
```

### 5.8 timer_node_t — 定时器节点（timer_wheel.c，内部结构体）

```c
typedef struct timer_node
{
    int fd;                   // 关联的套接字 fd
    int rotation;             // 剩余圈数（>0 未到期，减一留下；==0 到期触发）
    timer_callback cb;        // 到期回调函数
    void *arg;                // 回调参数（conn_ctx_t 指针）
    struct timer_node *next;  // 链表指针
} timer_node_t;
```

### 5.9 日志全局结构（log.c，内部）

```c
static struct
{
    log_node_t *head;         // 日志队列头
    log_node_t *tail;         // 日志队列尾
    pthread_mutex_t lock;     // 互斥锁
    pthread_cond_t  cond;     // 条件变量
    pthread_t tid;            // 后台日志线程 ID
    int shutdown;             // 关闭标志
    FILE *fp;                 // 日志文件指针（NULL 表示只输出 stderr）
    int level;                // 当前日志级别
} g_log;
```

---

## 6. 关键函数说明

### 6.1 webserver.c

#### `main` — 程序入口

```c
int main(int argc, char **argv)
```

- 解析命令行参数（`-p/-t/-d/-l/-q`）
- 注册信号处理（SIGPIPE 忽略，SIGINT/SIGTERM 设停止标志）
- 初始化日志、线程池、时间轮、监听套接字、epoll
- 进入主事件循环：epoll_wait → 事件分发 → 时间轮 tick
- 优雅退出：关闭监听 fd、销毁线程池/时间轮/epoll、关闭日志

#### `create_listen_socket` — 创建监听套接字

```c
static int create_listen_socket(int port)
```

- 创建非阻塞 + CLOEXEC 的 TCP socket
- 设置 `SO_REUSEADDR` 允许端口复用
- bind 到 `INADDR_ANY:port`，listen backlog 1024

#### `conn_worker` — 工作线程入口

```c
static void conn_worker(void *arg)
```

工作线程对独占连接（EPOLLONESHOT 保证）执行的核心逻辑：

1. **写续传**：若 `state == CONN_WRITING`，调用 `flush_response` 继续上次中断的发送
2. **读流程**：循环 `recv` 到 EAGAIN，处理对端 FIN
3. **请求循环**：循环调用 `http_parse` 处理缓冲区中所有完整请求（pipelining）
   - `PARSE_INCOMPLETE`：请求头未收全，`rearm_read` 等剩余数据
   - `PARSE_ERROR`：畸形请求/目录遍历，返回 404 并关闭
   - `PARSE_COMPLETE`：`build_response` → `flush_response` → `finish_response`
4. **收尾**：缓冲区空且对端已关闭则 `close_conn`，否则 `rearm_read`

#### `build_response` — 构建响应

```c
static int build_response(conn_ctx_t *ctx, const http_request_t *req)
```

- 根据请求路径定位文件（根路径映射为 `/index.html`）
- 仅支持 GET 和 HEAD 方法
- 文件不存在或方法不支持时返回自定义 `my404.html`
- 填充 `ctx->headbuf`（响应头：状态行、Server、Content-Type、Content-Length、Connection）
- 初始化发送现场（`headsent=0, filesent=0, filefd=filefd`）
- HEAD 方法：关闭文件 fd，不发送 body

#### `flush_response` — 增量发送响应

```c
static int flush_response(conn_ctx_t *ctx)
```

返回值：`0`=已全部发送完；`1`=发送缓冲暂满（EAGAIN），需等 EPOLLOUT 续传；`-1`=连接出错。

- **阶段1**：循环 `send` 发送响应头，处理 EINTR/EAGAIN/错误
- **阶段2**：循环 `sendfile` 零拷贝发送文件体，显式偏移支持断点续传

#### `finish_response` — 响应发送完成收尾

```c
static int finish_response(conn_ctx_t *ctx)
```

- 关闭文件 fd
- 从 `rbuf` 中移除已消费的请求头（`memmove` 前移剩余数据）
- 将状态切回 `CONN_READING`
- 返回 `0` 表示连接可继续（Keep-Alive），`-1` 表示应关闭

#### `close_conn` — 关闭连接

```c
static void close_conn(conn_ctx_t *ctx)
```

- 关闭文件 fd（若有）
- 摘除定时器（防御性空操作）
- 从 epoll 移除
- 关闭套接字 fd
- 释放 `conn_ctx_t`

#### `rearm_read` / `rearm_write` — 重新武装 epoll

```c
static void rearm_read(conn_ctx_t *ctx)
static void rearm_write(conn_ctx_t *ctx)
```

**关键时序**：先 `timer_wheel_add`（挂定时器），再 `epoll_ctl MOD`（武装 epoll）。
保证"事件可能被主线程返回"的那一刻，定时器一定已存在，主线程的 remove 一定摘得掉。

#### `get_mime_type` — MIME 类型映射

```c
static const char *get_mime_type(const char *path)
```

支持 `.html/.htm/.css/.js/.jpg/.jpeg/.png/.gif/.ico/.svg`，其余返回 `application/octet-stream`。

#### `on_conn_timeout` — 超时回调

```c
static void on_conn_timeout(int fd, void *arg)
```

Keep-Alive 空闲连接超时后执行：从 epoll 移除、关闭 fd、释放 ctx。

#### `on_signal` — 信号处理

```c
static void on_signal(int sig)
```

设置全局停止标志 `g_stop = 1`，主循环退出后优雅关闭。

#### `usage` — 用法提示

```c
static void usage(const char *prog)
```

打印命令行参数说明。

---

### 6.2 http_parser.c

#### `http_parse` — HTTP 请求解析主函数

```c
parse_result_t http_parse(const char *buf, int len, http_request_t *req)
```

解析步骤：
1. 定位 `\r\n\r\n` 判断请求头完整性 → 未找到返回 `PARSE_INCOMPLETE`
2. sscanf 解析请求行（METHOD / PATH / VERSION）
3. 剥离查询字符串（`?` 之后）
4. URL 解码（`%xx` → 字节）
5. 安全检查：解码后路径含 `..` 返回 `PARSE_ERROR`
6. 逐行解析头部，识别 `Connection: keep-alive/close`
7. HTTP/1.1 默认 Keep-Alive，HTTP/1.0 默认关闭

#### `url_decode` — URL 解码

```c
static void url_decode(char *out, int out_size, const char *in)
```

将 `%xx` 转义序列还原为对应字节，非法转义原样保留。

#### `hex_value` — 十六进制字符转数值

```c
static int hex_value(char c)
```

返回 `0-9/a-f/A-F` 对应的数值，非法字符返回 `-1`。

#### `mem_find` — 不依赖 `\0` 的子串查找

```c
static const char *mem_find(const char *buf, int len, const char *needle)
```

在 `buf[0..len)` 范围内查找子串，用于定位 `\r\n\r\n` 和 `\r\n`。

---

### 6.3 threadpool.c

#### `threadpool_create` — 创建线程池

```c
threadpool_t *threadpool_create(int thread_count)
```

- `thread_count <= 0` 时自动按 `sysconf(_SC_NPROCESSORS_ONLN)` 设置
- 创建工作线程数组，逐个 `pthread_create` 启动 `worker_loop`
- 初始化互斥锁和条件变量

#### `threadpool_add` — 提交任务

```c
int threadpool_add(threadpool_t *pool, void (*routine)(void *), void *arg)
```

- 分配 task 节点，追加到队列尾部
- `pthread_cond_signal` 唤醒一个空闲工作线程

#### `threadpool_destroy` — 销毁线程池

```c
void threadpool_destroy(threadpool_t *pool)
```

- 设置 `shutdown = 1`，`pthread_cond_broadcast` 唤醒所有线程
- `pthread_join` 等待所有线程退出
- 销毁锁/条件变量，释放内存

#### `worker_loop` — 工作线程主循环（内部）

```c
static void *worker_loop(void *arg)
```

- 加锁 → 等待条件变量（队列空且未关闭）
- 取队头任务 → 解锁 → 锁外执行 `task->func(task->arg)` → 释放 task
- shutdown 且队列空时退出

---

### 6.4 timer_wheel.c

#### `timer_wheel_create` — 创建时间轮

```c
timer_wheel_t *timer_wheel_create(int slot_count)
```

分配 `slot_count` 个槽位的数组，初始化互斥锁。

#### `timer_wheel_add` — 添加定时器

```c
int timer_wheel_add(timer_wheel_t *w, int fd, int timeout_sec,
                    timer_callback cb, void *arg)
```

- **持锁去重**：先遍历所有槽摘除同 fd 的旧节点
- 计算 `rotation = timeout / slot_count`，`slot = (cur + timeout) % slot_count`
- 头插到目标槽的链表

#### `timer_wheel_remove` — 移除定时器

```c
void timer_wheel_remove(timer_wheel_t *w, int fd)
```

遍历所有槽，摘除并释放匹配 fd 的所有节点。

#### `timer_wheel_tick` — 推进一格

```c
void timer_wheel_tick(timer_wheel_t *w)
```

- 取出当前槽的链表，清空槽位，cur_slot 前进一格
- 分离：`rotation > 0` 的减一挂回原槽；`rotation == 0` 的收集为 fire 链表
- 锁外遍历 fire 链表执行回调（可能 close fd + free ctx）

#### `timer_wheel_destroy` — 销毁时间轮

```c
void timer_wheel_destroy(timer_wheel_t *w)
```

遍历释放所有槽的链表节点，销毁锁，释放内存。

---

### 6.5 log.c

#### `log_init` — 初始化异步日志

```c
int log_init(const char *filepath, int level)
```

- 设置日志级别
- 可选打开日志文件（追加模式）
- 创建后台日志线程

#### `log_write` — 写入日志（内部接口）

```c
void log_write(int level, const char *file, int line, const char *fmt, ...)
```

- 级别过滤（低于配置级别直接返回）
- 格式化：`[时间] [级别] [tid] 消息 (文件:行号)`
- 分配节点，strdup 消息，入队，唤醒后台线程

#### `log_thread` — 后台日志线程（内部）

```c
static void *log_thread(void *arg)
```

- 等待条件变量（队列空且未关闭）
- 批量取出整条队列，锁外执行 I/O
- 逐条 fputs 到 stderr 和文件，fflush 文件
- shutdown 且队列空时退出

#### `log_shutdown` — 关闭日志

```c
void log_shutdown(void)
```

唤醒后台线程刷盘，join 等待退出，关闭文件。

#### 日志宏接口

```c
#define log_debug(...) log_write(LOG_DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#define log_info(...)  log_write(LOG_INFO,  __FILE__, __LINE__, __VA_ARGS__)
#define log_warn(...)  log_write(LOG_WARN,  __FILE__, __LINE__, __VA_ARGS__)
#define log_error(...) log_write(LOG_ERROR, __FILE__, __LINE__, __VA_ARGS__)
```

---

### 6.6 legacy/myweb_v1_thread.c（v1 版本）

#### `creat_socket` — 创建监听套接字

```c
int creat_socket()
```

创建阻塞式 TCP socket，bind + listen（backlog 10）。

#### `worker` — 连接处理线程

```c
void *worker(void *arg)
```

- recv 一次读取（1024 字节缓冲）
- sscanf 解析请求行
- 仅支持 GET，检查 `..` 路径遍历
- open + lseek 求大小 + read 循环 send
- 短连接：发送完即 close

#### `send_404` — 发送 404 响应

```c
void send_404(int fd)
```

优先读取 `my404.html` 发送，失败回退内置简单 404 页面。

#### `log_access` — 访问日志

```c
void log_access(const char *ip, const char *path, int status)
```

同步 printf 输出 `[时间] IP GET 路径 -> 状态码`。

#### `get_mime_type` — MIME 类型映射

```c
const char *get_mime_type(const char *path)
```

支持 `.html/.htm/.css/.js/.jpg/.jpeg/.png/.gif/.ico`。

---

## 7. 模块间依赖关系

### 7.1 编译依赖

```
webserver.c
  ├── #include "log.h"           → log.c
  ├── #include "threadpool.h"    → threadpool.c
  ├── #include "http_parser.h"  → http_parser.c
  └── #include "timer_wheel.h"   → timer_wheel.c
```

Makefile 编译命令：
```makefile
SRCS = src/webserver.c src/threadpool.c src/http_parser.c src/timer_wheel.c src/log.c
$(CC) $(CFLAGS) -o $(TARGET) $(SRCS)
```

`http_parser.c`、`threadpool.c`、`timer_wheel.c`、`log.c` 之间无相互依赖，各为独立模块。

### 7.2 运行时调用关系

```
main()
  │
  ├── log_init()                    // 初始化异步日志
  ├── threadpool_create()           // 创建线程池
  ├── timer_wheel_create()          // 创建时间轮
  ├── create_listen_socket()        // 创建监听套接字
  ├── epoll_create1()               // 创建 epoll 实例
  │
  └── while (!g_stop)               // 主事件循环
      │
      ├── epoll_wait()
      │   │
      │   ├── [监听 fd 可读]
      │   │   ├── accept4()
      │   │   ├── calloc(conn_ctx_t)
      │   │   ├── timer_wheel_add()       // 挂超时定时器
      │   │   └── epoll_ctl(ADD)          // 注册 EPOLLONESHOT
      │   │
      │   └── [连接 fd 就绪]
      │       ├── timer_wheel_remove()    // 先摘定时器（消除竞态）
      │       └── threadpool_add()        // 投递 conn_worker
      │
      └── [每秒] timer_wheel_tick()       // 推进时间轮
          └── on_conn_timeout()           // 超时回调

conn_worker()                             // 工作线程执行
  │
  ├── [CONN_WRITING] flush_response()     // 写续传
  │   └── send() / sendfile()
  │
  ├── [CONN_READING] recv() 循环
  │
  ├── http_parse()                        // 解析 HTTP 请求
  │
  ├── build_response()                    // 构建响应
  │   ├── get_mime_type()
  │   ├── open() / fstat()
  │   └── log_info()
  │
  ├── flush_response()                    // 发送响应
  │   ├── send()                          // 发响应头
  │   └── sendfile()                      // 发文件体
  │
  ├── finish_response()                   // 收尾
  │   ├── close(filefd)
  │   └── memmove()                       // 移除已消费请求
  │
  ├── rearm_read()                        // 重新挂回 epoll
  │   ├── timer_wheel_add()
  │   └── epoll_ctl(MOD)
  │
  └── close_conn()                        // 关闭连接
      ├── timer_wheel_remove()
      ├── epoll_ctl(DEL)
      ├── close(fd)
      └── free(ctx)
```

### 7.3 外部系统依赖

| 依赖 | 用途 | 模块 |
|------|------|------|
| `epoll` (Linux) | I/O 多路复用 | webserver.c |
| `pthread` | 线程池、日志后台线程、时间轮锁 | threadpool.c, log.c, timer_wheel.c |
| `sendfile` | 零拷贝文件传输 | webserver.c |
| `accept4` | 非阻塞 accept | webserver.c |
| `TCP_NODELAY` | 禁用 Nagle 算法 | webserver.c |
| `CLOCK_MONOTONIC` | 单调时钟（时间轮节拍） | webserver.c |
| `SO_REUSEADDR` | 端口复用 | webserver.c |
| `MSG_NOSIGNAL` | 防止 SIGPIPE | webserver.c |

---

## 8. 核心流程详解

### 8.1 新连接建立流程

```
epoll_wait 返回监听 fd 可读
  │
  ▼
循环 accept4（直到 EAGAIN）
  │
  ├─ setsockopt(TCP_NODELAY)           // 禁用 Nagle，消除 40ms 延迟
  ├─ calloc(conn_ctx_t)                // 分配连接上下文
  ├─ inet_ntop()                       // 记录客户端 IP
  ├─ timer_wheel_add(15s, on_conn_timeout)  // 先挂定时器
  └─ epoll_ctl(ADD, EPOLLIN|EPOLLONESHOT|EPOLLRDHUP)  // 再武装 epoll
```

### 8.2 请求处理流程（conn_worker）

```
[入口] conn_worker(ctx)
  │
  ├─ state == CONN_WRITING?
  │   ├─ YES → flush_response() 续传
  │   │   ├─ r==1 → rearm_write()，return
  │   │   ├─ r<0 → close_conn()，return
  │   │   └─ r==0 → finish_response()
  │   │       └─ 返回值!=0 → close_conn()，return
  │   └─ NO → 继续
  │
  ├─ recv 循环（读到 EAGAIN 为止）
  │   ├─ n>0 → rlen += n
  │   ├─ n==0 → peer_closed = 1
  │   └─ n<0 EAGAIN → break
  │
  ├─ for (;;) 请求循环（pipelining）
  │   ├─ rlen==0 → break
  │   │
  │   ├─ http_parse(rbuf, rlen, &req)
  │   │   ├─ INCOMPLETE
  │   │   │   ├─ rlen >= buf_size → close_conn（请求过大）
  │   │   │   ├─ !peer_closed → rearm_read()，return
  │   │   │   └─ peer_closed → close_conn()，return
  │   │   │
  │   │   ├─ ERROR
  │   │   │   └─ build_response(404) → flush_response → close_conn
  │   │   │
  │   │   └─ COMPLETE
  │   │       ├─ build_response(ctx, &req)
  │   │       ├─ ctx->cur_header_len = req.header_len
  │   │       ├─ flush_response()
  │   │       │   ├─ r==1 → CONN_WRITING → rearm_write()，return
  │   │       │   ├─ r<0 → close_conn()，return
  │   │       │   └─ r==0 → finish_response()
  │   │       │       ├─ 返回值!=0 → close_conn()，return
  │   │       │       └─ 返回值==0 → 继续循环（下一请求）
  │   │       │
  │   │       └─ [循环继续处理下一请求]
  │
  └─ [请求循环结束]
      ├─ peer_closed → close_conn()
      └─ !peer_closed → rearm_read()
```

### 8.3 时间轮工作流程

```
主循环每秒（CLOCK_MONOTONIC 限制）调用 timer_wheel_tick()
  │
  ├─ 取出 slots[cur_slot] 链表，清空槽位
  ├─ cur_slot = (cur_slot + 1) % slot_count
  │
  ├─ 遍历链表分离：
  │   ├─ rotation > 0 → rotation-- → 挂回刚离开的槽（等下一圈）
  │   └─ rotation == 0 → 加入 fire 链表（到期）
  │
  └─ 锁外遍历 fire 链表：
      └─ node->cb(fd, arg)  → on_conn_timeout: close(fd) + free(ctx)
```

---

## 9. 并发与线程安全模型

### 9.1 线程模型

| 线程 | 数量 | 职责 |
|------|------|------|
| 主线程（Reactor） | 1 | epoll_wait、accept4、事件分发、时间轮 tick |
| 工作线程（线程池） | N（默认=CPU核数） | conn_worker：recv → 解析 → sendfile → rearm |
| 日志线程 | 1 | 批量取出日志队列写入文件/stderr |

### 9.2 EPOLLONESHOT 保证连接级线程安全

- 连接注册 `EPOLLONESHOT`：事件触发后自动 disarm
- 同一连接的事件同一时刻只被一个线程处理
- 工作线程处理完毕后 `EPOLL_CTL_MOD` 重新挂回

### 9.3 竞态消除设计

**问题**：工作线程处理连接期间，时间轮可能触发超时回调 `free(ctx)`，导致 use-after-free。

**解决方案**：三道防线确保安全：

1. **投递前摘定时器**：主线程在 `threadpool_add` 之前先 `timer_wheel_remove`
   ```
   timer_wheel_remove(timer, ctx->fd);     // 先摘
   threadpool_add(pool, conn_worker, ctx);  // 再投递
   ```
   工作线程独占连接期间，定时器不存在，竞态窗口从设计上消除。

2. **先定时器后武装**：所有 rearm 路径统一顺序
   ```
   timer_wheel_add(...)      // ① 先挂定时器
   epoll_ctl(MOD, ...)       // ② 后武装 epoll
   ```
   保证"事件可能被返回"时定时器一定已存在。

3. **数据结构去重**：`timer_wheel_add` 内部持锁遍历摘除旧节点，保证同一 fd 至多一个定时器节点。

### 9.4 锁的保护范围

| 锁 | 保护对象 | 持锁线程 |
|----|---------|---------|
| `threadpool_t.lock` | 任务队列 | 主线程（add）、工作线程（take） |
| `timer_wheel_t.lock` | 槽位链表 | 主线程（tick/remove）、工作线程（add/remove via rearm） |
| `g_log.lock` | 日志队列 | 所有业务线程（log_write）、日志线程（batch take） |

---

## 10. 安全防护机制

### 10.1 目录遍历防护

```c
// http_parser.c
// 4. URL 解码（必须在安全检查之前，防 %2e%2e 绕过）
url_decode(req->path, sizeof(req->path), raw_path);

// 5. 安全防护：解码后的路径包含 .. 一律拒绝
if (strstr(req->path, "..") != NULL)
{
    return PARSE_ERROR;
}
```

**防护逻辑**：先 URL 解码（`%2e%2e` → `..`），再做 `..` 检查，防止编码绕过。非法请求返回 `PARSE_ERROR`，服务器返回 404 并关闭连接。

### 10.2 SIGPIPE 防护

```c
signal(SIGPIPE, SIG_IGN);                    // 全局忽略
send(fd, ..., MSG_NOSIGNAL);                // 单次调用级别防护
```

对端提前断开时 `send` 返回 `-1` 而非触发 SIGPIPE 终止进程。

### 10.3 请求头大小限制

```c
#define READ_BUFFER_SIZE 65536   // 接收缓冲区上限

if (ctx->rlen >= (int)sizeof(ctx->rbuf))
{
    // 请求头超过缓冲区上限，判定为异常，直接关闭
    close_conn(ctx);
}
```

### 10.4 端口复用

```c
int reuse = 1;
setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
```

服务器重启后可立即重新绑定端口。

---

## 11. 性能优化与压测

### 11.1 压测结果

| 版本 | 模型 | 连接方式 | QPS | 平均延迟 | 延迟稳定性 |
|------|------|----------|-----|----------|------------|
| v1 | 每连接一线程 | 短连接 | 5,897 | 21.05ms | 标准差 109.7ms、11 次超时 |
| v2 | epoll+线程池 | 短连接 | 21,568 | 4.13ms | 标准差 2.8ms、0 错误（P99 61ms） |
| v2 | epoll+线程池 | Keep-Alive | 50,141 | 3.63ms | 标准差 1.7ms、0 错误（P99 9.66ms） |

### 11.2 性能演进路径

```
QPS 3.47 ──(修 EAGAIN 误杀)──▶ 547 ──(修定时器飞转)──▶ 4,540
                                                        │
                              ──(修 Nagle 40ms)──▶ 21,568（短连接）/ 50,141（长连接）
```

### 11.3 排查的 6 个并发问题

| # | 问题 | 现象 | 根因 | 修复 |
|---|------|------|------|------|
| 1 | EAGAIN 误杀 | QPS 3.47, 27 万 write 错误 | 非阻塞 send 的 EAGAIN 被当致命错误 | 拆分 build/flush，EAGAIN 时存现场等 EPOLLOUT 续传 |
| 2 | 定时器 UAF | 段错误 | 投递任务后定时器仍可能触发 free(ctx) | 主线程投递前先 timer_remove |
| 3 | 双定时器节点 | 偶发段错误 + 93 万 write 错误 | rearm 顺序错（先 epoll 后 timer），remove 摘空 | 统一为先 timer_add 后 epoll_ctl |
| 4 | 时间轮飞转 | 9.5 万 read 错误，虚假 18K QPS | tick 每次 epoll_wait 返回都执行，高负载下每秒狂转上万格 | 用 CLOCK_MONOTONIC 限制每秒一次 |
| 5 | Nagle 40ms | Keep-Alive 延迟稳定 43.59ms | Nagle 算法 × Delayed ACK 互等 | accept 后 setsockopt(TCP_NODELAY) |
| 6 | v1 线程开销 | QPS 5897, 标准差 110ms | 每连接 pthread_create/销毁 | v2 用固定线程池 + epoll |

> 详细排查过程见 [docs/performance-notes.md](performance-notes.md)

---

## 12. 静态网站资源

### 12.1 index.html — 网站主页

- **标题**：拉贝尔大陆 · 花仙魔法使的回忆
- **内容**：拉贝尔大陆传说介绍、主要魔法使者（夏安安、库库鲁、千韩）卡片展示、精灵王图鉴收集进度条
- **技术**：HTML5 + 内联 CSS3，响应式设计（`@media max-width: 768px`），渐变背景、卡片布局、hover 动画
- **导航**：拉贝尔大陆（首页）、精灵王图鉴、在线观看（外链爱奇艺）

### 12.2 collection.html — 花仙精灵王图鉴

- **标题**：花仙精灵王图鉴 · 花之法典收集册
- **内容**：10 位花仙精灵王卡片（椿、山梦、红雨、玛格丽特、苏睦、曼珠沙华、君影、穆回、御姬、爱丽丝）
- **每张卡片**：精灵图片、花名、标签、精灵名、花语、描述
- **技术**：CSS Grid 自适应布局（`auto-fill, minmax(310px, 1fr)`），卡片 hover 浮动效果

### 12.3 my404.html — 自定义 404 页面

- **标题**：花朵走丢啦 · 404
- **内容**：404 错误码、趣味文案、8 秒倒计时自动跳转回首页
- **技术**：`meta http-equiv="refresh" content="8;url=index.html"` + JS 倒计时、飘落花瓣 CSS 动画（`@keyframes fall`）

### 12.4 images/ — 图片资源

13 张 JPG 图片，涵盖 3 个角色头像和 10 个精灵王卡片图。

---

## 13. 项目运行方式

### 13.1 环境要求

- **操作系统**：Linux（依赖 epoll、sendfile 等 Linux 专有 API）
- **编译器**：GCC（支持 C11/GNU 扩展）
- **构建工具**：Make

### 13.2 编译

```bash
make
```

编译参数：`gcc -Wall -Wextra -O2 -pthread -std=gnu11`

生成可执行文件 `myweb`。

### 13.3 运行

```bash
# 推荐使用 1024 以上端口，无需 root 权限
./myweb -p 8080 -t 4 -d . -l server.log
```

### 13.4 访问

- 首页：`http://127.0.0.1:8080`
- 精灵王图鉴：`http://127.0.0.1:8080/collection.html`
- 404 页面：`http://127.0.0.1:8080/任意不存在的路径`

### 13.5 压测

```bash
# 需要先安装 wrk 或 apache2-utils
./benchmark.sh 8080 4
```

脚本自动：编译 → 启动服务器 → 健康检查 → Keep-Alive + 短连接两组压测 → 汇总关键指标。

### 13.6 v1 版本编译运行

```bash
gcc -O2 -pthread -o myweb_v1 legacy/myweb_v1_thread.c
./myweb_v1   # 监听 80 端口（需 sudo）
```

### 13.7 清理

```bash
make clean   # 删除编译产物
```

---

## 14. 配置参数说明

### 14.1 命令行参数

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `-p <port>` | 监听端口 | 80（需 root） |
| `-t <threads>` | 工作线程数 | 0（自动=CPU核数） |
| `-d <dir>` | 网站静态文件根目录 | `.`（当前目录） |
| `-l <file>` | 访问日志文件路径 | NULL（仅输出到终端） |
| `-q` | 静默模式：只记录 WARN/ERROR | LOG_INFO |

### 14.2 编译期常量

| 宏 | 值 | 说明 |
|----|----|------|
| `MAX_EVENTS` | 1024 | epoll_wait 单次返回最大事件数 |
| `READ_BUFFER_SIZE` | 65536 | 单连接接收缓冲区大小 |
| `KEEPALIVE_IDLE` | 15 | Keep-Alive 空闲超时秒数 |
| `TIMER_SLOTS` | 60 | 时间轮槽位数（一圈 60 秒） |

---

## 15. v1 与 v2 版本对比

### 15.1 架构对比

| 维度 | v1 | v2 |
|------|-----|-----|
| 并发模型 | 每连接一线程（pthread_create + detached） | epoll Reactor + 固定线程池 |
| I/O 模式 | 阻塞 socket | 非阻塞 socket + EPOLLONESHOT |
| 请求读取 | 一次 recv（1024 字节缓冲） | 循环 recv 到 EAGAIN（65536 字节缓冲） |
| HTTP 解析 | sscanf 仅解析请求行 | 增量解析器：半包/粘包/pipelining |
| 安全防护 | `..` 检查（不解码） | URL 解码 + `..` 检查 |
| 文件传输 | lseek + read 循环 + send | sendfile 零拷贝 |
| 连接模式 | 短连接（发完即 close） | Keep-Alive 长连接（15s 超时回收） |
| 超时管理 | 无 | 单轮时间轮 |
| 日志 | printf 同步 | 异步日志（后台线程批量落盘） |
| 信号处理 | 仅忽略 SIGPIPE | SIGINT/SIGTERM 优雅退出 |
| 配置 | 硬编码 PORT/DOC_ROOT | 命令行参数 |
| 代码组织 | 单文件 | 模块化（5 个 .c/.h 文件） |

### 15.2 性能对比

| 指标 | v1（短连接） | v2（短连接） | v2（Keep-Alive） |
|------|-------------|-------------|-----------------|
| QPS | 5,897 | 21,568 | 50,141 |
| 平均延迟 | 21.05ms | 4.13ms | 3.63ms |
| 延迟标准差 | 109.7ms | 2.8ms | 1.7ms |
| 错误数 | 11 次超时 | 0 | 0 |
| P99 延迟 | 1.67s（最大） | 61ms | 9.66ms |

v2 相比 v1：QPS 提升 **3.7 倍**，延迟降至 **1/5**，延迟抖动下降两个数量级。

---

## 16. 待改进方向

- [ ] 主从 Reactor（多 Sub-Reactor 分担连接分发）
- [ ] 支持 POST 方法与静态文件缓存（ETag、Last-Modified）
- [ ] 支持 HTTPS（OpenSSL）
- [ ] 最小堆定时器替代时间轮以支持任意精度超时

---

## 附录：关键文件索引

| 文件 | 路径 | 行数 | 说明 |
|------|------|------|------|
| webserver.c | [src/webserver.c](file:///workspace/src/webserver.c) | 727 | 主程序：epoll Reactor |
| threadpool.c | [src/threadpool.c](file:///workspace/src/threadpool.c) | 156 | 线程池 |
| threadpool.h | [src/threadpool.h](file:///workspace/src/threadpool.h) | 18 | 线程池接口 |
| http_parser.c | [src/http_parser.c](file:///workspace/src/http_parser.c) | 138 | HTTP 解析器 |
| http_parser.h | [src/http_parser.h](file:///workspace/src/http_parser.h) | 30 | 解析器接口 |
| timer_wheel.c | [src/timer_wheel.c](file:///workspace/src/timer_wheel.c) | 201 | 时间轮 |
| timer_wheel.h | [src/timer_wheel.h](file:///workspace/src/timer_wheel.h) | 31 | 时间轮接口 |
| log.c | [src/log.c](file:///workspace/src/log.c) | 170 | 异步日志 |
| log.h | [src/log.h](file:///workspace/src/log.h) | 23 | 日志接口 |
| myweb_v1_thread.c | [legacy/myweb_v1_thread.c](file:///workspace/legacy/myweb_v1_thread.c) | 294 | v1 版本（对比用） |
| Makefile | [Makefile](file:///workspace/Makefile) | 12 | 构建脚本 |
| benchmark.sh | [benchmark.sh](file:///workspace/benchmark.sh) | 76 | 压测脚本 |
| index.html | [index.html](file:///workspace/index.html) | 274 | 网站主页 |
| collection.html | [collection.html](file:///workspace/collection.html) | 330 | 精灵王图鉴 |
| my404.html | [my404.html](file:///workspace/my404.html) | 281 | 404 页面 |
| README.md | [README.md](file:///workspace/README.md) | 170 | 项目说明 |
| performance-notes.md | [docs/performance-notes.md](file:///workspace/docs/performance-notes.md) | 155 | 性能调优笔记 |
| 项目通俗讲解.md | [docs/项目通俗讲解.md](file:///workspace/docs/项目通俗讲解.md) | 180 | 大白话讲解 |
