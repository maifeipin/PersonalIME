import Cocoa
import SwiftUI

/// Horizontal candidate bar styled after the native macOS input method panel:
/// rounded corners, translucent material, selection pill and per-item index badge.
struct CandidateBarView: View {
    let candidates: [String]
    let selectedIndex: Int

    var body: some View {
        HStack(spacing: 1) {
            ForEach(Array(candidates.enumerated()), id: \.offset) { index, text in
                HStack(spacing: 4) {
                    if index < 9 {
                        Text("\(index + 1)")
                            .font(.system(size: 10, weight: .medium, design: .rounded))
                            .foregroundStyle(index == selectedIndex ? .white : .secondary)
                    }
                    Text(text)
                        .font(.system(size: 15, weight: index == selectedIndex ? .semibold : .regular))
                        .foregroundStyle(index == selectedIndex ? .white : .primary)
                        .lineLimit(1)
                }
                .padding(.horizontal, 8)
                .padding(.vertical, 4)
                .background(
                    RoundedRectangle(cornerRadius: 6, style: .continuous)
                        .fill(index == selectedIndex
                              ? Color.accentColor.opacity(0.9)
                              : Color.clear)
                )
                .contentShape(RoundedRectangle(cornerRadius: 6, style: .continuous))
            }
        }
        .padding(.horizontal, 6)
        .padding(.vertical, 5)
    }
}

/// Owns the borderless floating panel that hosts the SwiftUI candidate bar.
/// Exposed to Objective-C++ through explicitly named selectors.
@objc public final class CandidatePanelController: NSObject {
    @objc public static let shared = CandidatePanelController()

    private var panel: NSPanel?
    private var effectView: NSVisualEffectView?
    private var hostingView: NSHostingView<CandidateBarView>?
    private var candidates: [String] = []
    private var selectedIndex: Int = 0

    private override init() {
        super.init()
    }

    @objc(showCandidates:selectedIndex:atPoint:)
    public func showCandidates(_ candidates: [String],
                               selectedIndex: Int,
                               at point: CGPoint) {
        guard !candidates.isEmpty else {
            hidePanel()
            return
        }
        self.candidates = candidates
        self.selectedIndex = min(max(selectedIndex, 0), candidates.count - 1)
        rebuildContent()
        reposition(at: point)
        panel?.orderFrontRegardless()
    }

    @objc public func updateSelection(_ index: Int) {
        guard !candidates.isEmpty else { return }
        let clamped = min(max(index, 0), candidates.count - 1)
        guard clamped != selectedIndex else { return }
        selectedIndex = clamped
        hostingView?.rootView = CandidateBarView(candidates: candidates,
                                                 selectedIndex: selectedIndex)
        resizeToFit()
    }

    @objc public func hidePanel() {
        panel?.orderOut(nil)
        candidates = []
        selectedIndex = 0
    }

    // MARK: - Panel construction

    private func ensurePanel() -> NSPanel {
        if let panel { return panel }

        let newPanel = NSPanel(contentRect: NSRect(x: 0, y: 0, width: 200, height: 34),
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
        effect.layer?.cornerRadius = 10
        effect.layer?.cornerCurve = .continuous
        effect.layer?.masksToBounds = true
        effect.layer?.borderWidth = 1
        effect.layer?.borderColor = NSColor.separatorColor.withAlphaComponent(0.5).cgColor
        effect.autoresizingMask = [.width, .height]

        let hosting = NSHostingView(rootView: CandidateBarView(candidates: [],
                                                               selectedIndex: 0))
        hosting.translatesAutoresizingMaskIntoConstraints = false
        hosting.setContentHuggingPriority(.required, for: .horizontal)
        hosting.setContentHuggingPriority(.required, for: .vertical)
        // The SwiftUI bar must never paint its own opaque background over the material.
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
        hostingView?.rootView = CandidateBarView(candidates: candidates,
                                                 selectedIndex: selectedIndex)
        resizeToFit()
    }

    private func resizeToFit() {
        guard let panel, let effect = effectView, let hosting = hostingView else { return }
        hosting.layoutSubtreeIfNeeded()
        var fitting = hosting.fittingSize
        if fitting.width <= 1 || fitting.height <= 1 {
            // Fall back to a text-width estimate when SwiftUI has not laid out yet.
            let estimated = candidates.enumerated().reduce(CGFloat(0)) { acc, pair in
                acc + CGFloat(pair.element.count) * 16 + 34
            } + 14
            fitting = CGSize(width: max(80, estimated), height: 34)
        }
        let size = NSSize(width: ceil(fitting.width), height: max(34, ceil(fitting.height)))
        panel.setContentSize(size)
        effect.frame = NSRect(origin: .zero, size: size)
        
        // Preserve the top-left anchor while the panel grows or shrinks.
        if let screenPoint = lastAnchor {
            panel.setFrameTopLeftPoint(screenPoint)
        }
    }

    private var lastAnchor: NSPoint?

    private func reposition(at point: CGPoint) {
        guard let panel else { return }
        // Place the panel just below the caret, flipping to above when it would
        // overflow the bottom of the visible frame.
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
