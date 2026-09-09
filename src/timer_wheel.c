/*
 * timer_wheel.c — 单轮时间轮
 *
 * 每个定时器节点根据超时秒数计算：
 *   rotation = timeout / slot_count （需要转的圈数）
 *   slot     = (cur_slot + timeout) % slot_count
 * tick 推进时检查当前槽：rotation>0 则减一并留下，rotation==0 则到期触发回调。
 *
 * 线程模型：
 *   tick 只在主线程调用；add/remove 在主线程（accept/分发）和工作线程
 *   （rearm/close_conn）都会调用，故所有链表操作持锁互斥；
 *   到期回调在锁外执行（回调会 close fd、free 连接上下文）。
 *   数据结构保证不变式：同一 fd 至多一个定时器节点（add 前先去重）。
 */
#include "timer_wheel.h"

#include <stdlib.h>
#include <pthread.h>

typedef struct timer_node
{
    int fd;
    int rotation;
    timer_callback cb;
    void *arg;
    struct timer_node *next;
} timer_node_t;

struct timer_wheel_t
{
    timer_node_t **slots;
    int slot_count;
    int cur_slot;
    pthread_mutex_t lock;
};

timer_wheel_t *timer_wheel_create(int slot_count)
{
    if (slot_count <= 0)
    {
        slot_count = 60;
    }
    timer_wheel_t *w = (timer_wheel_t *)calloc(1, sizeof(timer_wheel_t));
    if (w == NULL)
    {
        return NULL;
    }
    w->slots = (timer_node_t **)calloc((size_t)slot_count, sizeof(timer_node_t *));
    if (w->slots == NULL)
    {
        free(w);
        return NULL;
    }
    w->slot_count = slot_count;
    pthread_mutex_init(&w->lock, NULL);
    return w;
}

int timer_wheel_add(timer_wheel_t *w, int fd, int timeout_sec,
                    timer_callback cb, void *arg)
{
    timer_node_t *node = (timer_node_t *)malloc(sizeof(timer_node_t));
    if (node == NULL)
    {
        return -1;
    }
    node->fd = fd;
    node->cb = cb;
    node->arg = arg;

    if (timeout_sec < 1)
    {
        timeout_sec = 1;
    }

    pthread_mutex_lock(&w->lock);

    /* 防御性保证不变式：同一 fd 至多一个定时器节点，
     * 先摘除可能残留的旧节点（正常时序下不会命中） */
    for (int i = 0; i < w->slot_count; i++)
    {
        timer_node_t **pp = &w->slots[i];
        while (*pp != NULL)
        {
            timer_node_t *cur = *pp;
            if (cur->fd == fd)
            {
                *pp = cur->next;
                free(cur);
            }
            else
            {
                pp = &cur->next;
            }
        }
    }

    node->rotation = timeout_sec / w->slot_count;
    int slot = (w->cur_slot + timeout_sec) % w->slot_count;
    node->next = w->slots[slot];
    w->slots[slot] = node;
    pthread_mutex_unlock(&w->lock);
    return 0;
}

void timer_wheel_remove(timer_wheel_t *w, int fd)
{
    pthread_mutex_lock(&w->lock);
    for (int i = 0; i < w->slot_count; i++)
    {
        timer_node_t **pp = &w->slots[i];
        while (*pp != NULL)
        {
            timer_node_t *node = *pp;
            if (node->fd == fd)
            {
                *pp = node->next; /* 从链表摘除 */
                free(node);
            }
            else
            {
                pp = &node->next;
            }
        }
    }
    pthread_mutex_unlock(&w->lock);
}

void timer_wheel_tick(timer_wheel_t *w)
{
    pthread_mutex_lock(&w->lock);

    timer_node_t *expired = w->slots[w->cur_slot];
    w->slots[w->cur_slot] = NULL;

    int ticked_slot = w->cur_slot;
    w->cur_slot = (w->cur_slot + 1) % w->slot_count;

    /* 分离：未到期（rotation>0）的挂回原槽，到期的收集起来 */
    timer_node_t *keep = NULL;
    timer_node_t *fire = NULL;
    while (expired != NULL)
    {
        timer_node_t *node = expired;
        expired = expired->next;
        if (node->rotation > 0)
        {
            node->rotation--;
            node->next = keep;
            keep = node;
        }
        else
        {
            node->next = fire;
            fire = node;
        }
    }
    if (keep != NULL)
    {
        /* 挂回刚离开的槽，等下一圈到达 */
        timer_node_t *tail = keep;
        while (tail->next != NULL)
        {
            tail = tail->next;
        }
        tail->next = w->slots[ticked_slot];
        w->slots[ticked_slot] = keep;
    }

    pthread_mutex_unlock(&w->lock);

    /* 回调在锁外执行（可能 close fd，不应持锁） */
    while (fire != NULL)
    {
        timer_node_t *node = fire;
        fire = fire->next;
        node->cb(node->fd, node->arg);
        free(node);
    }
}

void timer_wheel_destroy(timer_wheel_t *w)
{
    if (w == NULL)
    {
        return;
    }
    for (int i = 0; i < w->slot_count; i++)
    {
        timer_node_t *node = w->slots[i];
        while (node != NULL)
        {
            timer_node_t *next = node->next;
            free(node);
            node = next;
        }
    }
    pthread_mutex_destroy(&w->lock);
    free(w->slots);
    free(w);
}
