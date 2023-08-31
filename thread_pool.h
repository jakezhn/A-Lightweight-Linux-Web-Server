/*
Author: Hongnan Zhang
Class: ECE6122
Last Date Modified: 2022/12/6
Description:
    Defines a thread pool class. Multiple threads are allocated statically at initialization to avoid future overhead of frequently creating and deleting threads.
    Main thread's job is to listen on server port for new http request, and when a new request arrives, it will be stored into a request queue (a list).
    Then a worker thread in thread pool will be awakened, to acquire and handle the newly arrived http request from the request queue.
*/
#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>

#include "locker.h"
#include "connection_pool.h"

template <typename T>
class threadpool
{
public:
    // thread_number is the number staticly allocated threads in thread pool, it is determined according to the number of cpu cores
    // max_request is the maximum number of threads allowed in the queue
    // connPool points to the connection pool
    threadpool(connection_pool *connPool, int thread_number = 8, int max_request = 10000);
    ~threadpool();
    // Append new request to the request queue
    bool append(T *request);

private:
    // Function run by worker thread, keeps handling requests from request queue
    static void *worker(void *arg);
    void run();

private:
    // Number of threads in thread pool
    int m_thread_number;
    // Max number of requests in request queue
    int m_max_requests;
    // Thread pool array
    pthread_t *m_threads;
    // Request queue
    std::list<T *> m_requestQueue;
    locker m_queuelocker;
    // Semaphore for request queue
    sem m_queuestat;
    bool m_stop;
    connection_pool *m_connPool;
};

// Create threadd pool instance
template <typename T>
threadpool<T>::threadpool( connection_pool *connPool, int thread_number, int max_requests) : m_thread_number(thread_number), m_max_requests(max_requests), m_stop(false), m_threads(NULL),m_connPool(connPool)
{
    if (thread_number <= 0 || max_requests <= 0)
        throw std::exception();
    // Initialize thread by id
    m_threads = new pthread_t[m_thread_number];
    if (!m_threads)
        throw std::exception();
    for (int i = 0; i < thread_number; ++i)
    {
        // Create new worker threads
        if (pthread_create(m_threads + i, NULL, worker, this) != 0)
        {
            delete[] m_threads;
            throw std::exception();
        }
        // Detach thread, in order to reclaim it after terminated
        if (pthread_detach(m_threads[i]))
        {
            delete[] m_threads;
            throw std::exception();
        }
    }
}

// Delete thread pool instance
template <typename T>
threadpool<T>::~threadpool()
{
    delete[] m_threads;
    m_stop = true;
}
// Append new request to queue
template <typename T>
bool threadpool<T>::append(T *request)
{
    // Lock and unlock queue before and after accessing it
    m_queuelocker.lock();
    if (m_requestQueue.size() > m_max_requests)
    {
        m_queuelocker.unlock();
        return false;
    }
    m_requestQueue.push_back(request);
    m_queuelocker.unlock();
    // Post the queue semaphore
    m_queuestat.post();
    return true;
}
// Call run() to process http request in a worker thread
template <typename T>
void *threadpool<T>::worker(void *arg)
{
    // Wake a thread from thread pool
    threadpool *pool = (threadpool *)arg;
    // Get request from request queue, and run http handler
    pool->run();
    return pool;
}
// Get request from request queue, and run http handler
template <typename T>
void threadpool<T>::run()
{
    while (!m_stop)
    {
        // Block untill semaphore of request queue > 1
        m_queuestat.wait();
        // Lock before accessing request queue
        m_queuelocker.lock();
        if (m_requestQueue.empty())
        {
            m_queuelocker.unlock();
            continue;
        }
        T *request = m_requestQueue.front();
        m_requestQueue.pop_front();
        m_queuelocker.unlock();
        if (!request)
            continue;

        // Wake a mysql connection from connection pool
        connectionRAII mysqlcon(&request->mysql, m_connPool);
        // Process http request
        request->process();
    }
}
#endif
