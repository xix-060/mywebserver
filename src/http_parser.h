#ifndef HTTP_PARSER_H
#define HTTP_PARSER_H

/*
 * 增量式 HTTP 请求解析器（状态机思想）
 *
 * 调用方每次收到数据就把缓冲区喂给 http_parse：
 *   - PARSE_INCOMPLETE：请求头还没收全（未遇到 \r\n\r\n），等待更多数据
 *   - PARSE_COMPLETE  ：请求头解析完成，结果在 http_request_t 中
 *   - PARSE_ERROR     ：请求格式错误或路径非法（含目录遍历）
 */

typedef enum
{
    PARSE_INCOMPLETE = 0,
    PARSE_COMPLETE   = 1,
    PARSE_ERROR      = 2
} parse_result_t;

typedef struct
{
    char method[8];      /* 请求方法，如 GET */
    char path[512];      /* URL 解码、去掉查询串后的请求路径 */
    int  keep_alive;     /* 是否保持长连接（HTTP/1.1 默认开启，Connection 头可覆盖） */
    int  header_len;     /* 请求头总字节数（含结尾 \r\n\r\n），用于处理 HTTP 管道化残留 */
} http_request_t;

parse_result_t http_parse(const char *buf, int len, http_request_t *req);

#endif /* HTTP_PARSER_H */
