#ifndef THREADPOOL_H
#define THREADPOOL_H

typedef struct threadpool_t threadpool_t;

/*
 * 创建线程池
 *   thread_count : 工作线程数量（<=0 时按 CPU 核数自动设置）
 *   返回 NULL 表示创建失败
 */
threadpool_t *threadpool_create(int thread_count);

/* 向线程池提交一个任务（routine 由某个工作线程执行） */
int threadpool_add(threadpool_t *pool, void (*routine)(void *), void *arg);

/* 销毁线程池：等待队列中所有任务执行完毕后回收线程 */
void threadpool_destroy(threadpool_t *pool);

#endif /* THREADPOOL_H */
