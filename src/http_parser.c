/*
 * http_parser.c — HTTP 请求头解析
 *
 * 与 v1 版本 sscanf 只解析请求行不同，这里：
 *   1. 定位完整请求头（\r\n\r\n），支持跨 recv 的半包；
 *   2. 逐行解析头部字段，识别 Connection: keep-alive/close；
 *   3. 对路径做 URL 解码（%xx），解码后再检测 ".."，
 *      防止 %2e%2e 形式的目录遍历绕过；
 *   4. 剥离查询字符串（? 之后的内容）。
 */
#include "http_parser.h"

#include <string.h>
#include <stdio.h>
#include <strings.h>

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* URL 解码：%xx → 对应字节，非法转义原样保留 */
static void url_decode(char *out, int out_size, const char *in)
{
    int o = 0;
    for (int i = 0; in[i] != '\0' && o < out_size - 1; )
    {
        if (in[i] == '%')
        {
            int hi = hex_value(in[i + 1]);
            int lo = hex_value(in[i + 2]);
            if (hi >= 0 && lo >= 0)
            {
                out[o++] = (char)(hi * 16 + lo);
                i += 3;
                continue;
            }
        }
        out[o++] = in[i++];
    }
    out[o] = '\0';
}

/* 在 buf[0..len) 范围内查找子串 needle（不依赖 '\0' 结尾） */
static const char *mem_find(const char *buf, int len, const char *needle)
{
    int nlen = (int)strlen(needle);
    if (nlen > len)
    {
        return NULL;
    }
    for (int i = 0; i <= len - nlen; i++)
    {
        if (buf[i] == needle[0] && memcmp(buf + i, needle, nlen) == 0)
        {
            return buf + i;
        }
    }
    return NULL;
}

parse_result_t http_parse(const char *buf, int len, http_request_t *req)
{
    memset(req, 0, sizeof(*req));

    /* 1. 请求头必须以 \r\n\r\n 结束，否则数据不完整 */
    const char *header_end = mem_find(buf, len, "\r\n\r\n");
    if (header_end == NULL)
    {
        return PARSE_INCOMPLETE;
    }
    req->header_len = (int)(header_end - buf) + 4;

    /* 2. 解析请求行：METHOD SP PATH SP VERSION */
    char version[16] = {0};
    char raw_path[512] = {0};
    if (sscanf(buf, "%7s %511s %15s", req->method, raw_path, version) != 3)
    {
        return PARSE_ERROR;
    }

    /* 3. 去掉查询字符串 */
    char *q = strchr(raw_path, '?');
    if (q != NULL)
    {
        *q = '\0';
    }

    /* 4. URL 解码（必须在安全检查之前，防 %2e%2e 绕过） */
    url_decode(req->path, (int)sizeof(req->path), raw_path);

    /* 5. 安全防护：解码后的路径包含 .. 一律拒绝 */
    if (strstr(req->path, "..") != NULL)
    {
        return PARSE_ERROR;
    }

    /* 6. 逐行解析头部字段 */
    req->keep_alive = (strcmp(version, "HTTP/1.1") == 0); /* HTTP/1.1 默认长连接 */

    const char *p = mem_find(buf, len, "\r\n");
    if (p == NULL)
    {
        return PARSE_ERROR;
    }
    p += 2; /* 跳过请求行 */

    while (p < header_end)
    {
        const char *le = (const char *)memchr(p, '\r', (size_t)(header_end - p));
        if (le == NULL)
        {
            break;
        }
        if (strncasecmp(p, "Connection:", 11) == 0)
        {
            const char *v = p + 11;
            while (v < le && (*v == ' ' || *v == '\t'))
            {
                v++;
            }
            if (strncasecmp(v, "keep-alive", 10) == 0)
            {
                req->keep_alive = 1;
            }
            else if (strncasecmp(v, "close", 5) == 0)
            {
                req->keep_alive = 0;
            }
        }
        p = le + 2; /* 下一行 */
    }

    return PARSE_COMPLETE;
}
