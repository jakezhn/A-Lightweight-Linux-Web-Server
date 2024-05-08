#include <map>
#include <mysql/mysql.h>
#include <fstream>

#include "http_handler.h"
#include "log.h"

// Directory for HTML resources
const char* doc_root = "/home/zhn/Desktop/WebServer/resource";

// Stores user credentials
map<string, string> users;

// Synchronization lock
locker m_lock;

// HTTP status messages
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Invalid request format.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "Access denied.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "Resource not found.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "Server error.\n";

// Sets file descriptor to non-blocking mode
int setNonBlocking(int fd) {
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

// Registers fd in epoll instance
void addFd(int epollfd, int fd, bool one_shot) {
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLRDHUP; // Detect half-closed connections

    if (one_shot) {
        event.events |= EPOLLONESHOT;
    }

    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setNonBlocking(fd);
}

// Removes fd from epoll instance
void removeFd(int epollfd, int fd) {
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

// Updates fd event to one-shot in epoll
void setEventOneshot(int epollfd, int fd, int ev) {
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

// Initialize new connections
void httpHandler::init(int sockfd, const sockaddr_in &addr) {
    m_sockfd = sockfd;
    m_address = addr;
    addFd(m_epollfd, sockfd, true);
    m_user_count++;
    init();
}

// Prepare socket for data handling
void httpHandler::init() {
    mysql = NULL;
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_check_state = CHECK_STATE_REQUESTLINE;  // Initial state for parsing requests
    m_linger = false;  // Connection close flag
    m_method = GET;  // Default HTTP method
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

// Initialize connection to MySQL database
void httpHandler::initMysql(connection_pool* connPool) {
    MYSQL* mysql = NULL;
    connectionRAII mysqlcon(&mysql, connPool);

    if (mysql_query(mysql, "SELECT username, passwd FROM user")) {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    MYSQL_RES* result = mysql_store_result(mysql);
    int num_fields = mysql_num_fields(result);

    while (MYSQL_ROW row = mysql_fetch_row(result)) {
        users[string(row[0])] = string(row[1]);
    }
}

// Parse incoming data
httpHandler::HTTP_CODE httpHandler::processRead() {
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;

    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parseLine()) == LINE_OK)) {
        text = get_line();
        m_start_line = m_checked_idx;
        LOG_INFO("%s", text);
        switch (m_check_state) {
            case CHECK_STATE_REQUESTLINE:
                ret = parseRequest(text);
                if (ret == BAD_REQUEST) return BAD_REQUEST;
                break;
            case CHECK_STATE_HEADER:
                ret = parseHeader(text);
                if (ret == BAD_REQUEST) return BAD_REQUEST;
                else if (ret == GET_REQUEST) return processRequest();
                break;
            case CHECK_STATE_CONTENT:
                ret = parseData(text);
                if (ret == GET_REQUEST) return processRequest();
                line_status = LINE_OPEN;
                break;
            default:
                return INTERNAL_ERROR;
        }
    }
    return NO_REQUEST;
}

// Send data to client
bool httpHandler::processWrite(HTTP_CODE ret) {
    switch (ret) {
        case INTERNAL_ERROR:
            add_status_line(500, error_500_title);
            add_headers(strlen(error_500_form));
            if (!add_content(error_500_form)) return false;
            break;
        case BAD_REQUEST:
            add_status_line(404, error_404_title);
            add_headers(strlen(error_404_form));
            if (!add_content(error_404_form)) return false;
            break;
        case FORBIDDEN_REQUEST:
            add_status_line(403, error_403_title);
            add_headers(strlen(error_403_form));
            if (!add_content(error_403_form)) return false;
            break;
        case FILE_REQUEST:
            add_status_line(200, ok_200_title);
            if (m_file_stat.st_size != 0) {
                add_headers(m_file_stat.st_size);
                m_iv[0].iov_base = m_writeBuff_buf;
                m_iv[0].iov_len = m_writeBuff_idx;
                m_iv[1].iov_base = m_file_address;
                m_iv[1].iov_len = m_file_stat.st_size;
                m_iv_count = 2;
                bytes_to_send = m_writeBuff_idx + m_file_stat.st_size;
                return true;
            } else {
                const char *ok_string = "<html><body></body></html>";
                add_headers(strlen(ok_string));
                if (!add_content(ok_string)) return false;
            }
            break;
        default:
            return false;
    }
    m_iv[0].iov_base = m_writeBuff_buf;
    m_iv[0].iov_len = m_writeBuff_idx;
    m_iv_count = 1;
    bytes_to_send = m_writeBuff_idx;
    return true;
}

// Main processing loop
void httpHandler::process() {
    HTTP_CODE read_ret = processRead();
    if (read_ret == NO_REQUEST) {
        setEventOneshot(m_epollfd, m_sockfd, EPOLLIN);
        return;
    }
    bool writeBuff_ret = processWrite(read_ret);
    if (!writeBuff_ret) {
        closeConnection();
    }
    setEventOneshot(m_epollfd, m_sockfd, EPOLLOUT);
}
