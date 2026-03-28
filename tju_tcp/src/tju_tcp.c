#include "tju_tcp.h"
#include<sys/time.h>

//添加处：计算时间
long getCurrentTime(){
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return tv.tv_sec * 1000 + tv.tv_usec / 1000;
}
/*
创建 TCP socket 
初始化对应的结构体
设置初始状态为 CLOSED
*/
tju_tcp_t* tju_socket(){
    tju_tcp_t* sock = (tju_tcp_t*)malloc(sizeof(tju_tcp_t));
    sock->state = CLOSED; //初始状态是closed
    
    pthread_mutex_init(&(sock->send_lock), NULL);
    sock->sending_buf = NULL;//发送数据缓冲区还未设置，可能在第二周完成可靠数据传输的时候需要更改
    //
    sock->sending_len = 0;

    pthread_mutex_init(&(sock->recv_lock), NULL);
    sock->received_buf = NULL;
    sock->received_len = 0;
    //接收缓冲区的初始设定应该也是0，需要在tju_recv函数中更改，但是感觉在初始就直接加上也可以，第二周再说
    if(pthread_cond_init(&sock->wait_cond, NULL) != 0){
        perror("ERROR condition variable not set\n");
        exit(-1);
    }//这个判断不道是用来干啥的，需要找一下pthread_cond_init函数的用法，参数含义
//添加处：此处在close中有用
	sock->window.wnd_send = (sender_window_t *)malloc(sizeof(sender_window_t));
	sock->window.wnd_recv = (receiver_window_t *)malloc(sizeof(receiver_window_t));
	sock->window.wnd_recv->expect_seq = 0;
    //添加处：判断超时有用
	sock->window.wnd_send->timeout = 200000;
	sock->window.wnd_send->estmated_rtt = 0;
	sock->window.wnd_send->dev_rtt = 0;



    return sock;
}

/*
绑定监听的地址 包括ip和端口
*/
int tju_bind(tju_tcp_t* sock, tju_sock_addr bind_addr){
//tju_sock_addr和tju_tcp_t的定义在global.h里边，上面是tju_tcp_t的初始化
    /*if(occupied_ports[cal_hash(0,bind_addr.port,0,0)] == 1)
    {//先给server端分配一个没有分配过的bind_addr.port，所以不需要远端ip和远端port
        //cout<<"ERROR:THE PORTS IS OCCUPYIED"<<endl;
        exit(-1);
    }
    else
    {
        occupied_ports[cal_hash(0,bind_addr.port,0,0)] = 1;//如果是空端口，就要给他占上*/
        sock->bind_addr = bind_addr;
        printf(">>>>>>>>>>>>finish bind<<<<<<<<<<<<\n");
        return 0;
    //}
}

/*
被动打开 监听bind的地址和端口
设置socket的状态为LISTEN
注册该socket到内核的监听socket哈希表
*/
int tju_listen(tju_tcp_t* sock){
    
    sock->state = LISTEN;
    int hashval = cal_hash(sock->bind_addr.ip, sock->bind_addr.port, 0, 0);
    listen_socks[hashval] = sock;
    return 0;
    printf(">>>>>>>>>>start listen<<<<<<<<<<<<\n");
    
}

/*
接受连接 
返回与客户端通信用的socket
这里返回的socket一定是已经完成3次握手建立了连接的socket
因为只要该函数返回, 用户就可以马上使用该socket进行send和recv
*/
tju_tcp_t* tju_accept(tju_tcp_t* listen_sock){
    tju_tcp_t* new_conn = (tju_tcp_t*)malloc(sizeof(tju_tcp_t));
    int connection_found = 0; // 用于指示是否找到连接
    while (!connection_found) {
        for (int index = 0; index < MAX_SOCK; ++index) {
            if (full_connected[index] != NULL) {
            // 找到一个可用的连接
                new_conn = full_connected[index];
                full_connected[index] = NULL; // 将连接槽置空
                connection_found = 1;
                break;
            }
        }
    }

    // 日志输出以确认新连接的建立
    printf("New connection accepted: successfully established communication socket.\n");

    return new_conn;
}

