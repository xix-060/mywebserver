/*
 * log.c — 异步日志
 *
 * 业务线程只负责把日志行格式化后压入链表队列并唤醒后台线程；
 * 后台日志线程批量取出写入 stderr 与日志文件，避免磁盘 I/O 阻塞工作线程。
 */
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <pthread.h>
#include <time.h>

typedef struct log_node
{
    char *msg;
    struct log_node *next;
} log_node_t;

static struct
{
    log_node_t *head;
    log_node_t *tail;
    pthread_mutex_t lock;
    pthread_cond_t  cond;
    pthread_t tid;
    int shutdown;
    FILE *fp;
    int level;
} g_log;

/* 后台日志线程：批量取出队列中的日志行落盘 */
static void *log_thread(void *arg)
{
    (void)arg;
    for (;;)
    {
        pthread_mutex_lock(&g_log.lock);
        while (g_log.head == NULL && !g_log.shutdown)
        {
            pthread_cond_wait(&g_log.cond, &g_log.lock);
        }
        log_node_t *batch = g_log.head;
        g_log.head = g_log.tail = NULL;
        int stop = g_log.shutdown;
        pthread_mutex_unlock(&g_log.lock);

        /* 锁外执行慢速 I/O */
        while (batch != NULL)
        {
            log_node_t *node = batch;
            batch = batch->next;

            fputs(node->msg, stderr);
            if (g_log.fp != NULL)
            {
                fputs(node->msg, g_log.fp);
            }
            free(node->msg);
            free(node);
        }
        if (g_log.fp != NULL)
        {
            fflush(g_log.fp);
        }

        if (stop)
        {
            break; /* 关闭标志已置位且队列已排空 */
        }
    }
    return NULL;
}

int log_init(const char *filepath, int level)
{
    memset(&g_log, 0, sizeof(g_log));
    g_log.level = level;

    if (filepath != NULL)
    {
        g_log.fp = fopen(filepath, "a");
        if (g_log.fp == NULL)
        {
            return -1;
        }
    }

    pthread_mutex_init(&g_log.lock, NULL);
    pthread_cond_init(&g_log.cond, NULL);

    if (pthread_create(&g_log.tid, NULL, log_thread, NULL) != 0)
    {
        return -1;
    }
    return 0;
}

void log_write(int level, const char *file, int line, const char *fmt, ...)
{
    if (level < g_log.level)
    {
        return;
    }

    static const char *level_name[] = {"DEBUG", "INFO", "WARN", "ERROR"};

    char buf[1024];
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char tbuf[32];
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &tmv);

    int n = snprintf(buf, sizeof(buf), "[%s] [%-5s] [tid=%lu] ",
                     tbuf, level_name[level], (unsigned long)pthread_self());

    va_list ap;
    va_start(ap, fmt);
    n += vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
    va_end(ap);

    snprintf(buf + n, sizeof(buf) - n, " (%s:%d)\n", file, line);

    log_node_t *node = (log_node_t *)malloc(sizeof(log_node_t));
    if (node == NULL)
    {
        return;
    }
    node->msg = strdup(buf);
    node->next = NULL;
    if (node->msg == NULL)
    {
        free(node);
        return;
    }

    pthread_mutex_lock(&g_log.lock);
    if (g_log.tail != NULL)
    {
        g_log.tail->next = node;
    }
    else
    {
        g_log.head = node;
    }
    g_log.tail = node;
    pthread_cond_signal(&g_log.cond);
    pthread_mutex_unlock(&g_log.lock);
}

void log_shutdown(void)
{
    pthread_mutex_lock(&g_log.lock);
    g_log.shutdown = 1;
    pthread_cond_signal(&g_log.cond);
    pthread_mutex_unlock(&g_log.lock);

    pthread_join(g_log.tid, NULL);

    if (g_log.fp != NULL)
    {
        fclose(g_log.fp);
        g_log.fp = NULL;
    }
    pthread_mutex_destroy(&g_log.lock);
    pthread_cond_destroy(&g_log.cond);
}
