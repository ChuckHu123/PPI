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

    printf("Sent %zd bytes\n", sent);
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

    printf("Received %zd bytes\n", received);
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

// 交互式主循环
void interactive_mode(ppi_client_t *client) {
    char cmd[16];
    printf("\n=== Interactive Mode ===\n");

    while (1) {
        printf("Available commands [r]:read [w]:write [q]:quit\n");
        printf("Enter command: ");
        fflush(stdout);

        if (scanf("%s", cmd) <= 0) break;
        if (strcmp(cmd, "r") == 0) { //读
            // 发送数据
            if (len > 0) {
                if (ppi_client_send(client, send_buf, len) < 0) {
                    fprintf(stderr, "Failed to send data\n");
                    break;
                }

                // 接收响应
                int ret = ppi_client_recv(client, recv_buf, sizeof(recv_buf));
                if (ret < 0) {
                    fprintf(stderr, "Failed to receive data\n");
                    break;
                } else if (ret == 0) {
                    break;
                }
                printf("Server: %s\n", recv_buf);
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
