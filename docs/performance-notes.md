# 性能调优与并发问题排查笔记

> 本文记录 myweb v2（epoll Reactor + 线程池）从「压测崩溃、QPS 个位数」到「5 万 QPS、0 错误」的完整排查过程。每个问题都是网络编程的经典考点，附现象、根因、修复和面试话术。

## 一、压测环境与方法

- 环境：VMware 虚拟机（Ubuntu），wrk 与服务器同机，走 127.0.0.1（排除外网干扰）
- 工具：wrk
  - Keep-Alive：`wrk -t4 -c200 -d10s --latency http://127.0.0.1:8080/index.html`
  - 短连接：`wrk -t4 -c100 -d10s -H "Connection: close" http://127.0.0.1:8080/index.html`
- 目标文件：`index.html`，约 10KB

**怎么读 wrk 输出**（排查时最关键的三项）：

| 指标 | 含义 | 异常指向 |
|------|------|----------|
| `Socket errors: connect/read/write/timeout` | connect：连不上；write：向已重置连接写数据；read：响应未收全就 EOF/RST；timeout：请求超时 | read/write 暴涨 = 服务器在大规模异常关闭连接 |
| `Latency` 的 Avg / Stdev / Max | 平均延迟、标准差、最大延迟 | Stdev 远大于 Avg、Max 达秒级 = 线程调度/锁/资源抖动 |
| `Requests/sec` | QPS | **不能只看 QPS**，必须结合错误数：高 QPS + 高错误可能是连接被反复重置后的"虚高" |

## 二、最终结果

| 版本 | 模型 | 连接方式 | QPS | 平均延迟 | 延迟稳定性 |
|------|------|----------|-----|----------|------------|
| v1 | 每连接一线程 | 短连接 | 5,897 | 21.05ms | 标准差 109.7ms、11 次超时（最大 1.67s） |
| v2 | epoll+线程池 | 短连接 | 21,568 | 4.13ms | 标准差 2.8ms、0 错误（P99 61ms） |
| v2 | epoll+线程池 | Keep-Alive | 50,141 | 3.63ms | 标准差 1.7ms、0 错误（P99 9.66ms） |

演进过程的关键拐点：

```
QPS 3.47  ──(修 EAGAIN 误杀)──▶  547    ──(修定时器飞转)──▶  4,540
                                                        │
                              ──(修 Nagle 40ms)──▶  21,568（短连接）/ 50,141（长连接）
```

## 三、问题逐个分析

### 问题 1：非阻塞 send 遇到 EAGAIN 被当成致命错误

**现象**：首次压测 `Socket errors: write 277118`，QPS 仅 3.47，连接被海量重置。

**根因**：所有 socket 都是非阻塞的。当发送缓冲写满时，`send`/`sendfile` 返回 `-1` 且 `errno == EAGAIN`，这是**正常现象**，含义是"现在发不出去，等 socket 可写后再发"。但代码把它当成了发送失败，直接关闭连接 → 客户端收到 RST → write 错误爆炸。

**修复**（[webserver.c](../src/webserver.c) 的 `flush_response`）：
- 拆分为 `build_response`（准备响应头和文件 fd）与 `flush_response`（增量发送）；
- 发送现场（已发字节数、文件偏移）保存在 `conn_ctx` 里；
- 遇到 EAGAIN：把连接置为 `CONN_WRITING` 状态，注册 `EPOLLOUT | EPOLLONESHOT`，等可写事件后续传，断点续发。

**考点**：非阻塞 I/O + Reactor 的完整读写状态机；short write / EINTR / EAGAIN 三者的区别。

---

### 问题 2：定时器回调与工作线程的 use-after-free

**现象**：压测中进程段错误（`核心已转储`），伴随大量 RST。

**根因**：连接事件投递给线程池后，存在一个时间窗口——工作线程还没来得及摘除超时定时器，主线程的时间轮 tick 就可能触发回调 `close(fd) + free(ctx)`。之后工作线程继续操作已释放的 `ctx`，或二次 free → 踩内存崩溃。

**修复**：把定时器的摘除时机从"工作线程开始处理时"提前到"**主线程投递任务前**"（[webserver.c 事件分发处](../src/webserver.c)）：

```c
conn_ctx_t *ctx = events[i].data.ptr;
timer_wheel_remove(timer, ctx->fd);      // 先摘除
threadpool_add(pool, conn_worker, ctx);  // 再投递
```

工作线程独占连接期间，定时器必然不存在，竞态窗口从设计上消除。

**考点**：跨线程资源生命周期管理；"谁分配谁释放"在事件驱动模型下要细化为"谁持有所有权"。

---

### 问题 3：rearm 时序导致同一连接出现两个定时器节点

**现象**：修复问题 2 后仍偶发段错误和 93 万 write 错误。

**根因**：工作线程处理完请求后要"重新武装"，原顺序是：① `epoll_ctl MOD`（重新监听）→ ② `timer_add`（加定时器）。两步之间主线程可能已经 `epoll_wait` 返回新事件并执行 `timer_remove`——**但此时新定时器还没加上，remove 摘了个空**。随后工作线程补上节点 A，下一次处理又加节点 B，同一 fd 残留两个定时器：节点 A 到期时回调会把**正被其他线程处理的连接**释放掉。

**修复**：所有 rearm 路径（新连接、读完待下一个请求、EPOLLOUT 待续传）统一为 **① 先加定时器 → ② 后武装 epoll**。保证"事件可能被返回"的那一刻，定时器一定已存在，主线程的 remove 一定摘得掉。

另外在 `timer_wheel_add` 内部加了**持锁去重**，从数据结构层面保证"同一 fd 至多一个定时器节点"的不变式（双保险）。

