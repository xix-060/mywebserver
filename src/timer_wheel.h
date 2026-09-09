#ifndef TIMER_WHEEL_H
#define TIMER_WHEEL_H

/*
 * 时间轮定时器：管理 Keep-Alive 空闲连接
 *
 * 主线程每秒调用一次 timer_wheel_tick() 推进一格；
 * 连接被取出处理时 timer_wheel_remove() 取消超时；
 * 处理完重新挂回 epoll 时 timer_wheel_add() 重新计时。
 * 到期连接由回调关闭，释放线程池模型下长期不活跃连接占用的资源。
 */

typedef void (*timer_callback)(int fd, void *arg);

typedef struct timer_wheel_t timer_wheel_t;

/* 创建时间轮，slot_count 为槽位数（tick 间隔 1 秒，一圈 = slot_count 秒） */
timer_wheel_t *timer_wheel_create(int slot_count);

/* 添加定时器：timeout_sec 秒后触发 cb(fd, arg) */
int  timer_wheel_add(timer_wheel_t *wheel, int fd, int timeout_sec,
                     timer_callback cb, void *arg);

/* 取消 fd 对应的定时器（连接开始处理数据时调用） */
void timer_wheel_remove(timer_wheel_t *wheel, int fd);

/* 时间推进一格（主线程每秒调用），触发到期定时器的回调 */
void timer_wheel_tick(timer_wheel_t *wheel);

void timer_wheel_destroy(timer_wheel_t *wheel);

#endif /* TIMER_WHEEL_H */
