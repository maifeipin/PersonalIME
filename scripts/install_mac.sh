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

# Clean up duplicate copy in user directory if present to avoid duplicate entries in System Settings
if [ -d "${USER_DIR}/${APP_NAME}" ]; then
    rm -rf "${USER_DIR}/${APP_NAME}" 2>/dev/null || true
fi

echo "==> Re-signing installed bundle..."
codesign --force --sign - "${DEST}"

echo "==> Registering with macOS TIS and LaunchServices..."
/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister -f "${DEST}"
xcrun swift - <<EOF
import Foundation
import Carbon

let url = URL(fileURLWithPath: "${DEST}")
_ = TISRegisterInputSource(url as CFURL)
if let bundle = Bundle(path: "/System/Library/PrivateFrameworks/IntlPreferences.framework"), bundle.load() {
    if let cls = NSClassFromString("IPInputSource") as AnyObject as? NSObjectProtocol {
        let sel = NSSelectorFromString("inputSources")
        if cls.responds(to: sel), let sources = cls.perform(sel)?.takeUnretainedValue() as? [NSObject] {
            for s in sources where s.responds(to: NSSelectorFromString("inputSourceID")) {
                let iid = s.perform(NSSelectorFromString("inputSourceID"))?.takeUnretainedValue() as? String
                if iid == "com.personalime.inputmethod.PersonalIME.Hans" {
                    _ = s.perform(NSSelectorFromString("setEnabled:"), with: true as NSNumber)
                }
            }
        }
    }
}
EOF

echo "==> Restarting System Settings & Menu bar keyboard cache..."
pkill -9 -f "KeyboardSettings" 2>/dev/null || true
pkill -9 -f "System Settings" 2>/dev/null || true
killall TextInputMenuAgent 2>/dev/null || true

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

