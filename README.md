# MyWebServer — epoll 多线程 HTTP 服务器 & 小花仙主题网站

基于 C 语言实现的高并发 HTTP/1.1 静态文件服务器，采用 **epoll Reactor + 线程池** 模型，支持 Keep-Alive 长连接、零拷贝文件发送、增量 HTTP 解析、时间轮超时管理与异步日志。同时依托该服务器部署了一个小花仙动漫主题的静态网站，包含首页、精灵王图鉴等页面。

> 项目经历了两个版本的演进（便于对比学习）：
> - **v1**（[legacy/myweb_v1_thread.c](legacy/myweb_v1_thread.c)）：每连接一个 pthread 的基础多线程版；
> - **v2**（当前版本）：epoll + 线程池 + 非阻塞 I/O 的 Reactor 版。

## 功能特性

- **epoll Reactor 并发模型**：主线程 `epoll_wait` 统一接收连接与分发事件，工作线程池处理请求，避免"每连接一线程"的创建/销毁开销。
- **EPOLLONESHOT 事件保护**：同一连接的事件同一时刻只被一个线程处理，无锁化保证连接级线程安全。
- **线程池**：固定数量工作线程 + 任务队列（互斥锁 + 条件变量，生产者-消费者模型），线程数可配，默认按 CPU 核数。
- **HTTP/1.1 Keep-Alive 长连接**：支持连接复用，响应头正确返回 `Connection`；空闲连接由**时间轮定时器**（15 秒）自动回收。
- **增量式 HTTP 解析器**：状态机思想逐字节解析请求头，正确处理**半包/粘包**与 HTTP pipelining；识别 `Connection` 头部。
- **sendfile 零拷贝**：静态文件直接在内核态从文件描述符传输到套接字，不经过用户态缓冲。
- **可靠传输**：`send` 循环处理短写（short write）与 `EINTR`；`MSG_NOSIGNAL` 防止对端断开导致进程退出。
- **安全防护**：路径先做 URL 解码（`%xx`）再检测 `..`，防止 `%2e%2e` 编码形式的目录遍历绕过。
- **MIME 类型映射**：支持 .html、.css、.js、.jpg、.png、.gif、.ico、.svg 等格式。
- **自定义 404 页面**：资源不存在时返回小花仙主题的 my404.html（含倒计时自动跳转）。
- **异步日志**：日志先入无界队列，由后台线程批量落盘，不阻塞工作线程；支持日志级别与文件输出。
- **工程化**：Makefile 构建、命令行参数配置（端口/线程数/根目录/日志文件）、SIGINT/SIGTERM 优雅退出。
- **静态网站部署**：内置小花仙主题网站（首页、精灵王图鉴、404 页），响应式设计。

## 技术栈

- **后端**：C11、Linux epoll、POSIX Socket（非阻塞 I/O）、pthread、sendfile、时间轮
- **协议**：HTTP/1.1（GET/HEAD、Keep-Alive）
- **工具**：Make、wrk / ab 压测
- **前端**：HTML5、CSS3（渐变背景、卡片布局、响应式设计）

## 快速开始

### 1. 克隆仓库

```bash
git clone git@github.com:xix-060/mywebserver.git
cd mywebserver
```

### 2. 编译

```bash
make
```

### 3. 运行

```bash
# 推荐使用 1024 以上端口，无需 root
./myweb -p 8080 -t 4 -d . -l server.log

# 参数说明：
#   -p 监听端口（默认 80，80 端口需要 sudo）
#   -t 工作线程数（默认 = CPU 核数）
#   -d 网站静态文件根目录（默认当前目录）
#   -l 日志文件路径（默认仅输出到终端）
```

### 4. 访问

打开浏览器，访问 `http://127.0.0.1:8080` 或 `http://你的IP地址:8080`，即可浏览小花仙主题网站。

访问一个不存在的路径（如 `http://127.0.0.1:8080/abc`）可触发自定义 404 页面。

### 5. 压测

```bash
./benchmark.sh 8080 4    # 需要先安装 wrk 或 apache2-utils
```

## 目录结构

```text
mywebserver/
├── src/                       # v2 服务器源码（模块化）
│   ├── webserver.c            #   主程序：epoll Reactor 事件循环、连接管理
│   ├── threadpool.c/.h        #   线程池（任务队列 + 互斥锁/条件变量）
│   ├── http_parser.c/.h       #   增量式 HTTP 请求头解析器
│   ├── timer_wheel.c/.h       #   时间轮定时器（Keep-Alive 超时回收）
│   └── log.c/.h               #   异步日志（后台线程批量落盘）
├── legacy/
│   └── myweb_v1_thread.c      # v1：每连接一线程的基础版本（演进对比）
├── index.html                 # 网站主页（拉贝尔大陆）
├── collection.html            # 花仙精灵王图鉴
├── my404.html                 # 自定义 404 页面
├── images/                    # 角色和精灵王图片
├── Makefile                   # 构建脚本
├── benchmark.sh               # wrk/ab 压测脚本
└── README.md
```

