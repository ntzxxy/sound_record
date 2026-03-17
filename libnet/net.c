#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "net.h"

int send_file_to_server(const char *filename, const char *ip, int port) {
    int sockfd = -1;
    struct sockaddr_in serv_addr;
    FILE *fp = NULL;
    char buffer[1024];
    int read_bytes, send_bytes;

    // 1. 创建 TCP 套接字
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("[NET] Socket 创建失败");
        return -1;
    }

    // 2. 配置服务器地址信息
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port); // 主机字节序转网络字节序
    
    if (inet_pton(AF_INET, ip, &serv_addr.sin_addr) <= 0) {
        fprintf(stderr, "[NET] 无效的 IP 地址: %s\n", ip);
        close(sockfd);
        return -1;
    }

    // 3. 建立连接 (三次握手就在这一步发生)
    printf("[NET] 正在连接服务器 %s:%d...\n", ip, port);
    if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("[NET] 连接服务器失败");
        close(sockfd);
        return -1;
    }

    // 4. 打开要发送的文件
    fp = fopen(filename, "rb");
    if (fp == NULL) {
        perror("[NET] 无法打开音频文件");
        close(sockfd);
        return -1;
    }

    // 5. 循环读取文件并发送到网络缓冲区
    printf("[NET] 开始上传文件: %s\n", filename);
    while ((read_bytes = fread(buffer, 1, sizeof(buffer), fp)) > 0) {
        // 注意：send 并不保证一次发完 read_bytes，但在 TCP 下通常对小块数据没问题
        send_bytes = send(sockfd, buffer, read_bytes, 0);
        if (send_bytes < 0) {
            perror("[NET] 数据发送中断");
            break;
        }
    }

    // 6. 清理资源
    printf("[NET] 上传完成，正在断开连接。\n");
    fclose(fp);
    close(sockfd);

    return 0;
}