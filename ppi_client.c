#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define BUFFER_SIZE 1024
#define DEFAULT_PORT 2000
#define DEFAULT_HOST "193.169.202.107"

// PPI Client 结构体
typedef struct {
    int sockfd;
    char host[64];
    int port;
} ppi_client_t;

// 初始化 client
int ppi_client_init(ppi_client_t *client, const char *host, int port) {
    if (!client) {
        fprintf(stderr, "Error: Invalid client pointer\n");
        return -1;
    }

    strncpy(client->host, host ? host : DEFAULT_HOST, sizeof(client->host) - 1);
    client->host[sizeof(client->host) - 1] = '\0';
    client->port = port ? port : DEFAULT_PORT;
    client->sockfd = -1;

    printf("PPI Client initialized: %s:%d\n", client->host, client->port);
    return 0;
}

// 连接到 server
int ppi_client_connect(ppi_client_t *client) {
    if (!client) {
        fprintf(stderr, "Error: Invalid client pointer\n");
        return -1;
    }

    // 创建 socket
    client->sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (client->sockfd < 0) {
        perror("Socket creation failed");
        return -1;
    }

    // 设置 server 地址
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(client->port);

    if (inet_pton(AF_INET, client->host, &server_addr.sin_addr) <= 0) {
        perror("Invalid address");
        close(client->sockfd);
        client->sockfd = -1;
        return -1;
    }

    // 连接 server
    if (connect(client->sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Connection failed");
        close(client->sockfd);
        client->sockfd = -1;
        return -1;
    }

    printf("Connected to server: %s:%d\n", client->host, client->port);
    return 0;
}

// 发送数据
int ppi_client_send(ppi_client_t *client, const char *data, size_t len) {
    if (!client || client->sockfd < 0 || !data) {
        fprintf(stderr, "Error: Invalid parameters\n");
        return -1;
    }

    ssize_t sent = send(client->sockfd, data, len, 0);
    if (sent < 0) {
        perror("Send failed");
        return -1;
    }

    //printf("Sent %zd bytes\n", sent);
    return (int)sent;
}

// 接收数据
int ppi_client_recv(ppi_client_t *client, char *buffer, size_t buf_size) {
    if (!client || client->sockfd < 0 || !buffer) {
        fprintf(stderr, "Error: Invalid parameters\n");
        return -1;
    }

    memset(buffer, 0, buf_size);
    ssize_t received = recv(client->sockfd, buffer, buf_size - 1, 0);
    if (received < 0) {
        perror("Recv failed");
        return -1;
    } else if (received == 0) {
        printf("Server disconnected\n");
        return 0;
    }

    //printf("Received %zd bytes\n", received);
    return (int)received;
}

// 断开连接
void ppi_client_disconnect(ppi_client_t *client) {
    if (client && client->sockfd >= 0) {
        close(client->sockfd);
        client->sockfd = -1;
        printf("Disconnected from server\n");
    }
}

// 清理资源
void ppi_client_cleanup(ppi_client_t *client) {
    if (client) {
        ppi_client_disconnect(client);
        memset(client, 0, sizeof(ppi_client_t));
    }
}

void build_read_frame(unsigned char *req_1){ 
    const unsigned char header[26] = {
        0x68, 0x1B, 0x1B, 0x68, 0x02, 0x00, 0x6C, 0x32, 
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0E, 0x00, 
        0x00, 0x04, 0x01, 0x12, 0x0A, 0x10,//读命令的前22个字节都是相同的
        0x00,//字节22 读取数据的单位
        0x00,//字节23 恒0
        0x00,//字节24 表示数据个数
        0x00,//字节25 恒0
    };
    memcpy(req_1, header, 26);
}

unsigned char calculate_fcs(const unsigned char *data, int start, int end) {
    unsigned char fcs = 0;
    for (int i = start; i <= end; i++) {
        fcs += data[i];
    }
    return fcs;
}

int parse_res_2(unsigned char *res_2, uint16_t read_mode, uint16_t addr) {
    if (read_mode == 0){
        printf("Reg[%d]: %d\n", addr, res_2[25] & 0x01);
    }else{
        uint16_t len = (res_2[23]<<8 | res_2[24])/8;
        for(int i = 25; i < 25+len; i++){
            printf("Reg[%d]: %02X\n", addr+i-25, res_2[i]);
        }
    }
    return 0;
}

int ppi_read(ppi_client_t *client, uint16_t reg_type, uint16_t addr, uint16_t qty, uint16_t read_mode) {
    unsigned char req_1[33]={0}; //第一次发送的报文
    build_read_frame(req_1);
    switch (read_mode){
        case 0: req_1[22] = 0x01; break;// bit
        case 1: req_1[22] = 0x02; break;// byte
        case 2: req_1[22] = 0x04; break;// word
        case 3: req_1[22] = 0x06; break;// double word
    }
    req_1[24] = qty;
    if (reg_type == 1){// 只有读取V存储器的时候这里是0x01，否则是0x00
        req_1[26]=0x01;
        req_1[27]=0x84;
    } else {
        req_1[26]=0x00;
        if (reg_type == 0) req_1[27]=0x83; //M寄存器是83
        else return -1;
    }

    uint32_t offset = addr * 8;  // 计算偏移量
    req_1[28] = (offset >> 16) & 0xFF;
    req_1[29] = (offset >> 8) & 0xFF;
    req_1[30] = offset & 0xFF;
    req_1[31] = calculate_fcs(req_1, 4, 30);  // FCS 校验码
    req_1[32] = 0x16;  // ED 结束符
    if(ppi_client_send(client, (const char *)req_1, 33) == -1){
        printf("Send failed in req 1\n");
        return -1;
    }

    // 接收第一个响应字节（应该是 0xE5）
    unsigned char res_1;
    int recv_len = ppi_client_recv(client, (char *)&res_1, 2);
    if (recv_len <= 0 || res_1 != 0xE5){
        printf("Invalid response: expected 0xE5, got 0x%02X\n", res_1);
        return -2;
    }

    unsigned char req_2[6];
    req_2[0] = 0x10; //固定的
    req_2[1] = 0x02; //主机地址
    req_2[2] = 0x00; //PLC地址
    req_2[3] = 0x5C; //固定
    req_2[4] = calculate_fcs(req_2, 1, 3);
    req_2[5] = 0x16; //固定
    if(ppi_client_send(client, (const char *)req_2, 6) == -1){
        printf("Send failed in req 2\n");
        return -3;
    }

    unsigned char res_2[256];
    int recv_len2 = ppi_client_recv(client, (char *)res_2, sizeof(res_2));
    if(recv_len2 == -1){
        printf("Recv failed in res 2\n");
        return -4;
    }
    if (res_2[21] != 0xFF)return -4
    parse_res_2(res_2, read_mode, addr);
    return 0;
}

// 交互式主循环
void interactive_mode(ppi_client_t *client) {
    char cmd[16];
    printf("\n=== Interactive Mode ===\n");

    while (1) {
        printf("\nAvailable commands [r]:read [w]:write [q]:quit\n");
        printf("Enter command: ");
        fflush(stdout);

        if (scanf("%s", cmd) <= 0) break;
        if (strcmp(cmd, "r") == 0) { //读
            uint16_t addr, qty, rm, rt;
            char read_mode[16];
            char reg_type[16];
            printf("Enter Register Type M/V : ");
            scanf("%s", reg_type);
            if (reg_type[0] == 'M') rt = 0;
            else if (reg_type[0] == 'V') rt = 1;
            else {
                printf("Unknown register type: %s\n", reg_type);
                continue;
            }

            printf("Enter read mode [b]:bit [B]:byte [w]:word [dw]:double word : ");
            scanf("%s", read_mode);
            if (strcmp(read_mode, "b") == 0) rm = 0;
            else if (strcmp(read_mode, "B") == 0) rm = 1;
            else if (strcmp(read_mode, "w") == 0) rm = 2;
            else if (strcmp(read_mode, "dw") == 0) rm = 3;
            else {
                printf("Unknown read mode: %s\n", read_mode);
                continue;
            }

            printf("Enter Address: ");
            scanf("%hu", &addr);
            if (rm != 0){
                printf("Enter quantity: ");
                scanf("%hu", &qty);
            }else qty = 1;
            
            if (qty > 208){//判断输入合理性
                printf("Invalid quantity\n");
                continue;
            }

            int ret = ppi_read(client, rt, addr, qty, rm);
            if(ret == -1){
                printf("Read failed in req 1\n");
            }else if (ret == -2) {
                printf("Read failed in res 1\n");
            }else if (ret == -3) {
                printf("Read failed in req 2\n");
            }else if (ret == -4) {
                printf("Read failed in res 2\n");
            }else {//成功，目前这里没东西

            }
        } else if (strcmp(cmd, "w") == 0) { //写

        } else if (strcmp(cmd, "q") == 0) { //退出
            break;
        } else {
            printf("Unknown command: %s\n", cmd);
        }
    }
}

int main(int argc, char *argv[]) {
    const char *host = DEFAULT_HOST;
    int port = DEFAULT_PORT;

    // 解析命令行参数
    if (argc > 1) {
        host = argv[1];
    }
    if (argc > 2) {
        port = atoi(argv[2]);
    }

    printf("=== PPI Client ===\n");

    // 初始化 client
    ppi_client_t client;
    if (ppi_client_init(&client, host, port) != 0) {
        return EXIT_FAILURE;
    }

    // 连接到 server
    if (ppi_client_connect(&client) != 0) {
        return EXIT_FAILURE;
    }

    // 进入交互模式
    interactive_mode(&client);

    // 清理资源
    ppi_client_cleanup(&client);
    printf("Client exited\n");

    return EXIT_SUCCESS;
}
