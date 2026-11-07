import Cocoa
import SwiftUI

// MARK: - Delegate Protocol

@objc public protocol CandidatePanelDelegate: AnyObject {
    func candidatePanelDidSelect(indexOnPage: Int)
    func candidatePanelDidHover(indexOnPage: Int)
    func candidatePanelDidPage(delta: Int)
    func candidatePanelDidOpenSettings()
}

// MARK: - Settings Data Source Protocol

@objc public protocol SettingsDataSource: AnyObject {
    func getPrimaryScheme() -> String
    func setPrimaryScheme(_ scheme: String)
    func getShuangpinLayout() -> String
    func setShuangpinLayout(_ layout: String)
    func getMixedInputEnabled() -> Bool
    func setMixedInputEnabled(_ enabled: Bool)
    func getPageSize() -> Int
    func setPageSize(_ size: Int)
    func getFallbackThreshold() -> Int
    func setFallbackThreshold(_ threshold: Int)
    func getFrequencyEnabled() -> Bool
    func setFrequencyEnabled(_ enabled: Bool)
    func getLearningEnabled() -> Bool
    func setLearningEnabled(_ enabled: Bool)
    func getStatistics() -> [String: Int]
    func resetLearningStats() -> Bool
    func exportData(to path: String) -> String?
    func importData(from path: String, replace: Bool) -> String?
    func allUserEntries() -> [[String: String]]
    func addUserEntry(text: String, scheme: String, code: String) -> String?
    func deleteUserEntry(id: String) -> Bool
}

// MARK: - Candidate Item Model

public struct CandidateDisplayItem: Identifiable, Equatable {
    public let id: Int
    public let text: String
    public let reverseLookupCode: String
    public let isPinned: Bool

    public init(id: Int, text: String, reverseLookupCode: String = "", isPinned: Bool = false) {
        self.id = id
        self.text = text
        self.reverseLookupCode = reverseLookupCode
        self.isPinned = isPinned
    }
}

// MARK: - Candidate Bar SwiftUI View

struct CandidateBarView: View {
    let items: [CandidateDisplayItem]
    let selectedIndex: Int
    let page: Int
    let totalPages: Int
    weak var delegate: CandidatePanelDelegate?

    var body: some View {
        HStack(spacing: 2) {
            ForEach(items) { item in
                let isSelected = item.id == selectedIndex
                HStack(spacing: 4) {
                    if item.id < 9 {
                        Text("\(item.id + 1)")
                            .font(.system(size: 11, weight: .bold, design: .rounded))
                            .foregroundStyle(isSelected ? Color.white : Color.secondary)
                    }
                    if item.isPinned {
                        Text("📌")
                            .font(.system(size: 9))
                    }
                    Text(item.text)
                        .font(.system(size: 15, weight: isSelected ? .semibold : .regular))
                        .foregroundStyle(isSelected ? Color.white : Color.primary)
                        .lineLimit(1)

                    if !item.reverseLookupCode.isEmpty {
                        Text("[\(item.reverseLookupCode)]")
                            .font(.system(size: 11, weight: .regular, design: .monospaced))
                            .foregroundStyle(isSelected ? Color.white.opacity(0.85) : Color.secondary)
                            .lineLimit(1)
                    }
                }
                .padding(.horizontal, 7)
                .padding(.vertical, 4)
                .background(
                    RoundedRectangle(cornerRadius: 6, style: .continuous)
                        .fill(isSelected ? Color.accentColor : Color.clear)
                )
                .contentShape(RoundedRectangle(cornerRadius: 6, style: .continuous))
                .onHover { isHovered in
                    if isHovered {
                        delegate?.candidatePanelDidHover(indexOnPage: item.id)
                    }
                }
                .onTapGesture {
                    delegate?.candidatePanelDidSelect(indexOnPage: item.id)
                }
            }

            if totalPages > 1 {
                Divider()
                    .frame(height: 14)
                    .padding(.horizontal, 2)

                HStack(spacing: 2) {
                    Button(action: { delegate?.candidatePanelDidPage(delta: -1) }) {
                        Image(systemName: "chevron.left")
                            .font(.system(size: 9, weight: .semibold))
                    }
                    .buttonStyle(.plain)
                    .disabled(page <= 1)

                    Text("\(page)/\(totalPages)")
                        .font(.system(size: 10, weight: .medium, design: .monospaced))
                        .foregroundStyle(Color.secondary)

                    Button(action: { delegate?.candidatePanelDidPage(delta: 1) }) {
                        Image(systemName: "chevron.right")
                            .font(.system(size: 9, weight: .semibold))
                    }
                    .buttonStyle(.plain)
                    .disabled(page >= totalPages)
                }
                .padding(.horizontal, 3)
            }

            Divider()
                .frame(height: 14)
                .padding(.horizontal, 2)

            Button(action: { delegate?.candidatePanelDidOpenSettings() }) {
                Image(systemName: "gearshape")
                    .font(.system(size: 12))
                    .foregroundStyle(Color.secondary)
            }
            .buttonStyle(.plain)
            .padding(.horizontal, 4)
        }
        .padding(.horizontal, 6)
        .padding(.vertical, 4)
    }
}

