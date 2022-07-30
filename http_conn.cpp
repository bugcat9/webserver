#include "http_conn.h"

int http_conn::m_epollfd = -1;
int http_conn::m_user_count = 0;

// 设置文件描述符非阻塞
void setnonblocking(int fd)
{
    int old_flag = fcntl(fd, F_GETFL);
    int new_flag = old_flag | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_flag);
}

// 添加文件描述符到epoll中
void addfd(int epollfd, int fd, bool one_shot)
{
    epoll_event event;
    event.data.fd = fd;
    // event.events = EPOLLIN | EPOLLRDHUP;
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;

    if (one_shot)
    {
        event.events |= EPOLLONESHOT;
    }
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    // 设置文件描述符非阻塞
    setnonblocking(fd);
}

// 从epoll中删除文件描述符
extern void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

// 修改文件描述符
void modfd(int epollfd, int fd, int ev)
{
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}
// 初始化连接
void http_conn::init(int sockfd, const sockaddr_in &addr)
{
    m_sockfd = sockfd;
    m_address = addr;

    //端口复用
    int reuse = 1;
    setsockopt(m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // 添加到epoll对象中
    addfd(m_epollfd, m_sockfd, true);
    m_user_count++;

    init();
}

void http_conn::init()
{
    m_check_state = CHECK_STATE_REQUESTLINE; // 初始化状态为解析请求首行
    m_cheched_index = 0;
    m_start_line = 0;
    m_read_idx = 0;
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_linger = false;
    bzero(m_read_buf, READ_BUFFER_SIZE);
}

// 关闭连接
void http_conn::close_conn()
{
    if (m_sockfd != -1)
    {
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--; // 关闭一个连接，客户总数量-1
    }
}
// 非阻塞的读
// 循环读取客户数据，直到无数据可读，或者对方关闭连接
bool http_conn::read()
{
    // printf("一次性读完数据\n");

    if (m_read_idx >= READ_BUFFER_SIZE)
    {
        return false;
    }

    // 读取到的字节
    int bytes_read = 0;
    while (true)
    {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);

        if (bytes_read == -1)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                // 没用数据
                break;
            }
            return false;
        }
        else if (bytes_read == 0)
        {
            // 对方关闭连接
            return false;
        }
        m_read_idx += bytes_read;
    }
    printf("读取到了数据：%s\n", m_read_buf);
    return true;
}

// 主状态机，解析请求
http_conn::HTTP_CODE http_conn::process_read()
{
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;

    while (((m_check_state == CHECK_STATE_CONTENT) && (line_status == LINE_OK)) ||
           (line_status = parse_line()) == LINE_OK)
    { // 解析到了一行完整的数据，或者解析到了请求体，也是完整的数据
        // 获取一行数据
        text = get_line();
        m_start_line = m_cheched_index;
        printf("got 1 http line: %s\n", text);
        switch (m_check_state)
        {
        case CHECK_STATE_REQUESTLINE:
        {
            ret = parse_request_line(text);
            if (ret == BAD_REQUEST)
            {
                return BAD_REQUEST;
            }
            break;
        }

        case CHECK_STATE_HEADER:
        {
            ret = parse_headers(text);
            if (ret == BAD_REQUEST)
            {
                return BAD_REQUEST;
            }
            else if (ret == GET_REQUEST)
            {
                return do_request();
            }
            break;
        }

        case CHECK_STATE_CONTENT:
        {
            ret = parse_content(text);
            if (ret == GET_REQUEST)
            {
                return do_request();
            }
            line_status = LINE_OPEN;
            break;
        }

        default:
        {
            return INTERNAL_ERROR;
        }
        }
        return NO_REQUEST;
    }
}
// 系欸小http请求行，获得请求方法，目标upl，http版本
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    // GET /index.html HTTP/1.1
    m_url = strpbrk(text, " \t");

    // GET\0/index.html HTTP/1.1
    *m_url++ = '\0';
    char *method = text;
    char *method = text;
    if (strcasecmp(method, "GET") == 0)
    {
        m_method = GET;
    }
    else
    {
        return BAD_REQUEST;
    }

    // /index.html HTTP/1.1
    m_version = strpbrk(m_url, " \t");
    if (!m_version)
    {
        return BAD_REQUEST;
    }
    *m_version++ = '\0';
    if (strcasecmp(m_version, "HTTP/1.1") != 0)
    {
        return BAD_REQUEST;
    }
    // http://192.168.1.1:10000/index.html
    if (strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;                 // 192.168.1.1:1000/index.html
        m_url = strchr(m_url, '/'); // /index.html
    }

    if (!m_url || m_url[0] != '/')
    {
        return BAD_REQUEST;
    }
    // 主状态机检查状态变成检查请求头
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    return NO_REQUEST;
}
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    return NO_REQUEST;
}
// 解析一行，判断依据\r\n
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    for (; m_cheched_index < m_read_idx; ++m_cheched_index)
    {
        temp = m_read_buf[m_cheched_index];
        if (temp == '\r')
        {
            if ((m_cheched_index + 1) == m_read_idx)
            {
                return LINE_OPEN;
            }
            else if (m_read_buf[m_cheched_index + 1] == '\n')
            {
                // 将 '\r\n' 变成'\0'
                m_read_buf[m_cheched_index++] = '\0';
                m_read_buf[m_cheched_index++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
        else if (temp == '\n')
        {
            if ((m_cheched_index > 1) && (m_read_buf[m_cheched_index - 1] == '\r'))
            {
                m_read_buf[m_cheched_index - 1] = '\0';
                m_read_buf[m_cheched_index++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
        return LINE_OPEN;
    }
}

http_conn::HTTP_CODE http_conn::do_request()
{
}
// 非阻塞的写
bool http_conn::write()
{
    printf("一次性写完数据\n");
    return true;
}

// 由线程池中的工作线程调用，这是处理http请求的入口函数
void http_conn::process()
{
    // 解析http请求
    HTTP_CODE read_ret = process_read();
    if (read_ret == NO_REQUEST)
    { // 请求不完整
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        return;
    }

    printf("parse request ,create response\n");
    // 生成响应
}