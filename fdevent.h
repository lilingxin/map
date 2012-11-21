#ifndef _FD_EVENT_H__
#define _FD_EVENT_H__

#include "util.h"

#ifdef __cplusplus
extern "C" {
#endif 

struct event {
    int epfd;
    struct {
        int num;
        struct epoll_event* e;
    } ees;
    struct {
        int num;
        int* fds;
    } fired;
};

int event_init(struct event* ev, int max_fd_num);
int event_term(struct event* ev);
int event_add(struct event* ev, int fd, int mask);
int event_del(struct event* ev, int fd);
int event_poll(struct event* ev, int milliseconds);

#ifdef __cplusplus
}
#endif

#endif /* _FD_EVENT_H__ */
