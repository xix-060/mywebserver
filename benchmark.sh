#!/bin/bash
# benchmark.sh — 压测脚本（需要安装 wrk 或 apache2-utils(ab)）
# 用法:
#   ./benchmark.sh              # 默认 8080 端口、线程数=CPU核数
#   ./benchmark.sh 8080 4       # 指定端口和工作线程数
#
# 压测前可先编译 v1（legacy/myweb_v1_thread.c）对比两个版本的 QPS：
#   gcc -O2 -pthread -o /tmp/myweb_v1 legacy/myweb_v1_thread.c
set -e

PORT=${1:-8080}
THREADS=${2:-0}

cd "$(dirname "$0")"
make

echo "==> 启动服务器: port=$PORT threads=$THREADS"
./myweb -p "$PORT" -t "$THREADS" &
SERVER_PID=$!
trap "kill $SERVER_PID 2>/dev/null" EXIT
sleep 1

URL="http://127.0.0.1:$PORT/index.html"

if command -v wrk >/dev/null 2>&1; then
    echo ""
    echo "==> [1/2] Keep-Alive 长连接压测 (wrk, 4线程 x 200连接, 10s)"
    wrk -t4 -c200 -d10s --latency "$URL"

    echo ""
    echo "==> [2/2] 短连接压测 (wrk + -H 'Connection: close', 4线程 x 100连接, 10s)"
    wrk -t4 -c100 -d10s -H "Connection: close" "$URL"
elif command -v ab >/dev/null 2>&1; then
    echo ""
    echo "==> [1/2] Keep-Alive 长连接压测 (ab -k, 200并发, 共5万请求)"
    ab -k -n 50000 -c 200 "$URL"

    echo ""
    echo "==> [2/2] 短连接压测 (ab, 100并发, 共2万请求)"
    ab -n 20000 -c 100 "$URL"
else
    echo "未找到 wrk 或 ab，请先安装："
    echo "  Ubuntu/Debian: sudo apt install wrk   或   sudo apt install apache2-utils"
    exit 1
fi

echo ""
echo "==> 压测完成，可将上面的 QPS / 延迟数据填入 README 对比表"
