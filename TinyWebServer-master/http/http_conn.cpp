#include "http_conn.h"

#include <mysql/mysql.h>
#include <fstream>

//定义http响应的一些状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

locker m_lock;
map<string, string> users;

void http_conn::initmysql_result(connection_pool *connPool)
{
    //先从连接池中取一个连接
    MYSQL *mysql = NULL;
    connectionRAII mysqlcon(&mysql, connPool);

    //在user表中检索username，passwd数据，浏览器端输入
    if (mysql_query(mysql, "SELECT username,passwd FROM user"))
    {  
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    //从表中检索完整的结果集
    MYSQL_RES *result = mysql_store_result(mysql);

    //返回结果集中的列数
    int num_fields = mysql_num_fields(result);

    //返回所有字段结构的数组
    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    //从结果集中获取下一行，将对应的用户名和密码，存入map中
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;
    }
}

//对文件描述符设置非阻塞
int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

//将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
void addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{   //这里不太懂一次性触发
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    else
        event.events = EPOLLIN | EPOLLRDHUP;

    if (one_shot)
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

//从内核时间表删除描述符
void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

//将事件重置为EPOLLONESHOT
void modfd(int epollfd, int fd, int ev, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    else
        event.events = ev | EPOLLONESHOT | EPOLLRDHUP;

    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}
 //这个初始值，后续会自动变化，静态变量。共享。
int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

//关闭连接，关闭一个连接，客户总量减一
void http_conn::close_conn(bool real_close)
{
    if (real_close && (m_sockfd != -1))
    {
        printf("close %d\n", m_sockfd);
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

//初始化连接,外部调用初始化套接字地址
void http_conn::init(int sockfd, const sockaddr_in &addr, char *root, int TRIGMode,
                     int close_log, string user, string passwd, string sqlname)
{
    m_sockfd = sockfd;
    m_address = addr;

    addfd(m_epollfd, sockfd, true, m_TRIGMode);
    m_user_count++;

    //当浏览器出现连接重置时，可能是网站根目录出错或http响应格式出错或者访问的文件中内容完全为空
    doc_root = root;
    m_TRIGMode = TRIGMode;
    m_close_log = close_log;

    strcpy(sql_user, user.c_str());
    strcpy(sql_passwd, passwd.c_str());
    strcpy(sql_name, sqlname.c_str());

    init();
}

//初始化新接受的连接
//check_state默认为分析请求行状态
void http_conn::init()
{
    mysql = NULL;
    bytes_to_send = 0;
    bytes_have_send = 0;
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
    m_write_idx = 0;
    cgi = 0;
    m_state = 0;
    timer_flag = 0;
    improv = 0;

    memset(m_read_buf, '\0', READ_BUFFER_SIZE);  //可读大小清空
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);  //真实文件路径清空，就是之前的那个工作目录workspace
}

//从状态机，用于分析出一行内容
//返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    for (; m_checked_idx < m_read_idx; ++m_checked_idx)
    {
        temp = m_read_buf[m_checked_idx];
        if (temp == '\r')
        {
            if ((m_checked_idx + 1) == m_read_idx)
                return LINE_OPEN;
            else if (m_read_buf[m_checked_idx + 1] == '\n')
            {
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
        else if (temp == '\n')
        {
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

//循环读取客户数据，直到无数据可读或对方关闭连接
//非阻塞ET工作模式下，需要一次性将数据读完
bool http_conn::read_once()
{
    if (m_read_idx >= READ_BUFFER_SIZE) //要读的大于缓冲区？
    {
        return false;
    }
    int bytes_read = 0;

    //LT读取数据
    if (0 == m_TRIGMode)
    {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        m_read_idx += bytes_read;

        if (bytes_read <= 0)
        {
            return false;
        }

        return true;
    }
    //ET读数据
    else
    {
        while (true)
        {    //读到出错或者读完
            bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
            if (bytes_read == -1)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)   //没有数据可读，并且没错误。那还读个鸡毛，退出/
                    break;
                return false;       //不是，出错了。
            }
            else if (bytes_read == 0)  //读取到了，对端关闭。
            {
                return false;
            }
            m_read_idx += bytes_read;  //一切正常。读了这么多。
        }
        return true;
    }
}

//解析http请求行，获得请求方法，目标url及http版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    m_url = strpbrk(text, " \t");
    if (!m_url)
    {
        return BAD_REQUEST;
    }
    *m_url++ = '\0';
    char *method = text;
    if (strcasecmp(method, "GET") == 0)
        m_method = GET;
    else if (strcasecmp(method, "POST") == 0)
    {
        m_method = POST;
        cgi = 1;
    }
    else
        return BAD_REQUEST;
    m_url += strspn(m_url, " \t");
    m_version = strpbrk(m_url, " \t");
    if (!m_version)
        return BAD_REQUEST;
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");
    if (strcasecmp(m_version, "HTTP/1.1") != 0)
        return BAD_REQUEST;
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
        return BAD_REQUEST;
    //当url为/时，显示判断界面
    if (strlen(m_url) == 1)
        strcat(m_url, "judge.html");
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}

//解析http请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    if (text[0] == '\0')  //是空行 还是请求头？ 空行
    {
        if (m_content_length != 0)   //是post还是get？ 消息体长度
        {
            m_check_state = CHECK_STATE_CONTENT;  //post 需要消息体处理。
            return NO_REQUEST;
        }
        return GET_REQUEST;
    }
    else if (strncasecmp(text, "Connection:", 11) == 0) //是请求头
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
    }
    return NO_REQUEST;
}

//判断http请求是否被完整读入
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    if (m_read_idx >= (m_content_length + m_checked_idx))
    {
        text[m_content_length] = '\0';
        //POST请求中最后为输入的用户名和密码
        m_string = text;
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::process_read()
{
    LINE_STATUS line_status = LINE_OK;// 初始化行状态为LINE_OK
    HTTP_CODE ret = NO_REQUEST;// 初始化HTTP请求处理结果为NO_REQUEST
    char *text = 0;// 初始化文本指针为0

    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parse_line()) == LINE_OK))
    {
        text = get_line();// 获取一行文本 缓冲区起始地址+开始位置
        m_start_line = m_checked_idx;// 更新起始行位置为当前检查位置
        LOG_INFO("%s", text);// 记录日志输出当前文本内容
        switch (m_check_state)
        {
        case CHECK_STATE_REQUESTLINE: // 检查状态为解析请求行
        {
            ret = parse_request_line(text);//解析请求行
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;// 若解析结果为BAD_REQUEST，则返回BAD_REQUEST
            break;
        }
        case CHECK_STATE_HEADER:// 检查状态为解析头部
        {
            ret = parse_headers(text);// 解析头部
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;// 若解析结果为BAD_REQUEST，则返回BAD_REQUEST
            else if (ret == GET_REQUEST)
            {
                return do_request();// 若解析结果为GET_REQUEST，则执行处理请求操作
            }
            break;
        }
        case CHECK_STATE_CONTENT:// 检查状态为解析内容
        {
            ret = parse_content(text);
            if (ret == GET_REQUEST)// 若解析结果为GET_REQUEST，则执行处理请求操作
                return do_request();
            line_status = LINE_OPEN;
            break;
        }
        default:
            return INTERNAL_ERROR;// 默认情况下返回INTERNAL_ERROR
        }
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::do_request() //读取完了会处理。 
{
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    //printf("m_url:%s\n", m_url);
    const char *p = strrchr(m_url, '/');

    //处理cgi
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3'))
    {

        //根据标志判断是登录检测还是注册检测
        char flag = m_url[1];

        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/");
        strcat(m_url_real, m_url + 2);
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1);
        free(m_url_real);

        //将用户名和密码提取出来
        //user=123&passwd=123
        char name[100], password[100];
        int i;
        for (i = 5; m_string[i] != '&'; ++i)
            name[i - 5] = m_string[i];
        name[i - 5] = '\0';

        int j = 0;
        for (i = i + 10; m_string[i] != '\0'; ++i, ++j)
            password[j] = m_string[i];
        password[j] = '\0';

        if (*(p + 1) == '3')
        {
            //如果是注册，先检测数据库中是否有重名的
            //没有重名的，进行增加数据
            char *sql_insert = (char *)malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");

            if (users.find(name) == users.end())   //没找到，注册
            {
                m_lock.lock();
                int res = mysql_query(mysql, sql_insert);
                users.insert(pair<string, string>(name, password));
                m_lock.unlock();

                if (!res)
                    strcpy(m_url, "/log.html");   //注册成功
                else
                    strcpy(m_url, "/registerError.html");  //注册失败
            }
            else
                strcpy(m_url, "/registerError.html");
        }
        //如果是登录，直接判断
        //若浏览器端输入的用户名和密码在表中可以查找到，返回1，否则返回0
        else if (*(p + 1) == '2') //登陆
        {
            if (users.find(name) != users.end() && users[name] == password)
                strcpy(m_url, "/welcome.html");
            else
                strcpy(m_url, "/logError.html");
        }
    }

    if (*(p + 1) == '0')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '1')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '5')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '6')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '7')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/fans.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);

    if (stat(m_real_file, &m_file_stat) < 0)
        return NO_RESOURCE;

    if (!(m_file_stat.st_mode & S_IROTH))
        return FORBIDDEN_REQUEST;

    if (S_ISDIR(m_file_stat.st_mode))
        return BAD_REQUEST;

    int fd = open(m_real_file, O_RDONLY);
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    return FILE_REQUEST;
}
void http_conn::unmap()
{
    if (m_file_address)
    {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}
bool http_conn::write()    // proactor这是主线程干，工作线程不干。 //这里是要把信息给发过去
{
    int temp = 0; 

    if (bytes_to_send == 0)  //如果待发送的字节数为0，说明响应已经发送完毕
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode); // 修改文件描述符的监听事件为EPOLLIN，准备接收新的请求
        init(); // 重置HTTP连接对象的状态
        return true;
    }

    while (1)  //循环发送  在这里会发送完
    {
        temp = writev(m_sockfd, m_iv, m_iv_count);  // 使用writev函数将响应内容写入套接字  ，这不是m_iv吗，就是他。

        if (temp < 0)  // 写入出错
        {
            if (errno == EAGAIN)  // 资源暂时不可写  就是writev还没回来把。
            {
                modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);  // 修改文件描述符的监听事件为EPOLLOUT，等待下次可写事件触发
                return true;
            }
            unmap();  // 取消映射内存
            return false;
        }

        bytes_have_send += temp;  // 更新已发送字节数
        bytes_to_send -= temp;   // 更新待发送字节数 
        if (bytes_have_send >= m_iv[0].iov_len)  // 如果已发送字节数大于等于第一个iovec结构体的长度 
        {
            m_iv[0].iov_len = 0;  // 将第一个iovec结构体的长度设置为0，表示已发送完 
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);  // 更新第二个iovec结构体的起始位置，继续发送剩余数据
            m_iv[1].iov_len = bytes_to_send;// 更新第二个iovec结构体的长度
        }
        else
        {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;  // 更新第一个iovec结构体的起始位置，继续发送剩余数据
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;//  // 更新第一个iovec结构体的长度
        }

        if (bytes_to_send <= 0) // 如果待发送字节数小于等于0，说明响应已经发送完毕
        { 
            unmap();  // 取消映射内存
            modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode); // 修改文件描述符的监听事件为EPOLLIN，准备接收新的请求

            if (m_linger)  // 如果需要保持连接   这个是请求体会传过来的，在http请求中拆解部分应该会保留他。
            {
                init(); // 重置HTTP连接对象的状态 ，写了0个，总共0个，没处理。。。等等。。read不初始化是因为他没有写这些变量。
                return true;
            }
            else
            {
                return false;    //发送完之后来看看还要不要连接，不要连接应该会返回一个false来端开？
            }
        }
    }
}  
bool http_conn::add_response(const char *format, ...)  //这里是关于怎么写响应体的。
{
    if (m_write_idx >= WRITE_BUFFER_SIZE)
        return false;
    va_list arg_list;
    va_start(arg_list, format);
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))
    {
        va_end(arg_list);
        return false;
    }
    m_write_idx += len;
    va_end(arg_list);

    LOG_INFO("request:%s", m_write_buf);

    return true;
}
bool http_conn::add_status_line(int status, const char *title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}
bool http_conn::add_headers(int content_len)
{
    return add_content_length(content_len) && add_linger() &&
           add_blank_line();
}
bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length:%d\r\n", content_len);
}
bool http_conn::add_content_type()
{
    return add_response("Content-Type:%s\r\n", "text/html");
}
bool http_conn::add_linger()
{
    return add_response("Connection:%s\r\n", (m_linger == true) ? "keep-alive" : "close");
}
bool http_conn::add_blank_line()
{
    return add_response("%s", "\r\n");
}
bool http_conn::add_content(const char *content)
{
    return add_response("%s", content);
}
bool http_conn::process_write(HTTP_CODE ret)   //ret要响应请求，但是响应什么请求呢？看看ret是什么。ret是do——request返回的
{
    switch (ret)
    {
    case INTERNAL_ERROR:                        //服务器错误 。
    {
        add_status_line(500, error_500_title);  //状态码设置为 500，状态消息设置为 error_500_title。
        add_headers(strlen(error_500_form));   //添加响应头部，内容长度设置为 error_500_form 的长度
        if (!add_content(error_500_form))     //添加响应体，响应体内容为 error_500_form
            return false;                   //出错返回。
        break;
    }
    case BAD_REQUEST:                        //所找的内容不存在，
    {
        add_status_line(404, error_404_title);  //添加状态行，状态码设置为 404，状态消息设置为 error_404_title。
        add_headers(strlen(error_404_form));   //添加响应头部，内容长度设置为 error_404_form 的长度。
        if (!add_content(error_404_form))     //添加响应体，响应体内容为 error_404_form
            return false;            //出错返回。
        break;
    }
    case FORBIDDEN_REQUEST:
    /*

    客户端没有提供正确的身份验证信息。
    客户端提供的身份验证信息不足以获得对请求资源的访问权限。
    服务器配置了访问控制列表（ACL）或权限设置，导致客户端无权访问某些资源。

    */
    {
        add_status_line(403, error_403_title);
        add_headers(strlen(error_403_form));
        if (!add_content(error_403_form))
            return false;
        break;
    }
    case FILE_REQUEST:   //读取文件的意思
    {
        add_status_line(200, ok_200_title);  //m——file——stat 是基于stat的一个文件结构体。
        if (m_file_stat.st_size != 0)  //如果要读取的文件大小不为0，那么会添加响应头部，并设置相应的数据信息。
        {                                 //同时设置m_iv结构体数组和m_iv_count用于准备发送文件内容。
            add_headers(m_file_stat.st_size);    //m_iv是两个iovec的结构体。 iovec主要存放的是一个地址开头，一个地址长度。
            m_iv[0].iov_base = m_write_buf;
            m_iv[0].iov_len = m_write_idx;    // 从缓冲区write开始到idex的距离。
            m_iv[1].iov_base = m_file_address;
            m_iv[1].iov_len = m_file_stat.st_size;  //这个是文件的地址到文件的距离。 
            m_iv_count = 2;
            bytes_to_send = m_write_idx + m_file_stat.st_size;  //发送的字节数？一个是缓冲区的一个是文件的。
            return true;
        }
        else
        {
            const char *ok_string = "<html><body></body></html>";  //为0 给个空报文。
            add_headers(strlen(ok_string)); 
            if (!add_content(ok_string))
                return false;
        }
    }
    default:    
        return false;                    //不是这几种，出错。
    }
    m_iv[0].iov_base = m_write_buf;   //没有出错也没有成功，就是不是以上几种，我也写个文件体，说两句，就把write缓冲区的发过去。
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}
void http_conn::process()
{
    HTTP_CODE read_ret = process_read();  // 读一下内容，但是内容在哪里不知道。
    if (read_ret == NO_REQUEST)      //当前没有完整的 HTTP 请求可读取
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);  //当前的注册监听，以便继续处理。
        return;                                        //如果有问题到这里就结束了。
    }
    bool write_ret = process_write(read_ret);    //没问题，开始回应。
    if (!write_ret)         //如果没写成功，关闭。
    {
        close_conn();
    }
    modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);  // 修改fd状态为EPOLLOUT 等待写？
}
