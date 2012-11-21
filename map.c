#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <memory.h>
#include <stdlib.h>
#include <error.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <getopt.h>
#include <sys/types.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/wait.h>

#include "util.h"
#include "fdevent.h"

#define MAX_BUF_SIZE 4096 * 64

struct map {
    char* cmd;
    long mapper;
    bool need_ev;
    int child_num;
    struct {
        int pipe;
        int pid;
    } *child;
};

static struct map g_map = {
    .cmd = NULL,
    .mapper = 2,
    .need_ev = true,
    .child_num = 0,
    .child = NULL
};

static struct event g_ev = {
    .epfd = -1,
    .ees = {0, NULL},
    .fired = NULL
};

static char* last_component(const char* in) 
{
    char* pos = strrchr(in, (int)'/');
    return (char*)++pos;
}

static int setnoblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        error(EXIT_FAILURE, errno, "fcntl F_GETFL error!");
    }

    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int open_file(const char* fname)
{
    int fd = 0;
    if ((fd = open(fname, O_RDONLY)) < 0) {
        error(EXIT_FAILURE, errno, "open %s failed!", fname);
    }
    return fd;
}

static int cread(int fd, char* buf, size_t size)
{
    int count = 0;
    char* beg = buf;

    while (size > 0) {
        int nread = read(fd, beg, size);
        if (nread < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) break;
            error(EXIT_FAILURE, errno, "read error!");
        }
        
        if (nread == 0) break; 
        count += nread;
        beg = buf + count;
        size -= nread;
    }
    return count;
}

static int cwrite(int fd, char* buf, size_t size) 
{
    int count = 0;
    char* beg = buf;
    while (size > 0) {
        int nwrite = write(fd, beg, size);
        if (nwrite < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) break;
            error(EXIT_FAILURE, errno, "write error!");
        }
        count += nwrite;
        beg = buf + count;
        size -= nwrite;
    }
    return count;
}

void init_data(struct map* m)
{
    m->child_num = 0;
    safe_malloc(m->child, *(m->child), m->mapper);
}

void free_data(struct map* m)
{
    safe_free(m->child);
}

void spawn_child(struct map* m)
{
    int pid = -1;
    int fds[2] = {-1, -1};
    if (pipe(fds) < 0)
        error(EXIT_FAILURE, errno, "pipe error!");
     
    if ((pid = fork()) < 0) 
        error(EXIT_FAILURE, errno, "fork error!");

    if (pid == 0) {
        close(fds[1]);
        for (int i = 0; i < m->child_num; ++i) 
            close(m->child[i].pipe);
        if (dup2(fds[0], 0) < 0)
            error(EXIT_FAILURE, errno, "dup2 error!");
        close(fds[0]);
        
        char* shell = getenv("SHELL");
        if (shell == NULL)
            shell = "/bin/sh";
        execl(shell, last_component(shell), "-c", m->cmd, NULL);
        error(EXIT_FAILURE, errno, "execl error!");
    }
    close(fds[0]);
    m->child[m->child_num].pipe = fds[1];
    m->child[m->child_num].pid = pid;
    ++(m->child_num);
}

void spawn_children(struct map* m)
{
    for (int i = 0; i < m->mapper; ++i) {
        spawn_child(m);
    }
}

void wait_children(struct map* m, struct event* ev, int options)
{
    int status = -1;
    int pid = -1;
    while ( (pid = waitpid(-1, &status, options)) > 0 ) {
        for (int i = 0; i < m->child_num; ++i) {
            if (m->child[i].pid == pid) {
                if (m->child[i].pipe != -1) {
                    if (m->need_ev) event_del(ev, m->child[i].pipe);
                    close(m->child[i].pipe);
                    --(ev->ees.num);
                }
            }
        }

        if (WIFSIGNALED(status)) {
            int signo = WTERMSIG(status);
            log("child [%d] exit by signal %d\n", pid, signo);
        }
    }
}

int init_event(struct event* ev, struct map* m)
{
    event_init(ev, m->mapper);
    for (int i = 0; i < m->mapper; ++i) {
        setnoblocking(m->child[i].pipe);
        event_add(ev, m->child[i].pipe, EPOLLOUT);
    }
}

static int remove_fired_fd(struct event* ev, int* num, int fd) 
{
    for (int i = 0; i < *num; ++i) {
        if (fd == ev->fired[i].fd) {
            ev->fired[i] = ev->fired[--(*num)];
            return ev->fired[i].fd;
        }
    } 

    error(EXIT_FAILURE, errno, "not found pipe_out %d \n", fd);
    return -1;
}

