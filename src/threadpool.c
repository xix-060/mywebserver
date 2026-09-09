/*
 * threadpool.c — 固定大小线程池（生产者-消费者模型）
 *
 * 主线程（生产者）accept 到连接后把任务追加到任务链表尾部，
 * 通过条件变量唤醒空闲工作线程（消费者）执行；
 * 任务队列无界（节点动态分配），工作线程数固定，避免频繁创建/销毁线程。
 */
#include "threadpool.h"

#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>

typedef struct task
{
    void (*func)(void *);
    void *arg;
    struct task *next;
} task_t;

struct threadpool_t
{
    pthread_mutex_t lock;
    pthread_cond_t  notify;
    pthread_t *threads;
    task_t *head;       /* 队头：工作线程取任务 */
    task_t *tail;       /* 队尾：主线程放任务 */
    int thread_count;
    int shutdown;
};

static void *worker_loop(void *arg)
{
    threadpool_t *pool = (threadpool_t *)arg;

    for (;;)
    {
        pthread_mutex_lock(&pool->lock);
        while (pool->head == NULL && !pool->shutdown)
        {
            pthread_cond_wait(&pool->notify, &pool->lock);
        }
        /* 关闭标志置位且队列已空，线程退出 */
        if (pool->shutdown && pool->head == NULL)
        {
            pthread_mutex_unlock(&pool->lock);
            break;
        }

        task_t *task = pool->head;
        pool->head = task->next;
        if (pool->head == NULL)
        {
            pool->tail = NULL;
        }
        pthread_mutex_unlock(&pool->lock);

        task->func(task->arg); /* 锁外执行业务逻辑 */
        free(task);
    }
    return NULL;
}

threadpool_t *threadpool_create(int thread_count)
{
    if (thread_count <= 0)
    {
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        thread_count = (n > 0) ? (int)n : 4;
    }

    threadpool_t *pool = (threadpool_t *)calloc(1, sizeof(threadpool_t));
    if (pool == NULL)
    {
        return NULL;
    }
    pool->thread_count = thread_count;
    pool->threads = (pthread_t *)calloc(thread_count, sizeof(pthread_t));
    if (pool->threads == NULL)
    {
        free(pool);
        return NULL;
    }

    pthread_mutex_init(&pool->lock, NULL);
    pthread_cond_init(&pool->notify, NULL);

    for (int i = 0; i < thread_count; i++)
    {
        if (pthread_create(&pool->threads[i], NULL, worker_loop, pool) != 0)
        {
            /* 创建失败：销毁已创建的线程 */
            pool->shutdown = 1;
            pthread_cond_broadcast(&pool->notify);
            for (int j = 0; j < i; j++)
            {
                pthread_join(pool->threads[j], NULL);
            }
            pthread_mutex_destroy(&pool->lock);
            pthread_cond_destroy(&pool->notify);
            free(pool->threads);
            free(pool);
            return NULL;
        }
    }
    return pool;
}

int threadpool_add(threadpool_t *pool, void (*routine)(void *), void *arg)
{
    task_t *task = (task_t *)malloc(sizeof(task_t));
    if (task == NULL)
    {
        return -1;
    }
    task->func = routine;
    task->arg = arg;
    task->next = NULL;

    pthread_mutex_lock(&pool->lock);
    if (pool->tail != NULL)
    {
        pool->tail->next = task;
    }
    else
    {
        pool->head = task;
    }
    pool->tail = task;
    pthread_cond_signal(&pool->notify); /* 唤醒一个空闲工作线程 */
    pthread_mutex_unlock(&pool->lock);
    return 0;
}

void threadpool_destroy(threadpool_t *pool)
{
    if (pool == NULL)
    {
        return;
    }

    pthread_mutex_lock(&pool->lock);
    pool->shutdown = 1;
    pthread_cond_broadcast(&pool->notify); /* 通知所有工作线程 */
    pthread_mutex_unlock(&pool->lock);

    for (int i = 0; i < pool->thread_count; i++)
    {
        pthread_join(pool->threads[i], NULL);
    }

    pthread_mutex_destroy(&pool->lock);
    pthread_cond_destroy(&pool->notify);
    free(pool->threads);
    free(pool);
}
