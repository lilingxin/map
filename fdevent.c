#include <sys/epoll.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <error.h>
#include <errno.h>

#include "util.h"
#include "fdevent.h"

int event_init(struct event* ev, int max_fd_num)
{
    ev->epfd = epoll_create(max_fd_num);
    check_error(ev->epfd > 0, "epoll_create error!");
    ev->ees.num = max_fd_num;
    safe_malloc(ev->ees.e, *(ev->ees.e), max_fd_num); 
    safe_malloc(ev->fired, *(ev->fired), max_fd_num);
    return 0;
}

int event_term(struct event* ev)
{
    safe_free(ev->ees.e);
    safe_free(ev->fired);
    close(ev->epfd);
    return 0;
}

int event_add(struct event *ev, int fd, int mask)
{
    struct epoll_event ee;
    ee.events = 0;
    ee.data.u64 = 0;
    ee.data.fd = fd;
    ee.events |= mask;
    epoll_ctl(ev->epfd, EPOLL_CTL_ADD, fd, &ee);
    return 0;
}

int event_del(struct event* ev, int fd) 
{
    struct epoll_event ee;
    epoll_ctl(ev->epfd, EPOLL_CTL_DEL, fd, &ee);
    return 0;
}

int event_poll(struct event *ev, int milliseconds)
{
    int rc, numevents = 0;
    rc = epoll_wait(ev->epfd, ev->ees.e, ev->ees.num, milliseconds);
    if (rc > 0) {
        numevents = rc;
        for (int i = 0; i < numevents; ++i) {
            int mask = 0;
            struct epoll_event *e = ev->ees.e + i;
            if (e->events | EPOLLIN) mask |= EPOLLIN;
            if (e->events | EPOLLOUT) mask |= EPOLLOUT;
            ev->fired[i].fd = e->data.fd;
            ev->fired[i].mask = mask;
        }
    }
    return numevents;
}