static void write_fired_fd(int fd, char* buf, size_t size, struct event* ev, int* fired_num)
{
    if (cwrite(fd, buf, size) < size) {
        int tfd = remove_fired_fd(ev, fired_num, fd);
        return write_fired_fd(tfd, buf, size, ev, fired_num);
    }
}

void lines_split_for_write(struct event* ev, int rfd)
{
    bool is_need_read = true;
    int nread = 0;
    char* buffer = (char*)malloc(MAX_BUF_SIZE + 1); 
    char* bp, *bout, *eob;
    while (true) {
        int num = event_poll(ev, 1000);
        if (num == -1) break;
        if (num == 0) continue;
        for (int i = 0; i < num; ) {
            if (is_need_read) {
                nread = cread(rfd, buffer, MAX_BUF_SIZE);
                bp = bout = buffer;
                eob = buffer + nread;
                *eob = '\n';
            }
             
            bp = memchr(bp, '\n', eob - bp + 1);
            is_need_read = (bp == eob);
            if (bp != eob) {
                write_fired_fd(ev->fired[i++].fd, bout, ++bp - bout, ev, &num);
                bout = bp;
                continue;
            }

            if (eob != bout) 
                write_fired_fd(ev->fired[i].fd, bout, eob - bout, ev, &num);
            if (nread != MAX_BUF_SIZE) {
                goto exit;
            }
        }
    }
    
exit:
    free(buffer);
}

void close_all(struct map* m)
{
    for (int i = 0; i < m->child_num; ++i) 
    {
        close(m->child[i].pipe);
        m->child[i].pipe = -1;
    }
}

void sig_handler(int signo)
{
    if (signo == SIGQUIT || signo == SIGINT || signo == SIGTERM) {
        for (int i = 0; i < g_map.child_num; ++i) 
            kill(g_map.child[i].pid, signo);
        wait_children(&g_map, &g_ev, 0);
        free_data(&g_map);
        exit(1);
    }

    if (signo == SIGCHLD) {
        wait_children(&g_map, &g_ev, WNOHANG);
    }
}


void usage(const char* pro)
{
    printf("Usage %s: \n"
            "   [-m | --mapper] the mapper count, default is 2\n"
            "   [-f | --file] the input file, if not needed input set 'none'(must)\n"
            "                 default is stdin\n"
            "   [-d | --cmd] the executed program, not empty\n"
            "   [-h | --help] the help Usage\n", pro);
}

int getopts(int argc, char** argv, struct map* m)
{
    const char* short_option = "c:m:f:h";
    struct option long_option[] = {
        {"mapper", required_argument, NULL, 'm'},
        {"file", required_argument, NULL, 'f'},
        {"cmd", required_argument, NULL, 'c'},
        {"help", no_argument, NULL, 'h'},
    };    
    
    int c = -1;
    while ((c = getopt_long(argc, argv, short_option, long_option, 0)) != -1)
    {
        int size = -1;
        switch (c) {
        case 'm':
            m->mapper = atol(optarg);
            break;
        case 'c':
            size = strlen(optarg);
            m->cmd = (char*) malloc (sizeof(char) * (size + 1));
            memset(m->cmd, 0, size);
            memcpy(m->cmd, optarg, size);
            break;
        case 'f':
            if (strncasecmp(optarg, "none", 4) == 0) {
                m->need_ev = false;
            } else {
                int fd = open_file(optarg);
                dup2(fd, STDIN_FILENO);
                close(fd);
                m->need_ev = true;
            }
            break;            
        case 'h':
            usage(argv[0]);
            exit(1); 
        }
    }

    return 0;
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        usage(argv[0]);
        exit(1);
    }

    getopts(argc, argv, &g_map);
    if (g_map.cmd == NULL) {
        usage(argv[0]);
        exit(1);
    }

    signal(SIGPIPE, SIG_IGN);
    signal(SIGQUIT, sig_handler);
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGCHLD, sig_handler);
    
    init_data(&g_map);
    spawn_children(&g_map); 

    if (g_map.need_ev) {
        init_event(&g_ev, &g_map);
        lines_split_for_write(&g_ev, STDIN_FILENO);
        event_term(&g_ev);
    }
    close_all(&g_map);
    wait_children(&g_map, &g_ev, 0);
    free_data(&g_map);
      
    return 0;    
}
