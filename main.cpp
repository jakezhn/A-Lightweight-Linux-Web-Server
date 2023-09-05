#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <cassert>
#include <sys/epoll.h>

#include "locker.h"
#include "thread_pool.h"
#include "timer.h"
#include "http_handler.h"
#include "log.h"
#include "connection_pool.h"

// Max number of file descriptors (called as "fd" below for short)
#define MAX_FD 65536
// Max number of events
#define MAX_EVENT_NUMBER 10000
// Timeslot for timer
#define TIMESLOT 5

// Functions below are defined in http_handler
// Add and remove fd to and from kernel envents table
extern int addFd(int epollfd, int fd, bool one_shot);
extern int remove(int epollfd, int fd);
// Set fd non-blocking, to support write action when the connection peer closes (half-close)
extern int setNonBlocking(int fd);

// Timer for the process
static int pipefd[2];
static sort_timer_lst timer_lst;

static int epollfd = 0;

// Signal handler, keep its last error number and write signal from the writing end of the pipe
void sigHandler(int sig)
{
    int save_errno = errno;
    int msg = sig;
    send(pipefd[1], (char *)&msg, 1, 0);
    errno = save_errno;
}

// Set up a signal
void setSig(int sig, void(handler)(int), bool restart = true)
{
    // Create sigaction
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;
    if (restart)
        sa.sa_flags |= SA_RESTART;
    // Add signals to signal set
    sigfillset(&sa.sa_mask);
    // Call sigaction
    assert(sigaction(sig, &sa, NULL) != -1);
}

// Trigger SIGALRM for each TIMESLOT
void timer_handler()
{
    timer_lst.tick();
    alarm(TIMESLOT);
}

