#ifndef LOG_H
#define LOG_H

#include <stdio.h>
#include <iostream>
#include <string>
#include <stdarg.h>
#include <pthread.h>

#include "locker.h"

using namespace std;

class Log
{
public:

    static Log *get_instance()
    {
        static Log instance;
        return &instance;
    }
    // Initilization for log instnce
    // split_lines is the maximum line number of the log file
    bool init(const char *file_name, int log_buf_size = 8192, int split_lines = 5000000);
    // Generate log with standard format
    void write_log(int level, const char *format, ...);
    // Flush write buffer
    void flush(void);

private:
    Log();
    virtual ~Log();

private:
    // Log file directory
    char dir_name[128];
    // Log file name
    char log_name[128];
    // Maximum line number of log file
    int m_split_lines;
    // Write buffer size
    int m_log_buf_size;
    // Line number counter
    long long m_count;
    // Date
    int m_today;
    // Pointer to log file
    FILE *m_fp;
    char *m_buf;
    locker m_mutex;
};


#define LOG_DEBUG(format, ...) Log::get_instance()->write_log(0, format, ##__VA_ARGS__)
#define LOG_INFO(format, ...) Log::get_instance()->write_log(1, format, ##__VA_ARGS__)
#define LOG_WARN(format, ...) Log::get_instance()->write_log(2, format, ##__VA_ARGS__)
#define LOG_ERROR(format, ...) Log::get_instance()->write_log(3, format, ##__VA_ARGS__)

#endif
