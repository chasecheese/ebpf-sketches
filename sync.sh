#!/bin/bash

PARENT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIR_NAME="$(basename "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)")"

# 定义要排除的路径为环境变量
EXCLUDE_PATHS=(
  '.git'
  'linux-6.5'
  'libbpf-1.3.0'
  'vmlinux.h'
)

# 定义服务器列表
SERVERS=(
  "root@114.212.85.240:~/"
  "root@114.212.85.15:~/"
)

# 定义同步函数
sync_to_server() {
  local dest=$1
  echo -e "---------------------------------------------------"
  echo -e "Sync to $dest start."
  rsync -avz \
    "${EXCLUDE_PATHS[@]/#/--exclude=}" \
    "$PARENT_DIR/$DIR_NAME" \
    "$dest"
  echo -e "Sync to $dest done."
  echo -e "---------------------------------------------------\n"
}

# 遍历服务器列表，同步到每个服务器
for server in "${SERVERS[@]}"; do
  sync_to_server "$server"
done

echo -e "***** All syncs completed. *****\n"