// MARK: - Candidate Panel Controller

@objc public final class CandidatePanelController: NSObject {
    @objc public static let shared = CandidatePanelController()

    @objc public weak var delegate: CandidatePanelDelegate?

    private var panel: NSPanel?
    private var effectView: NSVisualEffectView?
    private var hostingView: NSHostingView<CandidateBarView>?
    private var items: [CandidateDisplayItem] = []
    private var selectedIndex: Int = 0
    private var page: Int = 1
    private var totalPages: Int = 1
    private var lastAnchor: NSPoint?

    private override init() {
        super.init()
    }

    @objc(showCandidates:reverseCodes:isPinned:selectedIndex:page:totalPages:atPoint:)
    public func showCandidates(_ candidates: [String],
                               reverseCodes: [String],
                               isPinned: [NSNumber],
                               selectedIndex: Int,
                               page: Int,
                               totalPages: Int,
                               at point: CGPoint) {
        guard !candidates.isEmpty else {
            hidePanel()
            return
        }

        var newItems: [CandidateDisplayItem] = []
        for i in 0..<candidates.count {
            let rev = (i < reverseCodes.count) ? reverseCodes[i] : ""
            let pin = (i < isPinned.count) ? isPinned[i].boolValue : false
            newItems.append(CandidateDisplayItem(id: i,
                                                 text: candidates[i],
                                                 reverseLookupCode: rev,
                                                 isPinned: pin))
        }

        self.items = newItems
        self.selectedIndex = min(max(selectedIndex, 0), newItems.count - 1)
        self.page = max(page, 1)
        self.totalPages = max(totalPages, 1)

        rebuildContent()
        reposition(at: point)
        panel?.orderFrontRegardless()
    }

    @objc(updateSelection:page:totalPages:)
    public func updateSelection(_ index: Int, page: Int, totalPages: Int) {
        guard !items.isEmpty else { return }
        self.selectedIndex = min(max(index, 0), items.count - 1)
        self.page = max(page, 1)
        self.totalPages = max(totalPages, 1)
        hostingView?.rootView = CandidateBarView(items: items,
                                                 selectedIndex: selectedIndex,
                                                 page: self.page,
                                                 totalPages: self.totalPages,
                                                 delegate: delegate)
        resizeToFit()
    }

    @objc public func hidePanel() {
        panel?.orderOut(nil)
        items = []
        selectedIndex = 0
    }

    private func ensurePanel() -> NSPanel {
        if let panel { return panel }

        let newPanel = NSPanel(contentRect: NSRect(x: 0, y: 0, width: 220, height: 36),
                               styleMask: [.borderless, .nonactivatingPanel],
                               backing: .buffered,
                               defer: false)
        newPanel.isOpaque = false
        newPanel.backgroundColor = .clear
        newPanel.hasShadow = true
        newPanel.level = .popUpMenu
        newPanel.isMovable = false
        newPanel.hidesOnDeactivate = false
        newPanel.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .ignoresCycle]
        newPanel.animationBehavior = .utilityWindow

        let effect = NSVisualEffectView(frame: newPanel.contentLayoutRect)
        effect.material = .popover
        effect.blendingMode = .behindWindow
        effect.state = .active
        effect.wantsLayer = true
        effect.layer?.cornerRadius = 9
        effect.layer?.cornerCurve = .continuous
        effect.layer?.masksToBounds = true
        effect.layer?.borderWidth = 1
        effect.layer?.borderColor = NSColor.separatorColor.withAlphaComponent(0.4).cgColor
        effect.autoresizingMask = [.width, .height]

