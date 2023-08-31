/*
Description:
    Defined a timer class based on an ascending double-line linked list, in which timers is sorted from short to long by their expire time 
    (list head is the first to expire, tail is the last). 
    The expire time for each connection is 15s since its initialization or last interation with the server, any data exchange will renew the timer.
    The timer list will be checked by root process for each 5s, timeout connection will be considered inactive and closed. 
*/

#ifndef LST_TIMER
#define LST_TIMER

#include <time.h>

#include "log.h"

class util_timer;
// Data for a connection
struct client_data
{
    // Client's socket fd and address
    sockaddr_in address;
    int sockfd;
    // Connection's timer
    util_timer *timer;
};

class util_timer
{
public:
    util_timer() : prev(NULL), next(NULL) {}

public:
    // Expire time
    time_t expire;
    // Callback function at timeout
    void (*cb_func)(client_data *);
    client_data *user_data;
    // Pointers of the linked timer list
    util_timer *prev;
    util_timer *next;
};

// Container for timer list
class sort_timer_lst
{
public:
    sort_timer_lst() : head(NULL), tail(NULL) {}
    ~sort_timer_lst()
    {
        util_timer *tmp = head;
        while (tmp)
        {
            head = tmp->next;
            delete tmp;
            tmp = head;
        }
    }
    // Add a new timer to list
    void add_timer(util_timer *timer)
    {
        if (!timer)
        {
            return;
        }
        if (!head)
        {
            head = tail = timer;
            return;
        }
        // If the new timer is shorter than current head, then insert it to head
        if (timer->expire < head->expire)
        {
            timer->next = head;
            head->prev = timer;
            head = timer;
            return;
        }
        // Otherwise insert timer to its right position after literation and comparisons with elements in the list
        add_timer(timer, head);
    }
    // Adjust potision of timer when it is renewed
    void adjust_timer(util_timer *timer)
    {
        if (!timer)
        {
            return;
        }
        util_timer *tmp = timer->next;
        // If timer was at the tail of list, there is no need to adjust (next new timer is always longer)
        if (!tmp || (timer->expire < tmp->expire))
        {
            return;
        }
        // If timer was the head, remove it from list and insert the timer again
        if (timer == head)
        {
            head = head->next;
            head->prev = NULL;
            timer->next = NULL;
            add_timer(timer, head);
        }
        // If timer was and internal element of the list, remove it and insert the timer again
        else
        {
            timer->prev->next = timer->next;
            timer->next->prev = timer->prev;
            add_timer(timer, timer->next);
        }
    }
    // Delete timer
    void del_timer(util_timer *timer)
    {
        if (!timer)
        {
            return;
        }
        // If there is only one timer in the list
        if ((timer == head) && (timer == tail))
        {
            delete timer;
            head = NULL;
            tail = NULL;
            return;
        }
        // Delete the head 
        if (timer == head)
        {
            head = head->next;
            head->prev = NULL;
            delete timer;
            return;
        }
        // Delete the tail
        if (timer == tail)
        {
            tail = tail->prev;
            tail->next = NULL;
            delete timer;
            return;
        }
        timer->prev->next = timer->next;
        timer->next->prev = timer->prev;
        delete timer;
    }
    // Timeout event handler
    void tick()
    {
        if (!head)
        {
            return;
        }
        LOG_INFO("%s", "timer tick");
        Log::get_instance()->flush();
        // Get current time
        time_t cur = time(NULL);
        util_timer *tmp = head;
        while (tmp)
        {
            // Break until reaches an unexpired timer
            // The timer stores an absolute time, therefore, if current time is samller than current iterator
            // all timers behind current iterator do not expire at current time
            if (cur < tmp->expire)
            {
                break;
            }
            // If current timer expire, call the callbakc function and timeout handler
            tmp->cb_func(tmp->user_data);
            // Remove the expired timer, and reset the head
            // As it is an ascending list, head timer is always the first to expire
            head = tmp->next;
            if (head)
            {
                head->prev = NULL;
            }
            delete tmp;
            tmp = head;
        }
    }

private:
    // Called by public add_timer and adjust_time
    // Insert timer to list body after iteration
    void add_timer(util_timer *timer, util_timer *lst_head)
    {
        util_timer *prev = lst_head;
        util_timer *tmp = prev->next;
        // Iteration and comparison with each element of the list
        while (tmp)
        {
            if (timer->expire < tmp->expire)
            {
                prev->next = timer;
                timer->next = tmp;
                tmp->prev = timer;
                timer->prev = prev;
                break;
            }
            prev = tmp;
            tmp = tmp->next;
        }
        // If tmp->next == null, insert the timer to tail
        if (!tmp)
        {
            prev->next = timer;
            timer->prev = prev;
            timer->next = NULL;
            tail = timer;
        }
    }

private:
    util_timer *head;
    util_timer *tail;
};

#endif
