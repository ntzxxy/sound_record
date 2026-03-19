import socket
import os
import time

# 配置信息
LISTEN_IP = '0.0.0.0'  # 监听所有网卡
LISTEN_PORT = 8080      # 必须与你板子代码里的端口一致
SAVE_DIR = "./received_audio"

if not os.path.exists(SAVE_DIR):
    os.makedirs(SAVE_DIR)

def start_server():
    # 1. 创建 TCP Socket
    server_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    # 允许端口复用（防止脚本重启时报端口占用）
    server_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    
    # 2. 绑定并监听
    server_sock.bind((LISTEN_IP, LISTEN_PORT))
    server_sock.listen(5)
    print("[*] 服务器启动，等待板子连接 (端口 {})...".format(LISTEN_PORT))

    try:
        while True:
            # 3. 等待板子连接
            client_sock, addr = server_sock.accept()
            print("[+] 接收到来自 {} 的连接".format(addr))

            # 生成唯一文件名：以当前时间命名
            timestamp = time.strftime("%Y%m%d_%H%M%S")
            filename = os.path.join(SAVE_DIR, "voice_{}.wav".format(timestamp))

            # 4. 接收数据并写入文件
            with open(filename, 'wb') as f:
                while True:
                    data = client_sock.recv(4096) # 每次接收4KB
                    if not data:
                        break # 板子断开连接，说明发完了
                    f.write(data)
            
            print("[OK] 文件已保存至: {}".format(filename))
            client_sock.close()
            
            # --- 这里就是后续对接 AI 网站的入口 ---
            # print("[DEBUG] 准备将文件上传至 AI 平台...")
            
    except KeyboardInterrupt:
        print("\n[*] 服务器关闭")
        server_sock.close()

if __name__ == "__main__":
    start_server()