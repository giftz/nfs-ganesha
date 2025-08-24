# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概述

NFS-Ganesha 是一个运行在用户态的 NFS 文件服务器，支持 NFSv3、NFSv4、NFSv4.1 和 9p.2000L 协议。项目采用 FSAL (File System Abstraction Layer) 架构，允许连接到多种后端存储系统。

## 构建命令

### 基本构建流程
```bash
# 克隆代码（包含子模块）
git clone --recursive https://github.com/nfs-ganesha/nfs-ganesha.git

# 初始化子模块（如果未使用 --recursive）
git submodule update --init

# 创建并进入构建目录
mkdir build && cd build

# 配置构建
cmake ../src

# 编译
make

# 安装
make install
```

### 开发构建配置
```bash
# 维护者模式（启用严格编译）
cmake -DCMAKE_BUILD_TYPE=Maintainer ../src

# 完整功能构建（开启所有选项）
cmake -DCMAKE_BUILD_TYPE=Maintainer -DBUILD_CONFIG=everything ../src

# 单个 FSAL 构建示例
cmake -D_USE_9P=OFF -D_HANDLE_MAPPING=ON -DALLOCATOR=tcmalloc ../src
```

### 测试命令
```bash
# 运行基本测试
./src/test/run_test_mode.sh <build_path>

# 运行 Connectathon 测试
# 需要先安装 cthon04 测试套件

# 运行 pynfs 测试（NFSv4.0）
./testserver.py -v --outfile results.log --maketree 127.0.0.1:/export/test1/pynfs --showomit --secure --rundeps all ganesha

# 运行 pynfs 测试（NFSv4.1）  
./testserver.py -v --outfile results.log --maketree 127.0.0.1:/export/test1/pynfs --showomit --rundeps all ganesha
```

## 核心架构

### 主要组件层次结构

1. **协议层 (Protocols/)**
   - NFS (NFSv3/NFSv4)：处理 NFS 协议请求
   - 9P：处理 9p.2000L 协议请求
   - MOUNT、NLM、RQUOTA：辅助协议支持

2. **FSAL 抽象层 (FSAL/)**
   - **FSAL_VFS**：标准 POSIX 文件系统接口
   - **FSAL_CEPH**：Ceph 分布式存储
   - **FSAL_GLUSTER**：GlusterFS 集群文件系统
   - **FSAL_GPFS**：IBM GPFS 并行文件系统
   - **FSAL_RGW**：Ceph RADOS Gateway
   - **FSAL_PROXY_V3/V4**：代理其他 NFS 服务器
   - **Stackable_FSALs/FSAL_MDCACHE**：元数据缓存层

3. **核心服务 (MainNFSD/)**
   - **nfs_main.c**：主程序入口
   - **nfs_worker_thread.c**：工作线程管理
   - **nfs_rpc_dispatcher_thread.c**：RPC 请求分发
   - **nfs_admin_thread.c**：管理接口

4. **状态管理 (SAL/)**
   - **nfs4_state.c**：NFSv4 状态管理
   - **nfs4_clientid.c**：客户端 ID 管理
   - **recovery/**：故障恢复机制

5. **支持库 (support/)**
   - **export_mgr.c**：导出管理
   - **client_mgr.c**：客户端管理
   - **server_stats.c**：性能统计

### FSAL 插件架构
每个 FSAL 实现标准化接口，主要方法包括：
- `lookup`、`create`、`mkdir`：目录操作
- `read`、`write`、`open`、`close`：文件 I/O
- `getattrs`、`setattrs`：属性操作
- `link`、`symlink`、`rename`、`unlink`：文件系统操作

## 配置系统

### 配置文件结构
```
NFS_CORE_PARAM { ... }     # 核心参数
EXPORT { ... }             # 导出定义  
FSAL { ... }               # FSAL 特定配置
LOG { ... }                # 日志配置
```

### 配置样例位置
- `src/config_samples/`：各 FSAL 的配置样例
- `src/config_samples/ganesha.conf.example`：完整配置示例

## 测试和调试

### 测试脚本
- `src/test/run_test_mode.sh`：测试模式运行脚本
- `src/scripts/test_through_mountpoint/`：挂载点测试脚本

### 常用测试工具
- **Connectathon (cthon04)**：NFS 协议一致性测试
- **pynfs**：Python NFS 测试框架
- **pjdfstest**：POSIX 文件系统测试

### 调试工具
- `src/tools/multilock/`：锁机制测试工具
- `src/scripts/ganesha-top/`：性能监控工具
- `src/scripts/ganeshactl/`：管理控制工具

## 开发规范

### 代码提交流程
1. 使用 Gerrithub 进行代码审查（不接受 GitHub PR）
2. 安装 git hooks：`src/scripts/git_hooks/install_git_hooks.sh`
3. 提交时使用 `--signoff` 选项
4. 推送到 Gerrithub：`git push gerrit HEAD:refs/for/next`

### 分支管理
- **next**：主开发分支
- **fsal-seaweed-poc**：当前工作分支

### 代码规范
- 遵循项目现有代码风格
- 使用制表符缩进，宽度为 8
- 函数和变量使用下划线命名法

## 监控和运维

### 性能监控
- 支持 Prometheus 指标导出
- Grafana 仪表板配置：`src/config_samples/grafana.dashboard.json`
- ganesha-top 实时监控：`src/scripts/ganesha-top/`

### 日志配置
- 支持结构化日志输出
- 可配置日志级别和输出目标
- 日志轮转配置样例：`src/config_samples/logrotate_ganesha`

## 容器化部署

### Docker 支持
- Dockerfile 模板：`src/scripts/docker/`
- Podman 容器化：`src/scripts/podman/`

### 系统服务
- systemd 服务配置：`src/scripts/systemd/`
- SysV init 脚本：`src/scripts/init.d/`