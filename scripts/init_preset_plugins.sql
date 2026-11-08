-- ==============================================================================
-- PersonalIME: 预置插件与常用数据幂等初始化脚本
-- 目标数据库：%APPDATA%\PersonalIME\user_dict.db
-- 特性：
--   1. 幂等执行：可反复重复运行，已存在的记录会自动跳过，绝不产生重复项或主键冲突；
--   2. 自动建表：如果目标数据库尚未建表，自动建立标准表结构；
--   3. 覆盖内容：
--      - 预置插件 1：计算器 (calc.exe)
--      - 预置插件 2：画图 (mspaint.exe)
--      - 预置插件 3：Edge浏览器 (msedge.exe https://maifeipin.com)
--      - 实用工具 4：截图 (Snipaste / snipaste.exe)
--      - 核心组件 5：中华万年历 (PersonalIME.Manager.exe --calendar)
--      - 自定义词条模板：演示如何幂等添加个性化短语词条
-- ==============================================================================

-- ------------------------------------------------------------------------------
-- 1. 确保 plugins 表结构存在
-- ------------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS plugins (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL,
    icon TEXT NOT NULL,
    exec_path TEXT NOT NULL,
    arguments TEXT NOT NULL DEFAULT '',
    is_enabled INTEGER NOT NULL DEFAULT 1,
    sort_order INTEGER NOT NULL DEFAULT 0,
    created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now'))
);

CREATE TABLE IF NOT EXISTS plugin_logs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    plugin_id INTEGER,
    plugin_name TEXT NOT NULL,
    command_line TEXT NOT NULL,
    executed_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')),
    status TEXT NOT NULL,
    message TEXT NOT NULL DEFAULT ''
);

-- ------------------------------------------------------------------------------
-- 2. 幂等插入 5 个插件（3个预置 + Snipaste + 中华万年历）
-- ------------------------------------------------------------------------------

-- (1) 计算器
INSERT INTO plugins (name, icon, exec_path, arguments, is_enabled, sort_order)
SELECT '计算器', '🧮', 'calc.exe', '', 1, 0
WHERE NOT EXISTS (SELECT 1 FROM plugins WHERE name = '计算器');

-- (2) 画图
INSERT INTO plugins (name, icon, exec_path, arguments, is_enabled, sort_order)
SELECT '画图', '🎨', 'mspaint.exe', '', 1, 1
WHERE NOT EXISTS (SELECT 1 FROM plugins WHERE name = '画图');

-- (3) Edge浏览器
INSERT INTO plugins (name, icon, exec_path, arguments, is_enabled, sort_order)
SELECT 'Edge浏览器', '🌐', 'msedge.exe', 'https://maifeipin.com', 1, 2
WHERE NOT EXISTS (SELECT 1 FROM plugins WHERE name = 'Edge浏览器');

-- (4) 截图 (Snipaste)
INSERT INTO plugins (name, icon, exec_path, arguments, is_enabled, sort_order)
SELECT '截图 (Snipaste)', '✂️', 'snipaste.exe', '', 1, 3
WHERE NOT EXISTS (SELECT 1 FROM plugins WHERE name = '截图 (Snipaste)' OR name = 'Snipaste');

-- (5) 中华万年历
INSERT INTO plugins (name, icon, exec_path, arguments, is_enabled, sort_order)
SELECT '中华万年历', '📅', '%ProgramFiles%\PersonalIME\manager\PersonalIME.Manager.exe', '--calendar', 1, 4
WHERE NOT EXISTS (SELECT 1 FROM plugins WHERE name = '中华万年历' OR name = '万年历');

-- ------------------------------------------------------------------------------
-- 3. 自定义词条表结构与幂等插入示例（词库词条）
--    说明：
--      scheme: 1=拼音(pinyin), 2=五笔86(wubi), 3=自定义短语(custom), 4=双拼(shuangpin)
-- ------------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS entries (
    id TEXT PRIMARY KEY,
    text TEXT NOT NULL,
    source INTEGER NOT NULL DEFAULT 1,
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS entry_codes (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    entry_id TEXT NOT NULL REFERENCES entries(id) ON DELETE CASCADE,
    scheme INTEGER NOT NULL,
    code TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    UNIQUE(entry_id, scheme, code)
);

CREATE INDEX IF NOT EXISTS idx_entry_codes_lookup ON entry_codes(scheme, code);

-- 示例词条 1：快捷输入邮箱（输入 ;mail 输出常用邮箱）
INSERT OR IGNORE INTO entries (id, text, source, created_at, updated_at)
VALUES ('custom_preset_email', 'maifeipin@gmail.com', 1, strftime('%s', 'now'), strftime('%s', 'now'));

INSERT OR IGNORE INTO entry_codes (entry_id, scheme, code, created_at)
VALUES ('custom_preset_email', 3, ';mail', strftime('%s', 'now'));

-- 示例词条 2：快捷输入地址（输入 ;dz 输出示例地址，您可在后续随时在设置中修改）
INSERT OR IGNORE INTO entries (id, text, source, created_at, updated_at)
VALUES ('custom_preset_addr', '北京市海淀区中关村南大街', 1, strftime('%s', 'now'), strftime('%s', 'now'));

INSERT OR IGNORE INTO entry_codes (entry_id, scheme, code, created_at)
VALUES ('custom_preset_addr', 3, ';dz', strftime('%s', 'now'));