/*
连接到服务端
该函数以一个socket为参数
调用函数前, 该socket还未建立连接
函数正常返回后, 该socket一定是已经完成了3次握手, 建立了连接
因为只要该函数返回, 用户就可以马上使用该socket进行send和recv
*/
int tju_connect(tju_tcp_t* tcp_socket, tju_sock_addr destination_addr) {
    // 设置远程地址信息
    tcp_socket->established_remote_addr = destination_addr;

    // 设置本地地址信息和端口
    tju_sock_addr local_address;
    local_address.ip = inet_network("172.17.0.5");
    local_address.port = 5678; // 连接时随机分配端口号，这里手动指定
    tcp_socket->established_local_addr = local_address;

    // 构造 SYN 包
    int initial_seq = CLIENT_INIT_SEQ;
    char* syn_packet = create_packet_buf(tcp_socket->established_local_addr.port, tcp_socket->established_remote_addr.port,
                                         initial_seq, 0, DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN, SYN_FLAG_MASK, 1, 0, NULL, 0);

    // 发送 SYN 包到网络层
    sendToLayer3(syn_packet, DEFAULT_HEADER_LEN);
    gettimeofday(&tcp_socket->timer.start_time, NULL);
    printf(">>>>>>>>> Sending SYN packet <<<<<<<<<\n");

    // 将状态设为 SYN_SENT
    tcp_socket->state = SYN_SENT;

    // 将 socket 存入已建立连接的哈希表
    int conn_hash = cal_hash(local_address.ip, local_address.port, destination_addr.ip, destination_addr.port);
    established_socks[conn_hash] = tcp_socket;

    // 使用 for 循环等待状态变为 ESTABLISHED
    for (;;) {
        // 获取当前时间
        gettimeofday(&tcp_socket->timer.end_time, NULL);
        struct timeval elapsed_time;
        elapsed_time.tv_sec = tcp_socket->timer.end_time.tv_sec - tcp_socket->timer.start_time.tv_sec;

        // 检查是否超时
        if (elapsed_time.tv_sec >= TIMEOUT) {
            printf(">>>>>>>>> Timeout: No SYN ACK received. Retrying... <<<<<<<<<\n");
            gettimeofday(&tcp_socket->timer.start_time, NULL);
            sendToLayer3(syn_packet, DEFAULT_HEADER_LEN);
        }

        // 如果状态为已建立连接，跳出循环
        if (tcp_socket->state == ESTABLISHED) {
            break;
        }
    }

    // 连接建立后，将 socket 添加到已建立连接的哈希表中
    established_socks[conn_hash] = tcp_socket;

    return 0;
}

int tju_send(tju_tcp_t* sock, const void *buffer, int len){
    // 这里当然不能直接简单地调用sendToLayer3
    char* data = malloc(len);
    memcpy(data, buffer, len);

    char* msg;
    uint32_t seq = 464;
    uint16_t plen = DEFAULT_HEADER_LEN + len;

    msg = create_packet_buf(sock->established_local_addr.port, sock->established_remote_addr.port, seq, 0, 
              DEFAULT_HEADER_LEN, plen, NO_FLAG, 1, 0, data, len);

    sendToLayer3(msg, plen);
    printf(">>>>>>>>>send packet seq:%d<<<<<<<<<<<<\n", seq);//debug
    
    return 0;
}
int tju_recv(tju_tcp_t* sock, void *buffer, int len){
    while(sock->received_len<=0){
        // 阻塞
    }

    while(pthread_mutex_lock(&(sock->recv_lock)) != 0); // 加锁

    int read_len = 0;
    if (sock->received_len >= len){ // 从中读取len长度的数据
        read_len = len;
    }else{
        read_len = sock->received_len; // 读取sock->received_len长度的数据(全读出来)
    }

    memcpy(buffer, sock->received_buf, read_len);

    if(read_len < sock->received_len) { // 还剩下一些
        char* new_buf = malloc(sock->received_len - read_len);
        memcpy(new_buf, sock->received_buf + read_len, sock->received_len - read_len);
        free(sock->received_buf);
        sock->received_len -= read_len;
        sock->received_buf = new_buf;
    }else{
        free(sock->received_buf);
        sock->received_buf = NULL;
        sock->received_len = 0;
    }
    pthread_mutex_unlock(&(sock->recv_lock)); // 解锁
    //添加处
    printf(">>>>>>>>>>>>>receive a packet<<<<<<<<<<<<<\n");
    return read_len;
}