## 核心实现解析

### 整体架构（Reactor + 线程池）

```
              ┌───────────────────── 主线程（Reactor）─────────────────────┐
              │  epoll_wait                                               │
              │    ├─ 监听套接字可读 → accept4（非阻塞）→ 注册 EPOLLONESHOT │
              │    ├─ 连接可读     → 投递任务到线程池                       │
              │    └─ 每秒 tick    → 时间轮推进，回收空闲 Keep-Alive 连接   │
              └───────────────────────────┬───────────────────────────────┘
                                          │ 任务队列（mutex + cond）
                          ┌───────────────┼───────────────┐
                          ▼               ▼               ▼
                     工作线程 1       工作线程 2  ...  工作线程 N
                     recv→解析→sendfile→重新挂回 epoll
```

- 监听套接字用 **LT（水平触发）** 循环 `accept4` 到 EAGAIN，防止连接积压。
- 连接套接字注册 **EPOLLONESHOT**：事件触发后自动 disarm，工作线程处理完毕再 `EPOLL_CTL_MOD` 重新挂回，保证同一连接不会被多个线程同时操作。
- 所有套接字均为**非阻塞**（`SOCK_NONBLOCK`），工作线程 `recv` 到 EAGAIN 为止。

### HTTP 解析与半包处理

- v1 用 `sscanf` 只解析请求行、1024 缓冲截断；v2 的解析器先定位 `\r\n\r\n` 判断请求头是否完整，不完整则保留已收数据、重新挂回 epoll 等剩余字节（**半包**）。
- 一次 `recv` 可能读到多个请求（**粘包/HTTP pipelining**）：工作线程循环解析处理缓冲区中的所有完整请求，剩余字节用 `memmove` 前移。
- 逐行解析头部，HTTP/1.1 默认 Keep-Alive，`Connection: close` 可关闭。

### 安全防护

- 路径先 URL 解码（`%2e%2e` → `..`）再做目录遍历检查，杜绝编码绕过；非法请求直接关闭连接并记录 warn 日志。

### 零拷贝与可靠发送

- 文件体用 `sendfile()` 直接在内核态传输，省去 `read`+`send` 的两次用户态拷贝。
- 响应头发送用循环 `send` 处理短写，配合 `MSG_NOSIGNAL` 忽略对端断开产生的 SIGPIPE。

### 时间轮定时器

- 单轮时间轮（60 槽，1 秒/tick）：Keep-Alive 连接 15 秒无数据则被回调关闭，防止空闲连接耗尽文件描述符。
- 连接被工作线程取走处理时先摘除定时器，处理完重新挂回，避免"正在处理却被超时关闭"的竞态。

## 压测对比

> 测试环境：VMware 虚拟机，`wrk -t4 -c200 -d10s`（Keep-Alive）/ `-c100`（短连接），请求 `index.html`（约 10KB），wrk 与服务器同机（127.0.0.1）。所有用例 Socket errors 均为 0。

| 版本 | 模型 | 连接方式 | QPS | 平均延迟 | P99 延迟 |
|------|------|----------|-----|----------|----------|
| v1 | 每连接一线程 | 短连接 | _待测_ | _待测_ | _待测_ |
| v2 | epoll+线程池 | 短连接 | **21,568** | 4.13ms | 61.07ms |
| v2 | epoll+线程池 | Keep-Alive | **50,141** | 3.63ms | 9.66ms |

**结论**：
- Keep-Alive 长连接相比短连接 QPS 提升约 **2.3 倍**（省掉了 TCP 三次握手/四次挥手的开销），P99 延迟也更稳定。
- v2 的 epoll Reactor 模型由固定数量工作线程承载所有连接，避免了 v1 "每连接创建/销毁一个线程" 的开销，高并发下吞吐与资源占用优势明显。

> 复现：`./benchmark.sh 8080 4`，结果保存在 `bench-result-*.txt`。

## 待改进方向

- [x] ~~支持 HTTP 持久连接（Keep-Alive）~~
- [x] ~~引入线程池~~
- [x] ~~epoll I/O 多路复用 + 非阻塞 I/O~~
- [x] ~~sendfile 零拷贝~~
- [x] ~~增量 HTTP 解析（半包/粘包）~~
- [x] ~~异步日志、配置化、优雅退出~~
- [ ] 主从 Reactor（多 Sub-Reactor 分担连接分发）
- [ ] 支持 POST 方法与静态文件缓存（ETag、Last-Modified）
- [ ] 支持 HTTPS（OpenSSL）
- [ ] 最小堆定时器替代时间轮以支持任意精度超时

## 作者

[xix-060](https://github.com/xix-060) - GitHub

## 许可证

本项目仅供学习交流使用，无特定许可证。网站内容涉及《小花仙》动漫，版权归上海淘米动画有限公司所有。
