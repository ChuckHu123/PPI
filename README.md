# PPI Client
为了实习后续开发PPI协议相关内容，先通过这个项目熟悉一下协议（实习相关的ppi与s7转换可以看internship目录）
简易的 TCP PPI 客户端,用于与server 进行通信。

## 运行

默认连接到 `193.169.202.107:2080`:
```bash
./ppi_client
```

指定主机和端口:
```bash
./ppi_client <host> <port>
```

例如:
```bash
./ppi_client 192.168.1.100 9000
```

## API

主要函数:
- `ppi_client_init()` - 初始化客户端
- `ppi_client_connect()` - 连接到服务器
- `ppi_client_send()` - 发送数据
- `ppi_client_recv()` - 接收数据
- `ppi_client_disconnect()` - 断开连接
- `ppi_client_cleanup()` - 清理资源
