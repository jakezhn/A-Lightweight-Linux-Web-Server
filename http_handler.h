#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H

#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/uio.h>

#include "locker.h"
#include "connection_pool.h"

// Handles HTTP requests and connections
class httpHandler {
public:
    // Constants for buffer sizes and filename length
    static const int FILENAME_LEN = 200;
    static const int READ_BUFFER_SIZE = 2048;
    static const int WRITE_BUFFER_SIZE = 2048;

    // Supported HTTP methods for this handler
    enum METHOD {
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

    // Parsing states for the incoming HTTP request
    enum CHECK_STATE {
        REQUEST_LINE = 0,  // parsing the request line
        HEADER,            // parsing the header
        CONTENT            // parsing the body content
    };

    // Result codes from parsing HTTP requests
    enum HTTP_CODE {
        NO_REQUEST,         // More data needed to form a complete HTTP request
        GET_REQUEST,        // A complete GET request is received
        BAD_REQUEST,        // Malformed request data
        NO_RESOURCE,        // Requested resource not found
        FORBIDDEN_REQUEST,  // Access to the requested resource is forbidden
        FILE_REQUEST,       // Request for a file that exists and can be served
        INTERNAL_ERROR,     // Internal server error
        CLOSED_CONNECTION   // Client has closed the connection
    };

    // Status of parsing individual lines
    enum LINE_STATUS {
        LINE_OK = 0,   // Successfully parsed a complete line
        LINE_BAD,      // Line is malformed or incorrect
        LINE_OPEN      // Line parsing is incomplete
    };

    httpHandler() : m_sockfd(-1), m_url(nullptr), m_version(nullptr), m_host(nullptr),
                    m_content_length(0), m_linger(false), m_file_address(nullptr),
                    m_method(GET), m_check_state(REQUEST_LINE), cgi(0), bytes_to_send(0),
                    bytes_have_send(0), m_writeBuff_idx(0), m_read_idx(0), m_checked_idx(0),
                    m_start_line(0) {}

    ~httpHandler() {
        unmap(); // Unmap any mapped files
        if (m_sockfd != -1) {
            close(m_sockfd); // Close the socket if it's open
        }
    }

    // Initialize handler for a new connection
    void init(int sockfd, const sockaddr_in &addr);
    // Close the connection and clean up
    void closeConnection(bool real_close = true);
    // Main processing loop
    void process();
    // Read incoming data into the buffer
    bool readBuff();
    // Write data from the buffer to the client
    bool writeBuff();
    // Get the address of the connected socket
    sockaddr_in *get_address() { return &m_address; }
    // Initialize MySQL database connections
    void initMysql(connection_pool *connPool);

private:
    // Common initialization routine
    void init();
    // Process read data
    HTTP_CODE processRead();
    // Write response data to client
    bool processWrite(HTTP_CODE ret);
    // Parse an HTTP request line
    HTTP_CODE parseRequest(char *text);
    // Parse an HTTP header
    HTTP_CODE parseHeader(char *text);
    // Parse HTTP body data
    HTTP_CODE parseData(char *text);
    // Handle a complete HTTP request
    HTTP_CODE processRequest();
    // Get a pointer to the current line in the read buffer
    char *get_line() { return m_read_buf + m_start_line; }
    // Parse a line from the buffer
    LINE_STATUS parseLine();
    // Unmap the mapped file
    void unmap();
    // Add formatted response to the buffer
    bool add_response(const char *format, ...);
    // Add content to the HTTP response
    bool add_content(const char *content);
    // Add the status line to the HTTP response
    bool add_status_line(int status, const char *title);
    // Add headers to the HTTP response
    bool add_headers(int content_length);
    // Add content type header
    bool add_content_type();
    // Add content length header
    bool add_content_length(int content_length);
    // Add connection header (keep-alive or close)
    bool add_linger();
    // Add a blank line to signal the end of the headers
    bool add_blank_line();

public:
    // Static variables for epoll and user count
    static int m_epollfd;
    static int m_user_count;
    MYSQL *mysql;

private:
    // Connection details
    int m_sockfd;
    sockaddr_in m_address;
    char m_read_buf[READ_BUFFER_SIZE];
    char m_writeBuff_buf[WRITE_BUFFER_SIZE];
    int m_read_idx;
    int m_checked_idx;
    int m_start_line;
    int m_writeBuff_idx;
    CHECK_STATE m_check_state;
    METHOD m_method;
    char m_real_file[FILENAME_LEN];
    char *m_url;
    char *m_version;
    char *m_host;
    int m_content_length;
    bool m_linger;
    char *m_file_address;
    struct stat m_file_stat;
    struct iovec m_iv[2];
    int m_iv_count;
    int cgi;
    char *m_string;
    int bytes_to_send;
    int bytes_have_send;
};

#endif