        let hosting = NSHostingView(rootView: CandidateBarView(items: [],
                                                               selectedIndex: 0,
                                                               page: 1,
                                                               totalPages: 1,
                                                               delegate: delegate))
        hosting.translatesAutoresizingMaskIntoConstraints = false
        hosting.setContentHuggingPriority(.required, for: .horizontal)
        hosting.setContentHuggingPriority(.required, for: .vertical)
        hosting.wantsLayer = true
        hosting.layer?.backgroundColor = NSColor.clear.cgColor

        effect.addSubview(hosting)
        NSLayoutConstraint.activate([
            hosting.leadingAnchor.constraint(equalTo: effect.leadingAnchor),
            hosting.trailingAnchor.constraint(equalTo: effect.trailingAnchor),
            hosting.topAnchor.constraint(equalTo: effect.topAnchor),
            hosting.bottomAnchor.constraint(equalTo: effect.bottomAnchor),
        ])

        newPanel.contentView = effect

        panel = newPanel
        effectView = effect
        hostingView = hosting
        return newPanel
    }

    private func rebuildContent() {
        _ = ensurePanel()
        hostingView?.rootView = CandidateBarView(items: items,
                                                 selectedIndex: selectedIndex,
                                                 page: page,
                                                 totalPages: totalPages,
                                                 delegate: delegate)
        resizeToFit()
    }

    private func resizeToFit() {
        guard let panel, let effect = effectView, let hosting = hostingView else { return }
        hosting.layoutSubtreeIfNeeded()
        var fitting = hosting.fittingSize
        if fitting.width <= 1 || fitting.height <= 1 {
            let estimated = items.reduce(CGFloat(0)) { acc, item in
                acc + CGFloat(item.text.count) * 16 + (item.reverseLookupCode.isEmpty ? 32 : 60)
            } + 60
            fitting = CGSize(width: max(100, estimated), height: 36)
        }
        let size = NSSize(width: ceil(fitting.width), height: max(36, ceil(fitting.height)))
        panel.setContentSize(size)
        effect.frame = NSRect(origin: .zero, size: size)

        if let screenPoint = lastAnchor {
            panel.setFrameTopLeftPoint(screenPoint)
        }
    }

    private func reposition(at point: CGPoint) {
        guard let panel else { return }
        let size = panel.frame.size
        let visible = NSScreen.screens.first { $0.frame.contains(point) }?.visibleFrame
            ?? NSScreen.main?.visibleFrame
            ?? NSRect(x: 0, y: 0, width: 1440, height: 900)

        var topLeft = NSPoint(x: point.x, y: point.y - 4)
        if topLeft.y - size.height < visible.minY {
            topLeft.y = point.y + size.height + 4
        }
        if topLeft.x + size.width > visible.maxX {
            topLeft.x = visible.maxX - size.width
        }

        lastAnchor = topLeft
        panel.setFrameTopLeftPoint(topLeft)
    }
}

// MARK: - Native SwiftUI Settings View

struct SettingsMainView: View {
    weak var dataSource: SettingsDataSource?

    @State private var selectedTab: Int = 0
    @State private var primaryScheme: String = "wubi"
    @State private var shuangpinLayout: String = "natural"
    @State private var mixedInputEnabled: Bool = true
    @State private var fallbackThreshold: Int = 3
    @State private var pageSize: Int = 5
    @State private var frequencyEnabled: Bool = true
    @State private var learningEnabled: Bool = true

    // Wordbook
    @State private var userEntries: [[String: String]] = []
    @State private var newText: String = ""
    @State private var newScheme: String = "wubi"
    @State private var newCode: String = ""
    @State private var searchKeyword: String = ""

    // Maintenance
    @State private var stats: [String: Int] = [:]
    @State private var statusMessage: String = ""

    var body: some View {
        TabView(selection: $selectedTab) {
            inputSettingsTab
                .tabItem {
                    Label("输入设置", systemImage: "keyboard")
                }
                .tag(0)

            wordbookTab
                .tabItem {
                    Label("词库管理", systemImage: "book")
                }
                .tag(1)

            maintenanceTab
                .tabItem {
                    Label("系统与维护", systemImage: "wrench.and.screwdriver")
                }
                .tag(2)
        }
        .padding(16)
        .frame(width: 540, height: 420)
        .onAppear {
            loadInitialSettings()
        }
    }