**考点**：`EPOLLONESHOT` 的 rearm 顺序是 Reactor 编程最容易错的地方；用"不变式 + 防御性去重"替代"我相信时序不会错"。

---

### 问题 4：时间轮 tick 被 epoll 唤醒次数驱动（本次最隐蔽）

**现象**：问题 3 修复后段错误消失，但仍有 9.5 万 read 错误；更反常的是有一次跑出 18,407 QPS 的"好成绩"。

**根因**：时间轮设计上"每秒 tick 推进一格"，靠 `epoll_wait(1000ms)` 的超时驱动。但 tick 写在了主循环末尾、**每次 `epoll_wait` 返回都执行**：
- 空闲时：每秒超时返回 1 次 → tick 1 次，正常；
- 压测时：epoll_wait 每秒被事件唤醒上万次 → **时间轮每秒狂转上万格** → 设定 15 秒的超时实际几毫秒就到期 → 刚处理完请求、在等下一个请求的活跃连接被超时回调成批误杀。

那次 18,407 的"虚高 QPS"正是连接被误杀后 wrk 不停重连造成的假象。

**修复**：用单调时钟严格限定节拍（[webserver.c 主循环](../src/webserver.c)）：

```c
struct timespec last_tick;
clock_gettime(CLOCK_MONOTONIC, &last_tick);
// ...主循环...
struct timespec now;
clock_gettime(CLOCK_MONOTONIC, &now);
long elapsed_ms = (now.tv_sec - last_tick.tv_sec) * 1000
                + (now.tv_nsec - last_tick.tv_nsec) / 1000000;
if (elapsed_ms >= 1000) {
    timer_wheel_tick(timer);
    last_tick = now;
}
```

**考点**：事件循环里的时间不能用"循环迭代次数"度量，必须用 `CLOCK_MONOTONIC`（不受系统时间跳变影响）；QPS 高不等于健康，要和错误数、延迟一起看。

---

### 问题 5：Nagle 算法 × Delayed ACK 导致 40ms 固定延迟

**现象**：定时器修好后 read 错误清零，但 Keep-Alive 平均延迟稳定在 **43.59ms**（标准差仅 3.1ms），且比短连接还慢。

**根因**：延迟高度集中在某个固定值，是典型的**协议栈互等**症状，而非排队抖动：
- 服务器 socket 默认开 Nagle 算法：响应头、文件尾包等不足 MSS 的小包会被攒着，等收到对端 ACK 才发；
- 客户端（wrk）开 Delayed ACK：纯 ACK 最多延迟 40ms；
- 两边互相等 → 每个请求固定卡 ~40ms。
- 短连接关闭时 TCP 会发 FIN 顺带 flush，反而避开了部分停顿，所以短连接"看起来更快"。

**修复**：accept 后立即 `setsockopt(TCP_NODELAY)` 禁用 Nagle（nginx 等 HTTP 服务器默认就这么做）。

**效果**：Keep-Alive 平均延迟 43.59ms → **3.63ms**，QPS 4,540 → **50,141**，且长连接重新快于短连接（符合理论）。

**考点**：Nagle 与 Delayed ACK 的交互是网络编程经典面试题；HTTP 服务器为什么要开 TCP_NODELAY。

---

### 问题 6：v1 每连接一线程的固有瓶颈

**现象**：v1 在同样的短连接压测下 QPS 仅 5,897，延迟标准差 109.7ms（v2 为 2.8ms），最大延迟 1.67s，并出现 11 次超时。

**根因**：v1 每接受一个连接就 `pthread_create` 一个分离线程，请求结束线程销毁。100 并发意味着每秒钟大量线程的创建/销毁 + 上百个线程竞争 CPU 做上下文切换，调度延迟极不稳定。

**结论**：这正是 v2 用"固定大小线程池 + epoll 事件驱动"解决的问题——线程数不再随连接数增长，吞吐提升 3.7 倍，延迟抖动下降两个数量级。

## 四、排查方法论（可复用）

1. **先看错误数，再看 QPS**：`Socket errors` 全 0 是前提；read/write 错误暴涨优先怀疑服务器异常关闭连接。
2. **延迟分布比平均值更重要**：
   - Stdev 很小、Avg 很大且集中在固定值 → 协议栈/锁的固定等待（如 40ms 指向 Delayed ACK）；
   - Stdev 远大于 Avg、Max 达秒级 → 线程调度、资源争抢。
3. **用对照实验定位**：长连接 vs 短连接、不同并发数、curl 单请求 vs wrk 压测，差异即线索。
4. **竞态问题靠"时序推理"而非加日志**：想清楚"线程 A 执行到第几步时，线程 B 能做什么"，用"投递前摘定时器""先加定时器后武装"这类顺序约束消除窗口，再用数据结构不变式兜底。
5. **时间相关逻辑一律用单调时钟**，不要和事件循环迭代次数挂钩。
6. **每修一个点就重新压测回归**，并把数据记录下来——性能演进的过程本身就是项目最有说服力的部分。

## 五、复现命令

```bash
make clean && make
./benchmark.sh 8080 4          # v2：自动起服务、健康检查、长/短连接两组压测
                               # 结果在 bench-result-*.txt，服务器日志在 server-bench.log

# v1 对比（每连接一线程）
cp legacy/myweb_v1_thread.c /tmp/v1.c
sed -i 's/#define PORT      80/#define PORT      8081/' /tmp/v1.c
gcc -O2 -pthread -o /tmp/myweb_v1 /tmp/v1.c
(cd /path/to/project && /tmp/myweb_v1 > /dev/null 2>&1 &)
wrk -t4 -c100 -d10s -H "Connection: close" http://127.0.0.1:8081/index.html
pkill -f myweb_v1
```
