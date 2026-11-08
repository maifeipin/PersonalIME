# -*- coding: utf-8 -*-
"""
PersonalIME 预置插件与数据幂等初始化脚本
1. 读取并执行同目录下的 init_preset_plugins.sql 到 %APPDATA%/PersonalIME/user_dict.db
2. 如果后台输入法服务 (PersonalIME_Admin_Pipe) 正在运行，自动发送 IPC 通知刷新版本号，使候选窗立即热加载！
"""

import os
import sys
import sqlite3
import struct
import json

def notify_ime_service():
    """向后台服务发送 UpdatePlugin 指令以递增插件版本号 (Revision)，触发候选窗热加载"""
    pipe_path = r'\\.\pipe\PersonalIME_Admin_Pipe'
    try:
        # 1. 查询首个插件
        with open(pipe_path, 'r+b', buffering=0) as f:
            req = {'action': 'GetPlugins', 'params': {'only_enabled': False}}
            payload = json.dumps(req).encode('utf-8')
            hdr = struct.pack('<4sIII', b'PIMA', 0x0100, len(payload), 1)
            f.write(hdr + payload)
            resp_hdr = f.read(16)
            if len(resp_hdr) < 16:
                return False
            _, _, plen, _ = struct.unpack('<4sIII', resp_hdr)
            data = json.loads(f.read(plen).decode('utf-8'))
            plugins = data.get('data', {}).get('plugins', [])
            if not plugins:
                return False
            p1 = plugins[0]

        # 2. 触发 UpdatePlugin 递增服务端 revision
        with open(pipe_path, 'r+b', buffering=0) as f:
            req2 = {
                'action': 'UpdatePlugin',
                'params': {
                    'id': p1['id'],
                    'name': p1['name'],
                    'icon': p1['icon'],
                    'exec_path': p1['exec_path'],
                    'arguments': p1.get('arguments', ''),
                    'is_enabled': p1.get('is_enabled', True),
                    'sort_order': p1.get('sort_order', 0)
                }
            }
            payload2 = json.dumps(req2).encode('utf-8')
            hdr2 = struct.pack('<4sIII', b'PIMA', 0x0100, len(payload2), 2)
            f.write(hdr2 + payload2)
            resp_hdr2 = f.read(16)
            if len(resp_hdr2) == 16:
                _, _, plen2, _ = struct.unpack('<4sIII', resp_hdr2)
                f.read(plen2)
                print("[SUCCESS] 已向输入法服务发送热加载通知 (Revision 递增已生效)！")
                return True
    except Exception:
        pass
    return False

def main():
    appdata = os.environ.get("APPDATA")
    if not appdata:
        print("[ERROR] 无法获取系统环境变量 %APPDATA%")
        return 1

    db_dir = os.path.join(appdata, "PersonalIME")
    db_path = os.path.join(db_dir, "user_dict.db")
    script_dir = os.path.dirname(os.path.abspath(__file__))
    sql_path = os.path.join(script_dir, "init_preset_plugins.sql")

    if not os.path.exists(sql_path):
        print(f"[ERROR] 未找到 SQL 脚本文件: {sql_path}")
        return 1

    os.makedirs(db_dir, exist_ok=True)
    print(f"[INFO] 目标数据库: {db_path}")
    print(f"[INFO] SQL 脚本:   {sql_path}")

    try:
        with open(sql_path, "r", encoding="utf-8") as f:
            sql_content = f.read()

        conn = sqlite3.connect(db_path)
        conn.executescript(sql_content)
        conn.commit()

        print("\n[SUCCESS] 数据库初始化完成！")

        print("\n【当前已配置插件】")
        cursor = conn.execute("SELECT id, sort_order, icon, name, exec_path, arguments FROM plugins ORDER BY sort_order ASC")
        rows = cursor.fetchall()
        for r in rows:
            args_str = f" {r[5]}" if r[5] else ""
            print(f"  [{r[0]}] 排序:{r[1]} {r[2]} {r[3]} -> {r[4]}{args_str}")

        print("\n【当前自定义词条】")
        cursor = conn.execute(
            "SELECT c.scheme, c.code, e.text FROM entries e JOIN entry_codes c ON e.id = c.entry_id ORDER BY c.code"
        )
        entries = cursor.fetchall()
        if entries:
            for c in entries:
                scheme_name = {1: "拼音", 2: "五笔", 3: "自定义", 4: "双拼"}.get(c[0], "其他")
                print(f"  [{scheme_name}] 编码: {c[1]} -> 词条: {c[2]}")
        else:
            print("  (暂无自定义词条)")

        conn.close()

        # 尝试通知后台服务刷新
        notify_ime_service()

        return 0
    except Exception as ex:
        print(f"\n[ERROR] 执行失败: {ex}")
        return 1

if __name__ == "__main__":
    sys.exit(main())
