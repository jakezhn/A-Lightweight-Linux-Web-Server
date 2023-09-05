#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/uio.h>

#include "locker.h"
#include "connection_pool.h"

class httpHandler
{
public:
    // Size of file name to read
    static const int FILENAME_LEN = 200;
    // Size of read buffer
    static const int READ_BUFFER_SIZE = 2048;
    // Size of writeBuff buffer
    static const int writeBuff_BUFFER_SIZE = 2048;

    // Http requests, only GET and POST are put into use in this lab
    enum METHOD
    {
        GET = 0,
        POST,
        HEAD,
        PUT,
        DELETE,
        TRACE,
        OPTIONS,
        CONNECT,
        PATH
    };

    // Flags that indicates which part of the http message is to be parse
    enum CHECK_STATE
    {
        // Read http request line
        CHECK_STATE_REQUESTLINE = 0,
        // Read http header
        CHECK_STATE_HEADER,
        // Read http data
        CHECK_STATE_CONTENT
    };

    // Flags that indicates the parsing results of http request line, header, data
    enum HTTP_CODE
    {
        // Request is incomplete, continue listening
        NO_REQUEST,
        // Request is valid, process request
        GET_REQUEST,
        // Wrong request, send http error code
        BAD_REQUEST,
        // Resource does not exist, send http error code
        NO_RESOURCE,
        // No permission to requested resource, send http error code
        FORBIDDEN_REQUEST,
        // Requested file is accessable, process request
        FILE_REQUEST,
        // Server internal error
        INTERNAL_ERROR,
        // Connection closed, prepare to realease resource
        CLOSED_CONNECTION
    };

    // Flags of line parsing result
    enum LINE_STATUS
    {
        // Finished parsing a whole line
        LINE_OK = 0,
        // Line is incomplete / has syntax error
        LINE_BAD,
        // Parsing is not finished yet
        LINE_OPEN
    };

public:
    httpHandler() {}
    ~httpHandler() {}

public:
    // Init for sockets
    void init(int sockfd, const sockaddr_in &addr);
    // Close http connections
    void closeConnection(bool real_close = true);
    void process();
    // Receive message to read buffer
    bool readBuff();
    // Send message from write buffer to client
    bool writeBuff();
    sockaddr_in *get_address()
    {
        return &m_address;
    }
    // Init for Mysql table
    void initMysql(connection_pool *connPool);

private:
    void init();
    // Read from receive buffer, parsing request message
    HTTP_CODE processRead();
    // Write response to send buffer
    bool processWrite(HTTP_CODE ret);
    // Parsing http request, header, data
    HTTP_CODE parseRequest(char *text);
    HTTP_CODE parseHeader(char *text);
    HTTP_CODE parseData(char *text);
    // Process request
    HTTP_CODE processRequest();
    // Move pointer to unparsed content
    char *get_line() { return m_read_buf + m_start_line; };
    LINE_STATUS parseLine();
    // Unmap source file and memory
    void unmap();
    // Generate response, called by processRequest()
    bool add_response(const char *format, ...);
    bool add_content(const char *content);
    bool add_status_line(int status, const char *title);
    bool add_headers(int content_length);
    bool add_content_type();
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_blank_line();

public:
    // Epoll event table
    static int m_epollfd;
    // User counter
    static int m_user_count;
    MYSQL *mysql;

private:
    // Socket
    int m_sockfd;
    sockaddr_in m_address;
    // Read buffer, stores requests
    char m_read_buf[READ_BUFFER_SIZE];
    // Index of last bit in read buffer
    int m_read_idx;
    // Index of bit that is currently reading
    int m_checked_idx;
    // Index of the last bit that has already been read
    int m_start_line;
    // Write buffer, stores responses
    char m_writeBuff_buf[writeBuff_BUFFER_SIZE];
    // Index of last bit in write buffer
    int m_writeBuff_idx;
    // Flags that indicates which part of the http message is to be parse
    CHECK_STATE m_check_state;
    // Request type
    METHOD m_method;
    // Http message content
    char m_real_file[FILENAME_LEN];
    char *m_url;
    char *m_version;
    char *m_host;
    int m_content_length;
    bool m_linger;

    // File address in server
    char *m_file_address;
    struct stat m_file_stat;
    // io vector for readv and writev
    struct iovec m_iv[2];
    int m_iv_count;
    // Indicates POST request from clients
    int cgi;        
    // Store http header
    char *m_string; 
    // Bytes to be sent
    int bytes_to_send;
    // Bytes have been sent
    int bytes_have_send;
};
#endif