// Callback function for timer to close static connections
void cb_func(client_data *user_data)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    assert(user_data);
    close(user_data->sockfd);
    httpHandler::m_user_count--;
    LOG_INFO("close fd %d", user_data->sockfd);
    Log::get_instance()->flush();
}
// Write a message to connection, used as error message sender
void writeMsg(int connfd, const char *info)
{
    printf("%s", info);
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

int main(int argc, char *argv[])
{
    // Initialize server log
    Log::get_instance()->init("ServerLog", 2000, 800000);

    if (argc <= 1)
    {
        printf("usage: %s ip_address port_number\n", basename(argv[0]));
        return 1;
    }

    int port = atoi(argv[1]);

    setSig(SIGPIPE, SIG_IGN);

    // Create a mysql connection pool
    connection_pool *connPool = connection_pool::GetInstance();
    connPool->init("localhost", "root", "Aa199781.", "mydb", 6000, 8);

    // Creating a thread pool
    threadpool<httpHandler> *pool = NULL;
    try
    {
        pool = new threadpool<httpHandler>(connPool);
    }
    catch (...)
    {
        return 1;
    }

    // Create http connection instances
    httpHandler *users = new httpHandler[MAX_FD];
    assert(users);

    // Initialize Mysql read table
    users->initMysql(connPool);

    // Creaet listen socket
    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(listenfd >= 0);

    // Bind listen socket to server port
    int ret = 0;
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);

    int flag = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
    ret = bind(listenfd, (struct sockaddr *)&address, sizeof(address));
    assert(ret >= 0);
    ret = listen(listenfd, 5);
    assert(ret >= 0);

    // Create kernel events table
    epoll_event events[MAX_EVENT_NUMBER];
    epollfd = epoll_create(5);
    assert(epollfd != -1);

    // Add listen fd to event table
    addFd(epollfd, listenfd, false);
    // Assign event table fd to m_epollfd
    // "m_" indicates a shared variable
    httpHandler::m_epollfd = epollfd;

    // Create socket pipe
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert(ret != -1);
    // Set the write end of the pipe to non-blocking, to support a half-close socket
    setNonBlocking(pipefd[1]);
    addFd(epollfd, pipefd[0], false);
    // Set handler for SIGALRM and SIGTERM
    // SIGALRM -> trigger after a certain period of time
    // SIGTERM -> trigger at termination
    setSig(SIGALRM, sigHandler, false);
    setSig(SIGTERM, sigHandler, false);
    bool stop_server = false;

    client_data *users_timer = new client_data[MAX_FD];

    bool timeout = false;
    // Trigger SIGALRM once for each TIMESLOT
    // In this lab, SIGALRM is sent for each 5s to notice the root process to check the timer list, and close timeout connection
    // Timeout for each connection is 15s since its initialization or last interaction with server, and its timer will be checked for each 5s
    alarm(TIMESLOT);

    while (!stop_server)
    {
        // Wait for new event on listen fd
        int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        if (number < 0 && errno != EINTR)
        {
            LOG_ERROR("%s", "epoll failure");
            break;
        }
        // Process all new events
        for (int i = 0; i < number; i++)
        {
            int sockfd = events[i].data.fd;

            // Process new request on listen fd
            if (sockfd == listenfd)
            {
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof(client_address);

                int connfd = accept(listenfd, (struct sockaddr *)&client_address, &client_addrlength);
                if (connfd < 0)
                {
                    LOG_ERROR("%s:errno is:%d", "accept error", errno);
                    continue;
                }
                // If number of new events exceeds the maximum number allowed
                if (httpHandler::m_user_count >= MAX_FD)
                {
                    writeMsg(connfd, "Internal server busy");
                    LOG_ERROR("%s", "Internal server busy");
                    continue;
                }
                users[connfd].init(connfd, client_address);

                // Initialize user data
                // Create timer, set timeout callback function, and add timer to the ascending linked list
                users_timer[connfd].address = client_address;
                users_timer[connfd].sockfd = connfd;
                util_timer *timer = new util_timer;
                timer->user_data = &users_timer[connfd];
                timer->cb_func = cb_func;
                time_t cur = time(NULL);
                // Set expire time to current time + 5s * 3
                timer->expire = cur + 3 * TIMESLOT;
                // Add timer to connection fd
                users_timer[connfd].timer = timer;
                // Add timer to timer list
                timer_lst.add_timer(timer);
            }
            // Handle error events
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                // Remove timer when IO event error occurs
                util_timer *timer = users_timer[sockfd].timer;
                timer->cb_func(&users_timer[sockfd]);

                if (timer)
                {
                    timer_lst.del_timer(timer);
                }
            }

            // If any signal is triggered
            else if ((sockfd == pipefd[0]) && (events[i].events & EPOLLIN))
            {
                int sig;
                char signals[1024];
                // Read signal from pipe. return -1 when fails, return number of bytes when successes (normally return 1 for signal)
                ret = recv(pipefd[0], signals, sizeof(signals), 0);
                if (ret == -1)
                {
                    continue;
                }
                else if (ret == 0)
                {
                    continue;
                }
                // Handler for SIGALRM and SIGTERM
                else
                {
                    for (int i = 0; i < ret; ++i)
                    {
                        switch (signals[i])
                        {
                        case SIGALRM:
                        {
                            timeout = true;
                            break;
                        }
                        case SIGTERM:
                        {
                            stop_server = true;
                        }
                        }
                    }
                }
            }

            // If there is a read event (message sent from client)
            else if (events[i].events & EPOLLIN)
            {
                // Get the timer of the connection
                util_timer *timer = users_timer[sockfd].timer;
                // Read buffer
                if (users[sockfd].readBuff())
                {
                    LOG_INFO("deal with the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));
                    Log::get_instance()->flush();
                    // Add new event to request queue of thread pool
                    pool->append(users + sockfd);

                    if (timer)
                    {
                        // Renew the timer
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        LOG_INFO("%s", "adjust timer once");
                        Log::get_instance()->flush();
                        // Adjust timer's position in the list
                        timer_lst.adjust_timer(timer);
                    }
                }
                // If readBuff failed (error occurs or connection ends by server), close connection and delete timer
                else
                {
                    timer->cb_func(&users_timer[sockfd]);
                    if (timer)
                    {
                        timer_lst.del_timer(timer);
                    }
                }
            }
            else if (events[i].events & EPOLLOUT)
            {
                util_timer *timer = users_timer[sockfd].timer;
                if (users[sockfd].writeBuff())
                {
                    LOG_INFO("send data to the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));
                    Log::get_instance()->flush();

                    if (timer)
                    {
                        // Renew the timer
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        LOG_INFO("%s", "adjust timer once");
                        Log::get_instance()->flush();
                        // Adjust timer's position in the list
                        timer_lst.adjust_timer(timer);
                    }
                }
                else
                {
                    timer->cb_func(&users_timer[sockfd]);
                    if (timer)
                    {
                        timer_lst.del_timer(timer);
                    }
                }
            }
        }
        // If SIGALARM is triggered (for each 5s), timeout flags is set, then call timer handler to check the timer list
        if (timeout)
        {
            timer_handler();
            timeout = false;
        }
    }
    // Release resource
    close(epollfd);
    close(listenfd);
    close(pipefd[1]);
    close(pipefd[0]);
    delete[] users;
    delete[] users_timer;
    delete pool;
    return 0;
}
