/*
Author: Hongnan Zhang
Class: ECE6122
Last Date Modified: 2022/12/6
Description:
    Defined http handler class. 
    When the client (browser) initiate a new http connection, a new http instance will be create in main thread and store that request in its receive buffer. 
    Then the http instance will be added to a request queue, one of other work threads will process the request which instance carries.
    After a work thread acquires the instance, processRead will be called to read and parse the http message.
    Finally, processRequest will be called to generate response, which is then written to send buffer and sent to client on main thread.
*/

#include <map>
#include <mysql/mysql.h>
#include <fstream>

#include "http_handler.h"
#include "log.h"

// Define html resource directory
const char* doc_root = "/home/zhn/Desktop/WebServer/resource";

// Map to store user names and passwords
map<string, string> users;

// Mutual lock
locker m_lock;

// Define http status codes
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "The server cannot or will not process the request due to something that is perceived to be a client error.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "The client does not have access rights to the content\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The server cannot find the requested resource.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "The server has encountered a situation it does not know how to handle.\n";

// Set file descriptor (called as "fd" below for short) to non-blocking
int setNonBlocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

// Add readable fd to kernel events table
void addFd(int epollfd, int fd, bool one_shot)
{
    epoll_event event;
    event.data.fd = fd;

    // Support half-close socket with EPOLLRDHUP
    event.events = EPOLLIN | EPOLLRDHUP;

    // Set event to oneshot, in order to avoid competition among threads when necessary
    if (one_shot)
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setNonBlocking(fd);
}

// Remove fd from kernel events table
void removeFd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

