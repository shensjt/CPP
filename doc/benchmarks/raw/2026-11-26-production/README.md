# 2026-11-26 生产基线原始数据

- `h2load-healthz.log`：3 次 `/healthz`，`-n 3000 -c 32 -m 10`；
- `caddy-pidstat.log`：Caddy CPU 采样；
- `environment.txt`：环境、镜像、工具和命令；
- `h2load.log`：**invalid**。首页 34KB 的跨境 WAN 测试在 40% 时停止，测到的是带宽，不是容量；不作为结论。
