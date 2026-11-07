#!/usr/bin/env bash
set -e

# Target paths
INSTALL_DIR="${HOME}/Library/Input Methods"
APP_NAME="PersonalIME.app"
DEST="${INSTALL_DIR}/${APP_NAME}"
SRC="build_mac/${APP_NAME}"

if [ ! -d "${SRC}" ]; then
    echo "ERROR: ${SRC} not found. Please build the project first."
    exit 1
fi

echo "==> Stopping any running PersonalIME instance..."
killall -9 PersonalIME 2>/dev/null || true

echo "==> Installing ${APP_NAME} to ${INSTALL_DIR}..."
mkdir -p "${INSTALL_DIR}"
rm -rf "${DEST}"
cp -R "${SRC}" "${DEST}"

echo "==> Registering with macOS LaunchServices..."
/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister -f "${DEST}"

echo "==> Launching ${APP_NAME}..."
open "${DEST}" || true

echo "=========================================================================="
echo "  人人输入法 (PersonalIME) 已成功安装至本机！"
echo "=========================================================================="
echo "使用说明："
echo "1. 打开「系统设置」->「键盘」->「文字输入」-> 点击「编辑...」"
echo "2. 点击左下角「+」，在「简体中文」列表下方选择「人人输入法」并添加"
echo "3. 按 Control+Space (或 fn / 菜单栏图标) 切换至「人人输入法」即可开始打字"
echo "=========================================================================="

