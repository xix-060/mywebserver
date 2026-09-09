#!/bin/bash
# benchmark.sh — 压测脚本（需要安装 wrk 或 apache2-utils(ab)）
# 用法:
#   ./benchmark.sh              # 默认 8080 端口、线程数=CPU核数
#   ./benchmark.sh 8080 4       # 指定端口和工作线程数
#
# 结果说明：
#   wrk 汇总结果保存到 bench-result-*.txt 并在最后统一打印；
#   服务器日志（含异常请求）保存到 server-bench.log，压测后可查。
set -u

PORT=${1:-8080}
THREADS=${2:-0}

cd "$(dirname "$0")"
make

# 1. 端口占用检查：避免压到已有的旧服务实例
if ss -ltn 2>/dev/null | grep -q ":$PORT "; then
    echo "==> 错误：端口 $PORT 已被占用，请先停掉已有服务器："
    echo "    ss -ltnp | grep ':$PORT'    # 找到进程"
    echo "    kill <PID>                   # 停掉后重试"
    exit 1
fi

# -q 静默模式：服务器只记录 WARN/ERROR；stderr 重定向到文件，不干扰 wrk 输出
echo "==> 启动服务器: port=$PORT threads=${THREADS:-auto}（日志: server-bench.log）"
./myweb -p "$PORT" -t "$THREADS" -q >server-bench.log 2>&1 &
SERVER_PID=$!
trap "kill $SERVER_PID 2>/dev/null" EXIT

# 2. 等待并确认服务器真的启动成功
sleep 1
if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    echo "==> 错误：服务器进程启动后立刻退出，server-bench.log 内容："
    cat server-bench.log
    exit 1
fi
if ! curl -sf -o /dev/null "http://127.0.0.1:$PORT/"; then
    echo "==> 错误：健康检查失败，服务器未正常响应，server-bench.log 内容："
    cat server-bench.log
    exit 1
fi
echo "==> 服务器就绪 (pid=$SERVER_PID)"

URL="http://127.0.0.1:$PORT/index.html"
R1=bench-result-keepalive.txt
R2=bench-result-short.txt

if command -v wrk >/dev/null 2>&1; then
    echo "==> [1/2] Keep-Alive 长连接压测 (wrk, 4线程 x 200连接, 10s) ..."
    wrk -t4 -c200 -d10s --latency "$URL" | tee "$R1"

    echo "==> [2/2] 短连接压测 (wrk + Connection: close, 4线程 x 100连接, 10s) ..."
    wrk -t4 -c100 -d10s -H "Connection: close" "$URL" | tee "$R2"
elif command -v ab >/dev/null 2>&1; then
    echo "==> [1/2] Keep-Alive 长连接压测 (ab -k, 200并发, 共5万请求) ..."
    ab -k -n 50000 -c 200 "$URL" | tee "$R1"

    echo "==> [2/2] 短连接压测 (ab, 100并发, 共2万请求) ..."
    ab -n 20000 -c 100 "$URL" | tee "$R2"
else
    echo "未找到 wrk 或 ab，请先安装："
    echo "  Ubuntu/Debian: sudo apt install wrk   或   sudo apt install apache2-utils"
    exit 1
fi

# 3. 汇总关键指标，避免被刷屏淹没
echo ""
echo "==================== 关键结果汇总 ===================="
grep -E "Requests/sec|Socket errors|Latency Distribution|99%|requests in|Requests per second|Failed requests" "$R1" "$R2" 2>/dev/null
echo "====================================================="
echo "完整结果: $R1 / $R2    服务器日志: server-bench.log"
echo ""
echo "==> 重点确认：Socket errors / Failed requests 是否全为 0"
echo "==> 把 QPS、平均/P99 延迟、错误数填入 README 对比表"
