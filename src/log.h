#ifndef LOG_H
#define LOG_H

/* 日志级别（数值越小越详细） */
#define LOG_DEBUG 0
#define LOG_INFO  1
#define LOG_WARN  2
#define LOG_ERROR 3

/* 初始化异步日志：filepath 为日志文件路径（可为 NULL 表示只输出到 stderr） */
int  log_init(const char *filepath, int level);

/* 内部接口，业务代码请使用下方宏 */
void log_write(int level, const char *file, int line, const char *fmt, ...);

/* 关闭日志：唤醒后台线程刷盘并退出 */
void log_shutdown(void);

#define log_debug(...) log_write(LOG_DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#define log_info(...)  log_write(LOG_INFO,  __FILE__, __LINE__, __VA_ARGS__)
#define log_warn(...)  log_write(LOG_WARN,  __FILE__, __LINE__, __VA_ARGS__)
#define log_error(...) log_write(LOG_ERROR, __FILE__, __LINE__, __VA_ARGS__)

#endif /* LOG_H */
