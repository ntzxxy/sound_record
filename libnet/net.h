#ifndef __NET_H__
#define __NET_H__

/**
 * @brief 将本地文件通过 TCP 发送到服务器
 * * @param filename 要发送的文件路径 (如 "rec_001.wav")
 * @param ip       服务器的 IP 地址 (如 "172.25.6.200")
 * @param port     服务器监听的端口 (如 8080)
 * @return int     成功返回 0，失败返回 -1
 */
int send_file_to_server(const char *filename, const char *ip, int port);

#endif