    private var inputSettingsTab: some View {
        Form {
            Section(header: Text("输入方式与方案").font(.headline)) {
                Picker("主输入方案", selection: $primaryScheme) {
                    Text("86 五笔").tag("wubi")
                    Text("全拼").tag("pinyin")
                    Text("双拼").tag("shuangpin")
                }
                .pickerStyle(.segmented)
                .onChange(of: primaryScheme) { newValue in
                    dataSource?.setPrimaryScheme(newValue)
                }

                if primaryScheme == "shuangpin" {
                    Picker("双拼方案", selection: $shuangpinLayout) {
                        Text("自然码").tag("natural")
                        Text("微软双拼").tag("ms")
                        Text("小鹤双拼").tag("xiaohe")
                    }
                    .onChange(of: shuangpinLayout) { newValue in
                        dataSource?.setShuangpinLayout(newValue)
                    }
                }
            }

            Section(header: Text("瀑布流混合输入与智能反查").font(.headline)) {
                Toggle("开启智能瀑布流混输 (五笔精确词不足时自动拼音补全)", isOn: $mixedInputEnabled)
                    .onChange(of: mixedInputEnabled) { newValue in
                        dataSource?.setMixedInputEnabled(newValue)
                    }

                Stepper(value: $fallbackThreshold, in: 0...9) {
                    Text("次级补全阈值: \(fallbackThreshold) 个候选词")
                }
                .onChange(of: fallbackThreshold) { newValue in
                    dataSource?.setFallbackThreshold(newValue)
                }

                Stepper(value: $pageSize, in: 5...9) {
                    Text("每页候选词数: \(pageSize) 个")
                }
                .onChange(of: pageSize) { newValue in
                    dataSource?.setPageSize(newValue)
                }
            }

            Section(header: Text("习惯记忆与学习").font(.headline)) {
                Toggle("启用动态词频调整 (根据使用频率自动优化位次)", isOn: $frequencyEnabled)
                    .onChange(of: frequencyEnabled) { newValue in
                        dataSource?.setFrequencyEnabled(newValue)
                    }

                Toggle("记录输入学习习惯", isOn: $learningEnabled)
                    .onChange(of: learningEnabled) { newValue in
                        dataSource?.setLearningEnabled(newValue)
                    }
            }
        }
    }

    private var wordbookTab: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                TextField("搜索词条 / 编码", text: $searchKeyword)
                    .textFieldStyle(.roundedBorder)

