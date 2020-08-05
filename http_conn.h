#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#include <map>

#include "sql_connection_pool.h"
#include "log.h"
#include "lst_timer.h"
#include "locker.h"

class http_conn {
   public:
    /* 文件名的最大长度 */
    static const int FILENAME_LEN = 200;
    /* 读缓冲区的大小 */
    static const int READ_BUFFER_SIZE = 2048;
    /* 写缓冲区的大小 */
    static const int WRITE_BUFFER_SIZE = 1024;
    /* 确保一个TCP连接拥有足够的空闲缓冲区来处理拥塞 */

    /* HTTP请求方法 */
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

    /* 解析客户请求时，   主状态机所处的状态 
    CHECK_STATE_REQUESTLINE 当前正在分析请求行
    CHECK_STATE_HEADER      当前正在分析请求头
    CHECK_STATE_CONTENT     当前正在分析请求实体
    */
    enum CHECK_STATE {
        CHECK_STATE_REQUESTLINE = 0,
        CHECK_STATE_HEADER,
        CHECK_STATE_CONTENT
    };

    /* 服务器处理HTTP请求的可能结果
    NO_REQUEST          表示请求不完整，需要继续读取客户数据
    GET_REQUEST         表示获得了一个完整的客户请求
    BAD_REQUEST         表示客户请求有语法错误     
    NO_RESOURCE         表示没有所需资源
    FORBIDDEN_REQUEST   表示客户对资源没有足够的访问权限
    FILE_REQUEST        表示获得了文件资源
    INTERNAL_ERROR      表示服务器内部错误
    CLOSED_CONNECTION   表示客户端已经断开连接
     */
    enum HTTP_CODE {
        NO_REQUEST,
        GET_REQUEST,
        BAD_REQUEST,
        NO_RESOURCE,
        FORBIDDEN_REQUEST,
        FILE_REQUEST,
        INTERNAL_ERROR,
        CLOSED_CONNECTION
    };

    /* 行的读取状态     从状态机的状态
    LINE_OK     读取到一个完整的行
    LINE_BAD    行出错
    LINE_OPEN   读取到的行数据不完整
     */
    enum LINE_STATUS { LINE_OK = 0, LINE_BAD, LINE_OPEN };

    public:
    http_conn() {}
    ~http_conn() {}

    public:
    /* 初始化新接受的连接 */
    void init(int sockfd, const sockaddr_in &addr, char *, int, int,
              string user, string passwd, string sqlname);
    /* 关闭连接 */
    void close_conn(bool real_close = true);
    /* 处理客户请求 */
    void process();
    /* 读取客户端发来的全部数据 */
    bool read_once();
    /* 响应报文写入函数 */
    bool write();
    /* 获取客户端socket地址 */
    sockaddr_in *get_address() { return &m_address; }
    /* 初始化数据库读取列表 */
    void initmysql_result(connection_pool *connPool);
    int timer_flag;
    int improv;

   private:
    /* 初始化连接 */
    void init();
    /* 解析HTTP请求 */
    HTTP_CODE process_read();
    /* 填充HTTP应答 */
    bool process_write(HTTP_CODE ret);

    /* 下面部分被process_read()调用  分析HTTP请求 */
    HTTP_CODE parse_request_line(char *text);
    HTTP_CODE parse_headers(char *text);
    HTTP_CODE parse_content(char *text);
    HTTP_CODE do_request();

    /* 用于指针向后偏移，指向未处理的字符 */
    char *get_line() { return m_read_buf + m_start_line; };
    
    /* 从状态机读取一行，分析是请求报文的哪部分 */
    LINE_STATUS parse_line();

    /* 下面这组函数被process_write调用以填充HTTP应答 */
    void unmap();
    bool add_response(const char *format, ...);
    bool add_content(const char *content);
    bool add_status_line(int status, const char *title);
    bool add_headers(int content_length);
    bool add_content_type();
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_blank_line();

   public:
    /* 所有的socket上的事件都被注册到同一个epoll内核事件表中，故将epoll文件描述符设置为静态的 */
    static int m_epollfd;
    /* 统计用户的数量 */
    static int m_user_count;
    MYSQL *mysql;
    int m_state;    //读为0, 写为1

   private:
    /* 该HTTP连接的socket和对方的socket地址 */
    int m_sockfd;
    sockaddr_in m_address;

    /* 读缓冲区，读取的请求报文数据 */
    char m_read_buf[READ_BUFFER_SIZE];
    /* 标识读缓冲区已经读入的客户数据的最后一个字节的下一个位置 */
    int m_read_idx;
    /* 当前正在分析的字符在读缓冲区中的位置 */
    int m_checked_idx;
    /* 当前正在解析的行的起始位置 */
    int m_start_line;
    /* 写缓冲区，存储发出的相应报文 */
    char m_write_buf[WRITE_BUFFER_SIZE];
    /* 写缓冲区中待发送的字节数 */
    int m_write_idx;

    /* 主状态机的当前的状态 */
    CHECK_STATE m_check_state;
    /* 请求方法 */
    METHOD m_method;

    /* 客户请求的目标文件的完整路径 */
    char m_real_file[FILENAME_LEN];
    /* 客户请求的目标文件的文件名 */
    char *m_url;
    /* HTTP协议版本号 */
    char *m_version;
    /* 主机名 */
    char *m_host;
    /* HTTP请求的消息体的长度 */
    int m_content_length;
    /* HTTP请求是否要求保持连接 */
    bool m_linger;

    /* 客户请求的目标文件被mmap到内存的起始位置 */
    char *m_file_address;
    /* 目标文件的状态。判断文件是否存在，是否为目录，是否可读，并获取文件大小等信息 */
    struct stat m_file_stat;

    /* 使用writev来执行写操作。 */
    struct iovec m_iv[2];
    int m_iv_count;

    int cgi;         //是否启用的POST
    char *m_string;  //存储请求头数据
    int bytes_to_send;
    int bytes_have_send;
    /* 网站的根目录 */
    char *doc_root;

    map<string, string> m_users;
    //epoll触发模式   0----LT   1-----ET
    int m_TRIGMode;     
    int m_close_log;

    char sql_user[100];
    char sql_passwd[100];
    char sql_name[100];
};

#endif