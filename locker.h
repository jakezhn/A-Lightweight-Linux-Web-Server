#ifndef LOCKER_H
#define LOCKER_H

#include <exception>
#include <pthread.h>
#include <semaphore.h>

// Semaphore wrapper class for managing semaphores
class sem {
public:
    sem(int num = 0) {
        if (sem_init(&m_sem, 0, num) != 0) {
            throw std::exception(); 
        }
    }

    ~sem() {
        sem_destroy(&m_sem);
    }

    // Wait decreases the semaphore
    bool wait() {
        return sem_wait(&m_sem) == 0;
    }

    // Post increases the semaphore
    bool post() {
        return sem_post(&m_sem) == 0;
    }

private:
    sem_t m_sem;  
};

class locker {
public:
    locker() {
        if (pthread_mutex_init(&m_mutex, NULL) != 0) {
            throw std::exception();
        }
    }

    ~locker() {
        pthread_mutex_destroy(&m_mutex);
    }

    bool lock() {
        return pthread_mutex_lock(&m_mutex) == 0;
    }

    bool unlock() {
        return pthread_mutex_unlock(&m_mutex) == 0;
    }

    pthread_mutex_t *get() {
        return &m_mutex;
    }

private:
    pthread_mutex_t m_mutex; 
};

// Condition variable class for managing condition variables
class cond {
public:
    cond() {
        if (pthread_cond_init(&m_cond, NULL) != 0) {
            throw std::exception();
        }
    }

    ~cond() {
        pthread_cond_destroy(&m_cond);
    }

    bool wait(pthread_mutex_t *m_mutex) {
        return pthread_cond_wait(&m_cond, m_mutex) == 0;
    }

    bool timewait(pthread_mutex_t *m_mutex, struct timespec t) {
        return pthread_cond_timedwait(&m_cond, m_mutex, &t) == 0;
    }

    // Signal wakes one waiting thread
    bool signal() {
        return pthread_cond_signal(&m_cond) == 0;
    }

    // Broadcast wakes all waiting threads
    bool broadcast() {
        return pthread_cond_broadcast(&m_cond) == 0;
    }

private:
    pthread_cond_t m_cond; 
};

#endif