                Button("刷新") {
                    refreshWordbook()
                }
            }

            let filtered = userEntries.filter { entry in
                if searchKeyword.isEmpty { return true }
                let txt = entry["text"] ?? ""
                let cd = entry["code"] ?? ""
                return txt.localizedCaseInsensitiveContains(searchKeyword) ||
                       cd.localizedCaseInsensitiveContains(searchKeyword)
            }

            List(filtered, id: \.self) { entry in
                HStack {
                    Text(entry["text"] ?? "")
                        .font(.system(size: 14, weight: .medium))
                    Spacer()
                    Text("[\(entry["scheme"] ?? "")] \(entry["code"] ?? "")")
                        .font(.system(size: 12, design: .monospaced))
                        .foregroundStyle(Color.secondary)

                    Button(action: {
                        if let id = entry["id"], dataSource?.deleteUserEntry(id: id) == true {
                            refreshWordbook()
                        }
                    }) {
                        Image(systemName: "trash")
                            .foregroundStyle(Color.red)
                    }
                    .buttonStyle(.plain)
                }
                .padding(.vertical, 2)
            }
            .frame(maxHeight: 180)

            Divider()

            Text("添加自定义词条").font(.subheadline).bold()
            HStack(spacing: 8) {
                TextField("词条文本", text: $newText)
                    .textFieldStyle(.roundedBorder)
                Picker("", selection: $newScheme) {
                    Text("五笔").tag("wubi")
                    Text("拼音").tag("pinyin")
                    Text("短语").tag("custom")
                }
                .frame(width: 80)
                TextField("编码", text: $newCode)
                    .textFieldStyle(.roundedBorder)
                    .frame(width: 90)
                Button("添加") {
                    guard !newText.isEmpty && !newCode.isEmpty else { return }
                    _ = dataSource?.addUserEntry(text: newText, scheme: newScheme, code: newCode)
                    newText = ""
                    newCode = ""
                    refreshWordbook()
                }
            }
        }
    }

    private var maintenanceTab: some View {
        VStack(alignment: .leading, spacing: 14) {
            GroupBox(label: Text("词库统计").bold()) {
                VStack(alignment: .leading, spacing: 6) {
                    HStack {
                        Text("五笔基础词库:")
                        Spacer()
                        Text("\(stats["wubi"] ?? 0) 词条")
                            .font(.system(size: 13, design: .monospaced))
                    }
                    HStack {
                        Text("拼音基础词库:")
                        Spacer()
                        Text("\(stats["pinyin"] ?? 0) 词条")
                            .font(.system(size: 13, design: .monospaced))
                    }
                    HStack {
                        Text("用户个性词库:")
                        Spacer()
                        Text("\(stats["user"] ?? 0) 词条")
                            .font(.system(size: 13, design: .monospaced))
                    }
                }
                .padding(6)
            }

            GroupBox(label: Text("数据备份与恢复").bold()) {
                VStack(alignment: .leading, spacing: 10) {
                    HStack {
                        Button("导出数据备份 (JSON)...") {
                            exportBackup()
                        }
                        Button("导入数据备份 (JSON)...") {
                            importBackup()
                        }
                    }

                    Divider()

                    HStack {
                        Button("清空并重置输入学习习惯", role: .destructive) {
                            if dataSource?.resetLearningStats() == true {
                                statusMessage = "已成功重置输入学习习惯！"
                                loadInitialSettings()
                            }
                        }
                    }
                }
                .padding(6)
            }

            if !statusMessage.isEmpty {
                Text(statusMessage)
                    .font(.footnote)
                    .foregroundStyle(Color.accentColor)
            }

            Spacer()
        }
    }

    private func loadInitialSettings() {
        guard let ds = dataSource else { return }
        primaryScheme = ds.getPrimaryScheme()
        shuangpinLayout = ds.getShuangpinLayout()
        mixedInputEnabled = ds.getMixedInputEnabled()
        fallbackThreshold = ds.getFallbackThreshold()
        pageSize = ds.getPageSize()
        frequencyEnabled = ds.getFrequencyEnabled()
        learningEnabled = ds.getLearningEnabled()
        stats = ds.getStatistics()
        userEntries = ds.allUserEntries()
    }

    private func refreshWordbook() {
        guard let ds = dataSource else { return }
        userEntries = ds.allUserEntries()
        stats = ds.getStatistics()
    }

    private func exportBackup() {
        let panel = NSSavePanel()
        panel.allowedContentTypes = [.json]
        panel.nameFieldStringValue = "personal_ime_backup.json"
        if panel.runModal() == .OK, let url = panel.url {
            if let err = dataSource?.exportData(to: url.path) {
                statusMessage = "导出失败: \(err)"
            } else {
                statusMessage = "成功导出备份至: \(url.lastPathComponent)"
            }
        }
    }

    private func importBackup() {
        let panel = NSOpenPanel()
        panel.allowedContentTypes = [.json]
        panel.allowsMultipleSelection = false
        if panel.runModal() == .OK, let url = panel.url {
            if let err = dataSource?.importData(from: url.path, replace: false) {
                statusMessage = "导入失败: \(err)"
            } else {
                statusMessage = "成功从 \(url.lastPathComponent) 导入词库！"
                refreshWordbook()
            }
        }
    }
}

// MARK: - Settings Window Controller

@objc public final class SettingsWindowController: NSObject {
    @objc public static let shared = SettingsWindowController()

    @objc public weak var dataSource: SettingsDataSource?

    private var window: NSWindow?

    private override init() {
        super.init()
    }

    @objc public func show() {
        if let window {
            window.makeKeyAndOrderFront(nil)
            NSApp.activate(ignoringOtherApps: true)
            return
        }

        let newWindow = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 540, height: 440),
                                 styleMask: [.titled, .closable, .miniaturizable],
                                 backing: .buffered,
                                 defer: false)
        newWindow.title = "人人输入法 设置"
        newWindow.center()
        newWindow.isReleasedWhenClosed = false
        newWindow.level = .floating

        let hosting = NSHostingView(rootView: SettingsMainView(dataSource: dataSource))
        newWindow.contentView = hosting

        self.window = newWindow
        newWindow.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
    }
}