// Set/reset event to oneshot
void setEventOneshot(int epollfd, int fd, int ev)
{
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLONESHOT | EPOLLRDHUP;

    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

// Current user counter
// "m_" ahead of variable name indicates it is shared among threads
int httpHandler::m_user_count = 0;
// Fd of kernel event table
int httpHandler::m_epollfd = -1;

// Close connection
void httpHandler::closeConnection(bool real_close)
{
    if (real_close && (m_sockfd != -1))
    {
        removeFd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

// Initialization for new sockets
void httpHandler::init(int sockfd, const sockaddr_in &addr)
{
    m_sockfd = sockfd;
    m_address = addr;
    addFd(m_epollfd, sockfd, true);
    m_user_count++;
    init();
}

// Initialization for new connections
void httpHandler::init()
{
    mysql = NULL;
    bytes_to_send = 0;
    bytes_have_send = 0;
    // Set checkline 
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_linger = false;
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_writeBuff_idx = 0;
    cgi = 0;
    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_writeBuff_buf, '\0', writeBuff_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}

// Initialize Mysql
void httpHandler::initMysql(connection_pool* connPool)
{
    MYSQL* mysql = NULL;
    connectionRAII mysqlcon(&mysql, connPool);

    // Search for user name and password that client inputs in browser
    if (mysql_query(mysql, "SELECT username,passwd FROM user"))
    {
        // Generate error log
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    MYSQL_RES* result = mysql_store_result(mysql);

    //Get the result's number of fields in Mysql read table, and all 
    int num_fields = mysql_num_fields(result);
    MYSQL_FIELD* fields = mysql_fetch_fields(result);

    // Store new user name and password to the next row of the result's filed
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;
    }
}

// Parse content of a line
httpHandler::LINE_STATUS httpHandler::parseLine()
{
    char temp;
    // m_checked_idx indicates the byte that is currently reading
    // m_read_idx indicates the last byte of the buffer + 1 (the length of data in buffer)
    for (; m_checked_idx < m_read_idx; ++m_checked_idx)
    {
        temp = m_read_buf[m_checked_idx];
        // If \r is read, next line could be a complete line 
        if (temp == '\r')
        {
            // If the line ends with \r, consider it incomplete
            if ((m_checked_idx + 1) == m_read_idx)
            {
                return LINE_OPEN;
            }
            // If the subsequent byte of \r is \n, consider line is finished
            else if (m_read_buf[m_checked_idx + 1] == '\n')
            {
                // Remove \r and \n
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
        // If \r is read, next line could be a complete line 
        else if (temp == '\n')
        {
            // If the previous byte of \n is \r, consider line is finished
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r')
            {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

// Read from buffer
bool httpHandler::readBuff()
{
    if (m_read_idx >= READ_BUFFER_SIZE)
    {
        return false;
    }
    int bytes_read = 0;
    // Recv from socket, store message in m_read_buf
    bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);

    if (bytes_read <= 0)
    {
        return false;
    }
    // Increase number of bytes that have been read
    m_read_idx += bytes_read;

    return true;
}

// Parse the request line, acquire the method requested (GET or POST), URL, and Http version
httpHandler::HTTP_CODE httpHandler::parseRequest(char *text)
{
    // Request line of the http message indicates request type, URL ro access and http version
    // Find the position of the first" \t"
    m_url = strpbrk(text, " \t");
    if (!m_url)
    {
        return BAD_REQUEST;
    }
    // Set that byte to \0 to segment it from previous data
    *m_url++ = '\0';
    // Check method
    char *method = text;
    if (strcasecmp(method, "GET") == 0)
    {
        m_method = GET;
    }
    else if (strcasecmp(method, "POST") == 0)
    {
        m_method = POST;
        cgi = 1;
    }
    else
    {
        return BAD_REQUEST;
    }
    // Find next " \t" to get http bersion
    m_url += strspn(m_url, " \t");
    m_version = strpbrk(m_url, " \t");
    if (!m_version)
    {
        return BAD_REQUEST;
    }
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");
    if (strcasecmp(m_version, "HTTP/1.1") != 0)
    {
        return BAD_REQUEST;
    }
    // Check if there is "http://" or "https://"
    if (strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;
        m_url = strchr(m_url, '/');
    }

    if (strncasecmp(m_url, "https://", 8) == 0)
    {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }
    if (!m_url || m_url[0] != '/')
    {
        return BAD_REQUEST;
    }
    // Show home page when url == "/"
    if (strlen(m_url) == 1)
    {
        strcat(m_url, "home.html");
    }
    // Change state to CHECK_STATE_HEADER
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}

// Parse http header
httpHandler::HTTP_CODE httpHandler::parseHeader(char *text)
{
    if (text[0] == '\0')
    {
        // Check if it is a POST request (content length > 0)
        if (m_content_length != 0)
        {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        // Other wise it is a GET rerquest
        return GET_REQUEST;
    }
    // Parsing Header: connection, content length, host id
    else if (strncasecmp(text, "Connection:", 11) == 0)
    {
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0)
        {
            m_linger = true;
        }
    }
    else if (strncasecmp(text, "Content-length:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }
    else if (strncasecmp(text, "Host:", 5) == 0)
    {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else
    {
        LOG_INFO("oop!unknow header: %s", text);
        Log::get_instance()->flush();
    }
    return NO_REQUEST;
}

// Parsing the data of a POST request
// And POST request in this Lab is only submitting username and password
httpHandler::HTTP_CODE httpHandler::parseData(char *text)
{
    if (m_read_idx >= (m_content_length + m_checked_idx))
    {
        text[m_content_length] = '\0';
        // User name and password
        m_string = text;
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

// Read from receive buffer
httpHandler::HTTP_CODE httpHandler::processRead()
{
    // Initialize status for line parse and request parse
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;
     
    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parseLine()) == LINE_OK))
    {
        text = get_line();
        // m_start_line stands for the starting position of a line
        // m_checked_idx stands for the next position of the line that has been read last time
        m_start_line = m_checked_idx;
        LOG_INFO("%s", text);
        Log::get_instance()->flush();
        switch (m_check_state)
        {
        case CHECK_STATE_REQUESTLINE:
        {
            // Parsing http request line
            ret = parseRequest(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            break;
        }
        case CHECK_STATE_HEADER:
        {
            // Parsing http header
            ret = parseHeader(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            // If header indicates a GET request, process request
            else if (ret == GET_REQUEST)
            {
                return processRequest();
            }
            break;
        }
        case CHECK_STATE_CONTENT:
        {
            // Parsing http data
            ret = parseData(text);
            // All GET request would be processed at CHECK_STATE_HEADER
            // Only POST requests (submiting username and passowrd at login or register) reach CHECK_STATE_CONTENT
            if (ret == GET_REQUEST)
                return processRequest();
            line_status = LINE_OPEN;
            break;
        }
        default:
            return INTERNAL_ERROR;
        }
    }
    return NO_REQUEST;
}

// Process GET and POST request
httpHandler::HTTP_CODE httpHandler::processRequest()
{
    // Set directory of source file to m_real_file
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    // Find the position of "/" in url
    const char *p = strrchr(m_url, '/');

    // cgi == 1 indicates POST request
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3'))
    {
        // Append url to directory of source file
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/");
        strcat(m_url_real, m_url + 2);
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1);
        free(m_url_real);

        // Get user name and password
        char name[100], password[100];
        int i;
        for (i = 5; m_string[i] != '&'; ++i)
            name[i - 5] = m_string[i];
        name[i - 5] = '\0';

        int j = 0;
        for (i = i + 10; m_string[i] != '\0'; ++i, ++j)
            password[j] = m_string[i];
        password[j] = '\0';

        // 3 after / in url stands for submitting register information
        if (*(p + 1) == '3')
        {
            char *sql_insert = (char *)malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");

            if (users.find(name) == users.end())
            {

                m_lock.lock();
                int res = mysql_query(mysql, sql_insert);
                users.insert(pair<string, string>(name, password));
                m_lock.unlock();
                // Check for repeated user name before registering
                if (!res)
                    strcpy(m_url, "/login.html");
                else
                    strcpy(m_url, "/registerError.html");
            }
            else
                strcpy(m_url, "/registerError.html");
        }
        
        // 2 after / in url stands for submitting login information
        else if (*(p + 1) == '2')
        {
            // Check for user name and password validity
            if (users.find(name) != users.end() && users[name] == password)
                strcpy(m_url, "/picture.html");
            else
                strcpy(m_url, "/loginError.html");
        }
    }
    // 0 after / in url stands for entering register page
    if (*(p + 1) == '0')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    // 1 after / in url stands for entering login page
    else if (*(p + 1) == '1')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/login.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else
        // Append url to directory of source file (home page)
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);
    // Get the status of resource file
    if (stat(m_real_file, &m_file_stat) < 0)
        return NO_RESOURCE;
    // Get the access ability of resource file
    if (!(m_file_stat.st_mode & S_IROTH))
        return FORBIDDEN_REQUEST;
    // Get the type of resource file
    if (S_ISDIR(m_file_stat.st_mode))
        return BAD_REQUEST;
    // Open file in read only mode, map it to memory
    int fd = open(m_real_file, O_RDONLY);
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    return FILE_REQUEST;
}
void httpHandler::unmap()
{
    if (m_file_address)
    {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}

// Send write buffer to client
bool httpHandler::writeBuff()
{
    int temp = 0;

    // If send buffer is 0, reinitialize the event
    if (bytes_to_send == 0)
    {
        setEventOneshot(m_epollfd, m_sockfd, EPOLLIN);
        init();
        return true;
    }

    // Send the status line, header and content tot browser
    while (1)
    {
        // temp is the number of bytes that are sent
        temp = writev(m_sockfd, m_iv, m_iv_count);
        // If error occurs
        if (temp < 0)
        {
            if (errno == EAGAIN)
                // If send buffer is full
            {
                // Reset event oneshot
                setEventOneshot(m_epollfd, m_sockfd, EPOLLOUT);
                return true;
            }
            unmap();
            return false;
        }

        // Update number of bytes that are sent and about to send
        bytes_have_send += temp;
        bytes_to_send -= temp;
        // Send both of the io vectors
        if (bytes_have_send >= m_iv[0].iov_len)
        {
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_writeBuff_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        else
        {
            m_iv[0].iov_base = m_writeBuff_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }
        // Check if all data is sent, if so then unmap the file and reset event to oneshot
        if (bytes_to_send <= 0)
        {
            unmap();
            setEventOneshot(m_epollfd, m_sockfd, EPOLLIN);

            // If the connection status is linger, reinitialize the http instance
            if (m_linger)
            {
                init();
                return true;
            }
            else
            {
                return false;
            }
        }
    }
}

// Write response to send buffer
bool httpHandler::add_response(const char *format, ...)
{
    // Return false if writing data that is larger than buffer size
    if (m_writeBuff_idx >= writeBuff_BUFFER_SIZE)
        return false;
    va_list arg_list;
    va_start(arg_list, format);
    int len = vsnprintf(m_writeBuff_buf + m_writeBuff_idx, writeBuff_BUFFER_SIZE - 1 - m_writeBuff_idx, format, arg_list);
    // Return false if writing data that is larger than the reamaining space of the buffer
    if (len >= (writeBuff_BUFFER_SIZE - 1 - m_writeBuff_idx))
    {
        va_end(arg_list);
        return false;
    }
    m_writeBuff_idx += len;
    va_end(arg_list);
    LOG_INFO("request:%s", m_writeBuff_buf);
    Log::get_instance()->flush();
    return true;
}

// Add response line
bool httpHandler::add_status_line(int status, const char *title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}
// Add headers: content length, connection status and blank line
bool httpHandler::add_headers(int content_len)
{
    add_content_length(content_len);
    add_linger();
    add_blank_line();
}
bool httpHandler::add_content_length(int content_len)
{
    return add_response("Content-Length:%d\r\n", content_len);
}
bool httpHandler::add_content_type()
{
    return add_response("Content-Type:%s\r\n", "text/html");
}
bool httpHandler::add_linger()
{
    return add_response("Connection:%s\r\n", (m_linger == true) ? "keep-alive" : "close");
}
bool httpHandler::add_blank_line()
{
    return add_response("%s", "\r\n");
}
bool httpHandler::add_content(const char *content)
{
    return add_response("%s", content);
}
// Write response to send buffer and send to clients
bool httpHandler::processWrite(HTTP_CODE ret)
{
    switch (ret)
    {
        // Interal error 505
    case INTERNAL_ERROR:
    {
        // Status line
        add_status_line(500, error_500_title);
        // Header of message
        add_headers(strlen(error_500_form));
        if (!add_content(error_500_form))
            return false;
        break;
    }
    // Syntax error 404
    case BAD_REQUEST:
    {
        add_status_line(404, error_404_title);
        add_headers(strlen(error_404_form));
        if (!add_content(error_404_form))
            return false;
        break;
    }
    // No access right 403
    case FORBIDDEN_REQUEST:
    {
        add_status_line(403, error_403_title);
        add_headers(strlen(error_403_form));
        if (!add_content(error_403_form))
            return false;
        break;
    }
    // Request successful 200
    case FILE_REQUEST:
    {
        add_status_line(200, ok_200_title);
        if (m_file_stat.st_size != 0)
        {
            add_headers(m_file_stat.st_size);
            // m_iv[0] points to the write buffer
            m_iv[0].iov_base = m_writeBuff_buf;
            m_iv[0].iov_len = m_writeBuff_idx;
            // m_iv[1] points to the file mapped in memory
            m_iv[1].iov_base = m_file_address;
            m_iv[1].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            // bytes_to_send is message header size + file size
            bytes_to_send = m_writeBuff_idx + m_file_stat.st_size;
            return true;
        }
        else
        {
            // Return enmty html if requested file size is 0
            const char *ok_string = "<html><body></body></html>";
            add_headers(strlen(ok_string));
            if (!add_content(ok_string))
                return false;
        }
    }
    default:
        return false;
    }
    // m_iv[1] is only set when status is FILE_REQUEST
    m_iv[0].iov_base = m_writeBuff_buf;
    m_iv[0].iov_len = m_writeBuff_idx;
    m_iv_count = 1;
    bytes_to_send = m_writeBuff_idx;
    return true;
}

void httpHandler::process()
{
    HTTP_CODE read_ret = processRead();
    
    if (read_ret == NO_REQUEST)
    {
        // Reset event to oneshot and continue listening
        setEventOneshot(m_epollfd, m_sockfd, EPOLLIN);
        return;
    }
    // Write response to send buffer
    bool writeBuff_ret = processWrite(read_ret);
    if (!writeBuff_ret)
    {
        closeConnection();
    }
    setEventOneshot(m_epollfd, m_sockfd, EPOLLOUT);
}
