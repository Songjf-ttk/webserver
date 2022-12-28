#include"http_conn.h"

#include<mysql>
#include<fstream>

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
map<string,string> users;


//循环读取客户数据，直到无数据可读或对方关闭连接
//非阻塞ET工作模式下，需要一次性将数据读完
bool http_conn::read_once()
{
    if(m_read_idx>=READ_BUFFER_SIZE)
        return false;
    int bytes_read = 0;
    
    //LT读取数据
    if(0 == m_TRIGMode)
    {
        bytes_read = recv(m_sockfd,m_read_buf+m_read_idx,READ_BUFFER_SIZE-m_read_idx,0);
        m_read_idx += bytes_read;
        if(bytes_read<=0)
            return false;
        return true;
    }
    //ET读数据
    else
    {
        while(true)
        {
            bytes_read = recv(m_sockfd,m_read_buf+m_read_idx,READ_BUFFER_SIZE-m_read_idx,0);
            if(bytes_read == -1)
            {
                if(errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                return false;
            }
            else if(bytes_read == 0)
                return false;
            m_read_idx += bytes_read;
        }
        return true;
    }
}

//将事件重置为EPOLLONESHOT
void modfd(int epollfd,int fd,int ev,int m_TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;
    if(1 == m_TRIGMode)
        event.events = ev | EPOLLET | EPOLLONSHOT | EPOLLRDHUP;
    else
        event.events = ev | EPOLLONSHOT | EPOLLRDHUP;

    epoll_ctl(epollfd,EPOLL_CTL_MOD,fd,&event);   
}


//从状态机，用于分析出一行内容
//返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN
http_conn::LINE_STATUS http_conn::parse_line()
{
    chat temp;
    for(;m_checked_idx<m_read_idx;++m_checked_idx)
    {
        temp = m_read_buf[m_checked_idx];
        if(temp == '\r')
        {
            if((m_checked_idx+1) == m_read_idx)
                return LINE_OPEN;
            else if(m_read_buf[m_checked_idx+1] == '\n')
            {
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
        else if(temp == '\n')
        {
            if(m_checked_idx >1 && m_read_buf[m_checked_idx-1] == '\r')
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

//解析http请求行，获得请求方法，目标url及http版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    m_url = strpbrk(text," \t");
    if(!m_url)
        return BAD_REQUEST;
    *m_url++ = '\0';
    char *method = text;
    if(strcasecmp(method,"GET") == 0)
        m_method = GET;
    else if(strcasecmp(method,"POST") == 0 )
    {
        m_method == POST;
        cgi = 1;
    }
    else
        return BAD_REQUEST;
    //m_url此时跳过了第一个空格或\t字符，但不知道之后是否还有
    //将m_url向后偏移，通过查找，继续跳过空格和\t字符，指向请求资源的第一个字符
    m_url += strspn(m_url," \t");

    m_version = strpbrk(m_url," \t");
    if(!m_version)
        return BAD_REQUEST;
    *m_version++ = '\0';
    m_version += strspn(m_version," \t");
    if(strcasecmp(m_version,"HTTP/1.1") != 0)
        return BAD_REQUEST;
    if(strncasecmp(m_url,"http://",7) == 0)
    {
        m_url += 7;
        m_url = strchr(m_url,'/');
    }

    if(!m_url || m_url[0] != '/')
        return BAD_REQUEST;

    if(strlen(m_url) == 1) 
        strcat(m_url,"judge.html");
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;

}

http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    if(text[0] == '\0')
    {
        if(m_content_length !=0)
        {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        return GET_REQUEST;
    }
    else if(strncasecmp(text,"Connection:",11) == 0)
    {
        text += 11;
        text += strspn(text," \t");
        if(strcasecmp(text,"keep-alive") == 0)
            m_linger = true;
    }
    else if(strncasecmp(text,"Host:",5) == 0)
    {
        text += 5;
        text += strspn(text," \t");
        m_host = text;
    }
    else 
        LOG_INFO("oop!unknow head: %s",text);

    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    if(m_read_idx >= (m_content_length+m_check_idx))
    {
        text[m_content_length] = '\0';
        m_string = text;
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::process_read()
{
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text  = 0;
    while((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK)|| ((line_status = parse_line()) == LINE_OK) )
    {
        text =  get_line();
        m_start_line = m_checked_idx;
        LOG_INFO("%s",text);
        switch(m_check_state)
        {
            case CHECK_STATE_REQUESTLINE:
            {
                ret = parse_request_line(text);
                if(ret == BAD_REQUEST)
                    return BAD_REQUEST;
                break;
            }
            case CHECK_STATE_HEADER:
            {
                ret =  parse_headers();
                if(ret == BAD_REQUEST)
                    return BAD_REQUEST;
                else if(ret == GET_REQUEST)
                    return do_request();
                break;
            }
            case CHECK_STATE_CONTENT:
            {
                ret = parse_content(text);
                if(ret == GET_REQUEST)
                    return do_request();
                line_status = LINE_OPEN;
                break;
            }
            default:
                return INTERNAL_ERROR;
        }
        return NO_REQUEST;
    }
}

void http_conn::process()
{
    HTTP_CODE read_ret = process_read();
    //NO_REQUEST，表示请求不完整，需要继续接收请求数据
    if(read_ret == NO_REQUEST)
    {
        modfd(m_epollfd,m_sockfd,EPOLLIN,m_TRIGMode);
        return;
    }
    bool write_ret = process_write(read_ret);
    if(!write_ret)
        close_conn();
    modfd(m_epollfd,m_sockfd,EPOLLOUT,m_TRIGMode);
}

http_conn::HTTP_CODE http_conn::do_request()
{
    strcpy(m_real_file,doc_root);
    int len = strlen(doc_root);
    const char *p = strrchr(m_url,'/');

    if(cgi == 1 && (*(p+1) == '2') || *(p+1) == '3')
    {
        //根据标志判断是登录检测还是注册检测

        //同步线程登录校验

        //CGI多进程登录校验

    }

    if(*(p+1) == '0')
    {
        char *m_url_real = (char *)malloc(sizeof(char)*200);
        strcpy(m_url_real,"/register.html");
        strncpy(m_real_file+len,m_url_real,strlen(m_url_real));
        free(m_url_real);
    }
    else if(*(p+1) == '1')
    {
        char *m_url_real = (char*)malloc(sizeof(char)*200);
        strcpy(m_url_real,"/log.html");
        strncpy(m_real_file+len,m_url_real,strlen(m_url_real));
        free(m_url_real);
    }
    else if(*(p+1) == '5')
    {
        char *m_url_real = (char*)malloc(sizeof(char)*200);
        strcpy(m_url_real,"/picture.html");
        strncpy(m_real_file+len,m_url_real,strlen(m_url_real));
        free(m_url_real);
    }
    else if(*(p+1) == '6')
    {
        char *m_url_real = (char*)malloc(sizeof(char)*200);
        strcpy(m_url_real,"/video.html");
        strncpy(m_real_file+len,m_url_real,strlen(m_url_real));
        free(m_url_real);
    }
    else 
        strncpy(m_real_file+len,m_url,FILENAME_LEN - len - 1);
        //如果以上均不符合，即不是登录和注册，直接将url与网站目录拼接
        //这里的情况是welcome界面，请求服务器上的一个图片


    if(stat(m_real_file))    
}