//添加处：四次挥手时回复ack包
void send_ack_on_close(tju_tcp_t* tcp_socket) {
    // 计算接收窗口大小和ACK序列号
    uint16_t recv_window = 5000 * MAX_LEN - tcp_socket->received_len;
    uint32_t ack_num = tcp_socket->window.wnd_recv->expect_seq;
    uint32_t seq_num = tcp_socket->window.wnd_send->nextseq;

    // 创建ACK数据包
    char* ack_packet = create_packet_buf(tcp_socket->established_local_addr.port, tcp_socket->established_remote_addr.port, 
                                         seq_num, ack_num, DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN, ACK_FLAG_MASK, 
                                         recv_window, 0, NULL, 0);
    
    // 发送ACK包到网络层
    sendToLayer3(ack_packet, DEFAULT_HEADER_LEN);
    printf("ACK sent with sequence number %d and acknowledgment number %d\n", seq_num, ack_num);
    
    // 释放分配的内存
    free(ack_packet);
}

int tju_handle_packet(tju_tcp_t* tcp_socket, char* packet) {
    // 获取数据包的长度、标志位、源端口、目标端口和序列号
    uint32_t payload_length = get_plen(packet) - DEFAULT_HEADER_LEN;
    uint8_t packet_flags = get_flags(packet);
    uint32_t source_port = get_src(packet);
    uint32_t destination_port = get_dst(packet);
    uint32_t sequence_number = get_seq(packet);

    // 改用switch-case结构代替if判断
    switch (tcp_socket->state) {
        case LISTEN: // 如果socket处于LISTEN状态
            if (packet_flags == SYN_FLAG_MASK) { // 当收到的包是SYN包时
                printf("-------Received SYN packet-------\n");
                // 创建一个新的socket，并放入半连接队列中
                tju_tcp_t* new_socket = tju_socket();
                new_socket->state = LISTEN;
                new_socket->established_local_addr.ip = tcp_socket->bind_addr.ip;
                new_socket->established_local_addr.port = tcp_socket->bind_addr.port;
                new_socket->established_remote_addr.ip = inet_network("172.17.0.5");
                new_socket->established_remote_addr.port = source_port;

                // 计算哈希值，将新的socket放入半连接队列和已建立连接哈希表中
                int hash_value = cal_hash(new_socket->established_local_addr.ip, new_socket->established_local_addr.port,
                                          new_socket->established_remote_addr.ip, new_socket->established_remote_addr.port);
                half_connected[hash_value] = new_socket;
                established_socks[hash_value] = new_socket;

                // 发送SYN ACK进行第二次握手
                int seq_num = SERVER_INIT_SEQ;
                char* response_msg = create_packet_buf(new_socket->established_local_addr.port, new_socket->established_remote_addr.port,
                                                       seq_num, get_seq(packet) + 1, DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN,
                                                       SYN_FLAG_MASK + ACK_FLAG_MASK, 1, 0, NULL, 0);
                sendToLayer3(response_msg, DEFAULT_HEADER_LEN);
                gettimeofday(&new_socket->timer.start_time, NULL);
                printf("-------Sent SYN ACK-------\n");
                printf("[%ld] (SYN_RECV) Server sent SYN ACK packet with seq=%d and ack=%d\n", getCurrentTime(), seq_num, get_seq(packet) + 1);
                new_socket->state = SYN_RECV;
                half_connected[hash_value] = new_socket;
                established_socks[hash_value] = new_socket;

                // 使用for循环代替if判断和超时重传
                for (;;) {
                    if (established_socks[hash_value] != NULL) {
                        gettimeofday(&established_socks[hash_value]->timer.end_time, NULL);
                        struct timeval elapsed_time;
                        elapsed_time.tv_sec = established_socks[hash_value]->timer.end_time.tv_sec - established_socks[hash_value]->timer.start_time.tv_sec;
                        if (elapsed_time.tv_sec >= TIMEOUT) {
                            int seq_num = SERVER_INIT_SEQ;
                            char* resend_msg = create_packet_buf(established_socks[hash_value]->established_local_addr.port,
                                                                 established_socks[hash_value]->established_remote_addr.port, seq_num,
                                                                 get_src(packet) + 1, DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN,
                                                                 SYN_FLAG_MASK + ACK_FLAG_MASK, 1, 0, NULL, 0);
                            sendToLayer3(resend_msg, DEFAULT_HEADER_LEN);
                            printf("-------Timeout: Did not receive ACK. Retransmitting SYN ACK-------\n");
                            gettimeofday(&established_socks[hash_value]->timer.start_time, NULL);
                        }
                        break; // 退出for循环
                    }
                }
            }
            break;

        case SYN_SENT: // 如果socket处于SYN_SENT状态
            switch (packet_flags) {
                case SYN_FLAG_MASK + ACK_FLAG_MASK: // 收到SYN ACK包时
                    printf("-------Received SYN ACK packet-------\n");
                    printf("[%ld] (SYN_SENT) Client received SYN ACK packet with seq=%d and ack=%d\n", getCurrentTime(), get_seq(packet), get_ack(packet));
                    // 发送ACK包进行第三次握手
                    char* ack_msg = create_packet_buf(destination_port, source_port, get_ack(packet), get_seq(packet) + 1,
                                                      DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN, ACK_FLAG_MASK, 1, 0, NULL, 0);
                    sendToLayer3(ack_msg, DEFAULT_HEADER_LEN);
                    tcp_socket->state = ESTABLISHED;
                    printf("-------Sent ACK-------\n");
                    printf("[%ld] (ESTABLISHED) Client sent ACK packet with seq=%d and ack=%d\n", getCurrentTime(), sequence_number, get_seq(packet) + 1);
                    break;
                default:
                    break;
            }
            break;

        case SYN_RECV: // 如果socket处于SYN_RECV状态
            if (packet_flags == ACK_FLAG_MASK) { // 收到ACK包时
                printf("-------Received ACK packet-------\n");
                printf("[%ld] (SYN_RECV) Server received ACK packet with seq=%d and ack=%d\n", getCurrentTime(), get_seq(packet), get_ack(packet));
                tcp_socket->state = ESTABLISHED;

                // 将连接从半连接队列移至全连接队列
                int hash_value = cal_hash(tcp_socket->established_local_addr.ip, tcp_socket->established_local_addr.port,
                                          tcp_socket->established_remote_addr.ip, tcp_socket->established_remote_addr.port);
                half_connected[hash_value] = NULL;
                full_connected[hash_value] = tcp_socket;
                established_socks[hash_value] = tcp_socket;
            }
            break;

        case ESTABLISHED: // 如果socket已建立连接
            switch (packet_flags) {
                case SYN_FLAG_MASK + ACK_FLAG_MASK: // 收到SYN ACK包
                    printf("-------ESTABLISHED: Received SYN ACK packet -------\n");
                    // 发送ACK包
                    char* ack_msg = create_packet_buf(destination_port, source_port, get_ack(packet), get_seq(packet) + 1,
                                                      DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN, ACK_FLAG_MASK, 1, 0, NULL, 0);
                    sendToLayer3(ack_msg, DEFAULT_HEADER_LEN);
                    printf("-------ESTABLISHED: Sent ACK-------\n");
                    printf("[%ld] [Timeout] (ESTABLISHED) Client sent ACK packet with seq=%d and ack=%d\n", getCurrentTime(), sequence_number, get_seq(packet) + 1);
                    break;
                default:
                    break;
            }
            break;

        default:
            break;
    }

    // 客户端取消连接时的处理
    if (tcp_socket->established_local_addr.ip == inet_network("172.17.0.5")) {
        // 客户端主动关闭连接的处理逻辑
        switch (tcp_socket->state) {
            case FIN_WAIT_1:
                if ((packet_flags == FIN_FLAG_MASK + ACK_FLAG_MASK) || (packet_flags == FIN_FLAG_MASK)) {
                    tcp_socket->state = CLOSING;
                    tcp_socket->window.wnd_recv->expect_seq = get_seq(packet) + 1;
                    send_ack_on_close(tcp_socket);
                    tcp_socket->window.wnd_send->nextseq = tcp_socket->window.wnd_send->nextseq + 1;
                    printf("-------FIN1:client received fin-------\n");
                } else if (packet_flags == ACK_FLAG_MASK) {
                    tcp_socket->state = FIN_WAIT_2;
                    tcp_socket->window.wnd_recv->expect_seq = get_seq(packet) + 1;
                    printf("-------FIN1:client received fin's ack-------\n");
                }
                break;

            case FIN_WAIT_2:
                if ((packet_flags == FIN_FLAG_MASK + ACK_FLAG_MASK) || (packet_flags == FIN_FLAG_MASK)) {
                    tcp_socket->state = TIME_WAIT;
                    tcp_socket->window.wnd_recv->expect_seq = get_seq(packet) + 1;
                    send_ack_on_close(tcp_socket);
                    sleep(2);
                    tcp_socket->state = CLOSED;
                }
                break;

            case CLOSING:
                if (packet_flags == ACK_FLAG_MASK) {
                    tcp_socket->state = TIME_WAIT;
                    sleep(2);
                    tcp_socket->state = CLOSED;
                }
                break;

            default:
                break;
        }
    }

    // 服务端取消连接时的处理
    if (tcp_socket->established_local_addr.ip == inet_network("172.17.0.6")) {
    // 处理服务端关闭连接的逻辑
        switch (tcp_socket->state) {
            case ESTABLISHED:
                if ((packet_flags == FIN_FLAG_MASK + ACK_FLAG_MASK) || (packet_flags == FIN_FLAG_MASK)) {
                    tcp_socket->state = CLOSE_WAIT;
                    tcp_socket->window.wnd_recv->expect_seq = get_seq(packet) + 1;
                    send_ack_on_close(tcp_socket);

                    // 创建 FIN 数据包用于第二次挥手
                    char* msg = create_packet_buf(tcp_socket->established_local_addr.port, tcp_socket->established_remote_addr.port,
                                                tcp_socket->window.wnd_send->nextseq, tcp_socket->window.wnd_recv->expect_seq,
                                                DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN, FIN_FLAG_MASK, 1, 0, NULL, 0);
                    sendToLayer3(msg, DEFAULT_HEADER_LEN);
                    free(msg);
                    tcp_socket->state = LAST_ACK; // 更新状态为 LAST_ACK，表示服务端等待最终确认
                }
                break;

            case FIN_WAIT_1:
                if ((packet_flags == FIN_FLAG_MASK + ACK_FLAG_MASK) || (packet_flags == FIN_FLAG_MASK)) {
                    tcp_socket->state = CLOSING;
                    tcp_socket->window.wnd_recv->expect_seq = get_seq(packet) + 1;
                    send_ack_on_close(tcp_socket);
                    tcp_socket->window.wnd_send->nextseq = tcp_socket->window.wnd_send->nextseq + 1;
                    printf("-------FIN1:server received fin-------\n");
                }
                break;

            case CLOSING:
                if (packet_flags == ACK_FLAG_MASK) {
                    tcp_socket->state = TIME_WAIT;
                    sleep(2); // 等待 2 秒钟以确保连接关闭
                    tcp_socket->state = CLOSED;
                }
                break;

            case LAST_ACK:
                if (packet_flags == ACK_FLAG_MASK) {
                    tcp_socket->state = CLOSED; // 完全关闭连接
                }
                break;

            default:
                break;
        }
    }
}   
int tju_close (tju_tcp_t* sock){  //主动关闭，发送FIN包，进入FIN_WAIT_1状态
    char* msg = create_packet_buf(sock->established_local_addr.port, sock->established_remote_addr.port, sock->window.wnd_send->nextseq, sock->window.wnd_recv->expect_seq,
                    DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN, FIN_FLAG_MASK , 1, 0, NULL, 0);
    sendToLayer3(msg, DEFAULT_HEADER_LEN);
    sock->window.wnd_send->nextseq ++;
    sock->state = FIN_WAIT_1;
    while(sock->state != CLOSED){
    }
    return 0;
}