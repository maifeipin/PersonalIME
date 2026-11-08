#!/usr/bin/env bash
set -e

# Target paths
GLOBAL_DIR="/Library/Input Methods"
USER_DIR="${HOME}/Library/Input Methods"
APP_NAME="PersonalIME.app"
SRC="build_mac/${APP_NAME}"

if [ ! -d "${SRC}" ]; then
    echo "ERROR: ${SRC} not found. Please build the project first."
    exit 1
fi

echo "==> Stopping any running PersonalIME instance..."
killall -9 PersonalIME 2>/dev/null || true

# Modern macOS System Settings (KeyboardSettings.appex) is sandboxed and cannot read ~/Library/Input Methods.
# It requires installation into /Library/Input Methods.
if [ -w "${GLOBAL_DIR}" ]; then
    DEST="${GLOBAL_DIR}/${APP_NAME}"
    echo "==> Installing ${APP_NAME} to ${GLOBAL_DIR} (Global)..."
    rm -rf "${DEST}"
    cp -R "${SRC}" "${DEST}"
else
    echo "==> Note: Installing to /Library/Input Methods requires administrator privileges."
    echo "==> Trying sudo cp..."
    sudo rm -rf "${GLOBAL_DIR}/${APP_NAME}"
    sudo cp -R "${SRC}" "${GLOBAL_DIR}/${APP_NAME}"
    DEST="${GLOBAL_DIR}/${APP_NAME}"
fi

# Also keep a copy in user directory for user session fallback
rm -rf "${USER_DIR}/${APP_NAME}"
mkdir -p "${USER_DIR}"
cp -R "${SRC}" "${USER_DIR}/${APP_NAME}"

echo "==> Registering with macOS LaunchServices..."
/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister -f "${DEST}"

echo "==> Restarting System Settings keyboard cache..."
killall KeyboardSettings "System Settings" 2>/dev/null || true

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

