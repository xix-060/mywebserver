#!/bin/bash
# benchmark.sh — 压测脚本（需要安装 wrk 或 apache2-utils(ab)）
# 用法:
#   ./benchmark.sh              # 默认 8080 端口、线程数=CPU核数
#   ./benchmark.sh 8080 4       # 指定端口和工作线程数
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

echo "==> 启动服务器: port=$PORT threads=${THREADS:-auto}"
./myweb -p "$PORT" -t "$THREADS" &
SERVER_PID=$!
trap "kill $SERVER_PID 2>/dev/null" EXIT

# 2. 等待并确认服务器真的启动成功
sleep 1
if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    echo "==> 错误：服务器进程启动后立刻退出，检查端口权限/二进制是否正常"
    exit 1
fi
if ! curl -sf -o /dev/null "http://127.0.0.1:$PORT/"; then
    echo "==> 错误：健康检查失败，服务器未正常响应"
    exit 1
fi
echo "==> 服务器就绪 (pid=$SERVER_PID)"

URL="http://127.0.0.1:$PORT/index.html"

if command -v wrk >/dev/null 2>&1; then
    echo ""
    echo "==> [1/2] Keep-Alive 长连接压测 (wrk, 4线程 x 200连接, 10s)"
    wrk -t4 -c200 -d10s --latency "$URL"

    echo ""
    echo "==> [2/2] 短连接压测 (wrk + Connection: close, 4线程 x 100连接, 10s)"
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
echo "==> 压测完成（服务器将自动停止），把上面的 QPS/延迟/错误数填入 README 对比表"
