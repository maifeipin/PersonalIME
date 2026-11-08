# -*- coding: utf-8 -*-
"""
PersonalIME 预置插件与数据幂等初始化脚本
读取同目录下的 init_preset_plugins.sql，执行到 %APPDATA%/PersonalIME/user_dict.db
"""

import os
import sys
import sqlite3

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
        return 0
    except Exception as ex:
        print(f"\n[ERROR] 执行失败: {ex}")
        return 1

if __name__ == "__main__":
    sys.exit(main())

