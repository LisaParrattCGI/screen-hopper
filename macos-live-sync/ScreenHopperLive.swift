import AppKit
import CoreGraphics
import Foundation
import IOKit.hid
import ServiceManagement

private enum FeatureReportMode {
    case bodyOnly
    case reportIDPrefixed
}

private struct Options {
    var activeScreen: Int8 = -1
    var pointerResolution: Double = 400.0
    var frameRate: Double = 67.0
    var fixedMultiplier: Double = 1.0
    var placementTolerance: Double = 0.5
    var pollInterval: TimeInterval = 1.0
}

private struct MouseConfig: Equatable {
    var trackingSpeed: Double
    var pointerResolution: Double
    var frameRate: Double
    var fixedMultiplier: Double
    var placementTolerance: Double

    var fixedValues: [UInt32] {
        [
            fixed16(trackingSpeed),
            fixed16(pointerResolution),
            fixed16(frameRate),
            fixed16(fixedMultiplier),
            fixed16(placementTolerance),
        ]
    }

    static func == (lhs: MouseConfig, rhs: MouseConfig) -> Bool {
        lhs.fixedValues == rhs.fixedValues
    }
}

private struct ScreenConfig: Equatable {
    var x: UInt32
    var y: UInt32
    var width: UInt32
    var height: UInt32
    var sensitivity: UInt32
}

private struct MappingConfig: Equatable {
    var targetUsage: UInt32
    var sourceUsage: UInt32
    var scaling: Int32
    var layer: UInt8
    var sticky: Bool
}

private struct PersistentConfig: Equatable {
    var unmappedPassthrough: Bool
    var partialScrollTimeout: UInt32
    var intervalOverride: UInt8
    var constraintMode: UInt8
    var offscreenSensitivity: UInt32
    var cursorPlacementIntervalSeconds: UInt32
    var mouse: MouseConfig
    var screens: [ScreenConfig]
    var mappings: [MappingConfig]
}

private struct RuntimeCursor: Equatable {
    var x: Int64
    var y: Int64
    var activeScreen: Int8
}

private final class CRC32 {
    private static let table: [UInt32] = (0..<256).map { value in
        var crc = UInt32(value)
        for _ in 0..<8 {
            if (crc & 1) != 0 {
                crc = 0xEDB88320 ^ (crc >> 1)
            } else {
                crc >>= 1
            }
        }
        return crc
    }

    static func compute(_ data: Data) -> UInt32 {
        var crc: UInt32 = 0xFFFF_FFFF
        for byte in data {
            let index = Int((crc ^ UInt32(byte)) & 0xFF)
            crc = table[index] ^ (crc >> 8)
        }
        return crc ^ 0xFFFF_FFFF
    }
}

private final class ScreenHopperDevice {
    private let device: IOHIDDevice
    private let mode: FeatureReportMode

    init(device: IOHIDDevice, mode: FeatureReportMode) {
        self.device = device
        self.mode = mode
    }

    func requestStatus() throws {
        try sendRuntimeCommand(.getStatus)
    }

    func readStatusPayload() throws -> Data {
        try readFeatureReport(reportID: runtimeReportID, size: runtimeSize)
    }

    func sendMouseConfig(_ config: MouseConfig) throws {
        var payload = Data()
        for value in config.fixedValues {
            payload.appendUInt32LE(value)
        }
        try sendRuntimeCommand(.setMouseConfig, payload: payload)
    }

    func sendCursor(_ cursor: RuntimeCursor) throws {
        var payload = Data()
        payload.appendInt64LE(cursor.x)
        payload.appendInt64LE(cursor.y)
        payload.appendUInt8(UInt8(bitPattern: cursor.activeScreen))
        try sendRuntimeCommand(.setHostCursor, payload: payload)
    }

    func fetchPersistentConfig() throws -> PersistentConfig {
        try sendConfigCommand(.getConfig)
        let payload = try readFeatureReport(reportID: configReportID, size: configSize)

        guard payload[0] == configVersion else {
            throw LiveSyncError.incompatibleConfigVersion(payload[0])
        }

        let mappingCount = payload.readUInt32LE(at: 6)
        var screens: [ScreenConfig] = []
        var mappings: [MappingConfig] = []

        for index in 0..<screenCount {
            var screenPayload = Data()
            screenPayload.appendUInt32LE(UInt32(index))
            try sendConfigCommand(.getScreen, payload: screenPayload)
            let screenData = try readFeatureReport(reportID: configReportID, size: configSize)
            screens.append(
                ScreenConfig(
                    x: screenData.readUInt32LE(at: 0),
                    y: screenData.readUInt32LE(at: 4),
                    width: screenData.readUInt32LE(at: 8),
                    height: screenData.readUInt32LE(at: 12),
                    sensitivity: screenData.readUInt32LE(at: 16)
                )
            )
        }

        for index in 0..<mappingCount {
            var mappingPayload = Data()
            mappingPayload.appendUInt32LE(index)
            try sendConfigCommand(.getMapping, payload: mappingPayload)
            let mappingData = try readFeatureReport(reportID: configReportID, size: configSize)
            mappings.append(
                MappingConfig(
                    targetUsage: mappingData.readUInt32LE(at: 0),
                    sourceUsage: mappingData.readUInt32LE(at: 4),
                    scaling: mappingData.readInt32LE(at: 8),
                    layer: mappingData[12],
                    sticky: (mappingData[13] & stickyMappingFlag) != 0
                )
            )
        }

        return PersistentConfig(
            unmappedPassthrough: (payload[1] & unmappedPassthroughFlag) != 0,
            partialScrollTimeout: payload.readUInt32LE(at: 2),
            intervalOverride: payload[18],
            constraintMode: payload[19],
            offscreenSensitivity: payload.readUInt32LE(at: 20),
            cursorPlacementIntervalSeconds: payload.readUInt32LE(at: 24),
            mouse: MouseConfig(
                trackingSpeed: doubleFromFixed16(payload.readUInt32LE(at: 28)),
                pointerResolution: doubleFromFixed16(payload.readUInt32LE(at: 32)),
                frameRate: doubleFromFixed16(payload.readUInt32LE(at: 36)),
                fixedMultiplier: doubleFromFixed16(payload.readUInt32LE(at: 40)),
                placementTolerance: doubleFromFixed16(payload.readUInt32LE(at: 44))
            ),
            screens: screens,
            mappings: mappings
        )
    }

    func savePersistentConfig(_ config: PersistentConfig) throws {
        var suspended = false
        do {
            try sendConfigCommand(.suspend)
            suspended = true

            var configPayload = Data()
            configPayload.appendUInt8(config.unmappedPassthrough ? unmappedPassthroughFlag : 0)
            configPayload.appendUInt32LE(config.partialScrollTimeout)
            configPayload.appendUInt8(config.intervalOverride)
            configPayload.appendUInt8(config.constraintMode)
            configPayload.appendUInt32LE(config.offscreenSensitivity)
            configPayload.appendUInt32LE(config.cursorPlacementIntervalSeconds)
            for value in config.mouse.fixedValues {
                configPayload.appendUInt32LE(value)
            }
            try sendConfigCommand(.setConfig, payload: configPayload)

            for (index, screen) in config.screens.enumerated() {
                var screenPayload = Data()
                screenPayload.appendUInt8(UInt8(index))
                screenPayload.appendUInt32LE(screen.x)
                screenPayload.appendUInt32LE(screen.y)
                screenPayload.appendUInt32LE(screen.width)
                screenPayload.appendUInt32LE(screen.height)
                screenPayload.appendUInt32LE(screen.sensitivity)
                try sendConfigCommand(.setScreen, payload: screenPayload)
            }

            try sendConfigCommand(.clearMapping)
            for mapping in config.mappings {
                var mappingPayload = Data()
                mappingPayload.appendUInt32LE(mapping.targetUsage)
                mappingPayload.appendUInt32LE(mapping.sourceUsage)
                mappingPayload.appendInt32LE(mapping.scaling)
                mappingPayload.appendUInt8(mapping.layer)
                mappingPayload.appendUInt8(mapping.sticky ? stickyMappingFlag : 0)
                try sendConfigCommand(.addMapping, payload: mappingPayload)
            }

            try sendConfigCommand(.persistConfig)
            try sendConfigCommand(.resume)
            suspended = false
        } catch {
            if suspended {
                try? sendConfigCommand(.resume)
            }
            throw error
        }
    }

    private func sendRuntimeCommand(_ command: RuntimeCommand, payload: Data = Data()) throws {
        try sendFeatureReport(reportID: runtimeReportID, size: runtimeSize, command: command.rawValue, payload: payload)
    }

    private func sendConfigCommand(_ command: ConfigCommand, payload: Data = Data()) throws {
        try sendFeatureReport(reportID: configReportID, size: configSize, command: command.rawValue, payload: payload)
    }

    private func sendFeatureReport(reportID: CFIndex, size: Int, command: UInt8, payload: Data = Data()) throws {
        var body = Data()
        body.appendUInt8(configVersion)
        body.appendUInt8(command)
        body.append(payload)

        if body.count > size - 4 {
            throw LiveSyncError.payloadTooLarge
        }

        body.append(contentsOf: repeatElement(UInt8(0), count: size - 4 - body.count))
        body.appendUInt32LE(CRC32.compute(body))

        var report = Data()
        if mode == .reportIDPrefixed {
            report.appendUInt8(UInt8(reportID))
        }
        report.append(body)

        let result = report.withUnsafeBytes { bytes -> IOReturn in
            guard let pointer = bytes.bindMemory(to: UInt8.self).baseAddress else {
                return kIOReturnNoMemory
            }
            return IOHIDDeviceSetReport(device, kIOHIDReportTypeFeature, reportID, pointer, report.count)
        }

        guard result == kIOReturnSuccess else {
            throw LiveSyncError.hidSetReportFailed(result)
        }
    }

    private func readFeatureReport(reportID: CFIndex, size: Int) throws -> Data {
        let length = mode == .reportIDPrefixed ? size + 1 : size
        var report = Data(repeating: 0, count: length)
        var reportLength = report.count

        let result = report.withUnsafeMutableBytes { bytes -> IOReturn in
            guard let pointer = bytes.bindMemory(to: UInt8.self).baseAddress else {
                return kIOReturnNoMemory
            }
            return IOHIDDeviceGetReport(device, kIOHIDReportTypeFeature, reportID, pointer, &reportLength)
        }

        guard result == kIOReturnSuccess else {
            throw LiveSyncError.hidGetReportFailed(result)
        }

        report = report.prefix(reportLength)
        let body: Data
        switch mode {
        case .bodyOnly:
            body = report
        case .reportIDPrefixed:
            guard report.first == UInt8(reportID) else {
                throw LiveSyncError.invalidReport
            }
            body = report.dropFirst()
        }

        guard body.count >= size else {
            throw LiveSyncError.invalidReport
        }

        let payload = body.prefix(size - 4)
        let expected = body.readUInt32LE(at: size - 4)
        guard CRC32.compute(payload) == expected else {
            throw LiveSyncError.invalidCRC
        }

        return payload
    }
}

private final class DeviceLocator {
    private let manager: IOHIDManager

    init() {
        manager = IOHIDManagerCreate(kCFAllocatorDefault, IOOptionBits(kIOHIDOptionsTypeNone))
        let match: [String: Any] = [
            kIOHIDVendorIDKey as String: vendorID,
            kIOHIDProductIDKey as String: productID,
        ]
        IOHIDManagerSetDeviceMatching(manager, match as CFDictionary)
        IOHIDManagerOpen(manager, IOOptionBits(kIOHIDOptionsTypeNone))
    }

    func findRuntimeDevice() -> ScreenHopperDevice? {
        guard let devices = IOHIDManagerCopyDevices(manager) as? Set<IOHIDDevice> else {
            return nil
        }

        for device in devices {
            if IOHIDDeviceOpen(device, IOOptionBits(kIOHIDOptionsTypeNone)) != kIOReturnSuccess {
                continue
            }

            for mode in [FeatureReportMode.bodyOnly, FeatureReportMode.reportIDPrefixed] {
                let candidate = ScreenHopperDevice(device: device, mode: mode)
                do {
                    try probe(candidate, device: device, mode: mode)
                    return candidate
                } catch {
                    continue
                }
            }
        }

        return nil
    }

    private func probe(_ candidate: ScreenHopperDevice, device: IOHIDDevice, mode: FeatureReportMode) throws {
        try candidate.requestStatus()

        let length = mode == .reportIDPrefixed ? runtimeSize + 1 : runtimeSize
        var report = Data(repeating: 0, count: length)
        var reportLength = report.count

        let result = report.withUnsafeMutableBytes { bytes -> IOReturn in
            guard let pointer = bytes.bindMemory(to: UInt8.self).baseAddress else {
                return kIOReturnNoMemory
            }
            return IOHIDDeviceGetReport(device, kIOHIDReportTypeFeature, runtimeReportID, pointer, &reportLength)
        }

        guard result == kIOReturnSuccess else {
            throw LiveSyncError.hidGetReportFailed(result)
        }

        report = report.prefix(reportLength)
        let body: Data
        switch mode {
        case .bodyOnly:
            body = report
        case .reportIDPrefixed:
            guard report.first == UInt8(runtimeReportID) else {
                throw LiveSyncError.invalidReport
            }
            body = report.dropFirst()
        }

        guard body.count >= runtimeSize else {
            throw LiveSyncError.invalidReport
        }

        let payload = body.prefix(runtimeSize - 4)
        let expected = body.readUInt32LE(at: runtimeSize - 4)
        guard CRC32.compute(payload) == expected else {
            throw LiveSyncError.invalidCRC
        }
    }
}

private final class MouseConfigReader {
    private let mouseManager: IOHIDManager
    private let options: Options

    init(options: Options) {
        self.options = options
        mouseManager = IOHIDManagerCreate(kCFAllocatorDefault, IOOptionBits(kIOHIDOptionsTypeNone))
        let match: [String: Any] = [
            kIOHIDDeviceUsagePageKey as String: kHIDPage_GenericDesktop,
            kIOHIDDeviceUsageKey as String: kHIDUsage_GD_Mouse,
        ]
        IOHIDManagerSetDeviceMatching(mouseManager, match as CFDictionary)
        IOHIDManagerOpen(mouseManager, IOOptionBits(kIOHIDOptionsTypeNone))
    }

    func currentConfig() -> MouseConfig {
        MouseConfig(
            trackingSpeed: readTrackingSpeed() ?? 0.6875,
            pointerResolution: readPointerResolution() ?? options.pointerResolution,
            frameRate: options.frameRate,
            fixedMultiplier: options.fixedMultiplier,
            placementTolerance: options.placementTolerance
        )
    }

    private func readTrackingSpeed() -> Double? {
        CFPreferencesAppSynchronize(kCFPreferencesAnyApplication)
        guard let raw = CFPreferencesCopyValue(
            "com.apple.mouse.scaling" as CFString,
            kCFPreferencesAnyApplication,
            kCFPreferencesCurrentUser,
            kCFPreferencesAnyHost
        ) else {
            return nil
        }

        if let number = raw as? NSNumber {
            return number.doubleValue
        }
        if let string = raw as? String {
            return Double(string)
        }
        return nil
    }

    private func readPointerResolution() -> Double? {
        guard let devices = IOHIDManagerCopyDevices(mouseManager) as? Set<IOHIDDevice> else {
            return nil
        }

        for device in devices {
            guard let property = IOHIDDeviceGetProperty(device, "HIDPointerResolution" as CFString) else {
                continue
            }

            if let number = property as? NSNumber {
                let value = number.doubleValue
                return value > 4096.0 ? value / fixed16Scale : value
            }
        }

        return nil
    }
}

private final class ScreenLayoutView: NSView {
    var screens: [ScreenConfig] = [] {
        didSet {
            needsDisplay = true
        }
    }

    var selectedIndex: Int = 0 {
        didSet {
            needsDisplay = true
        }
    }

    var onChange: (([ScreenConfig]) -> Void)?

    private var dragPoint: NSPoint?

    override var isFlipped: Bool {
        true
    }

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        wantsLayer = true
        layer?.cornerRadius = 12
        layer?.masksToBounds = true
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    override func draw(_ dirtyRect: NSRect) {
        NSColor.controlBackgroundColor.setFill()
        bounds.fill()

        drawGrid()

        for index in screens.indices {
            let rect = viewRect(for: screens[index])
            let path = NSBezierPath(roundedRect: rect, xRadius: 8, yRadius: 8)
            (index == selectedIndex ? NSColor.controlAccentColor : NSColor.secondaryLabelColor).setStroke()
            NSColor.windowBackgroundColor.withAlphaComponent(index == selectedIndex ? 0.92 : 0.72).setFill()
            path.lineWidth = index == selectedIndex ? 2.5 : 1.5
            path.fill()
            path.stroke()

            let label = "Screen \(index)"
            let attrs: [NSAttributedString.Key: Any] = [
                .font: NSFont.systemFont(ofSize: 13, weight: .semibold),
                .foregroundColor: NSColor.labelColor,
            ]
            let size = label.size(withAttributes: attrs)
            label.draw(
                at: NSPoint(x: rect.midX - size.width / 2, y: rect.midY - size.height / 2),
                withAttributes: attrs
            )
        }
    }

    override func mouseDown(with event: NSEvent) {
        let point = convert(event.locationInWindow, from: nil)
        for index in screens.indices.reversed() {
            if viewRect(for: screens[index]).contains(point) {
                selectedIndex = index
                dragPoint = point
                return
            }
        }
        dragPoint = nil
    }

    override func mouseDragged(with event: NSEvent) {
        guard let last = dragPoint, selectedIndex < screens.count else {
            return
        }

        let point = convert(event.locationInWindow, from: nil)
        let scale = layoutScale()
        guard scale > 0 else {
            return
        }

        let dx = Int64(((point.x - last.x) / scale).rounded())
        let dy = Int64(((point.y - last.y) / scale).rounded())
        if dx == 0 && dy == 0 {
            return
        }

        var screen = screens[selectedIndex]
        screen.x = clampedUInt32(Int64(screen.x) + dx)
        screen.y = clampedUInt32(Int64(screen.y) + dy)
        screens[selectedIndex] = screen
        dragPoint = point
        onChange?(screens)
    }

    private func drawGrid() {
        NSColor.separatorColor.withAlphaComponent(0.35).setStroke()
        let path = NSBezierPath()
        path.lineWidth = 1
        let step: CGFloat = 24
        var x: CGFloat = 0
        while x <= bounds.width {
            path.move(to: NSPoint(x: x, y: 0))
            path.line(to: NSPoint(x: x, y: bounds.height))
            x += step
        }
        var y: CGFloat = 0
        while y <= bounds.height {
            path.move(to: NSPoint(x: 0, y: y))
            path.line(to: NSPoint(x: bounds.width, y: y))
            y += step
        }
        path.stroke()
    }

    private func viewRect(for screen: ScreenConfig) -> NSRect {
        let metrics = layoutMetrics()
        return NSRect(
            x: metrics.origin.x + (CGFloat(screen.x) - metrics.minX) * metrics.scale,
            y: metrics.origin.y + (CGFloat(screen.y) - metrics.minY) * metrics.scale,
            width: max(CGFloat(screen.width) * metrics.scale, 22),
            height: max(CGFloat(screen.height) * metrics.scale, 18)
        )
    }

    private func layoutScale() -> CGFloat {
        layoutMetrics().scale
    }

    private func layoutMetrics() -> (origin: NSPoint, minX: CGFloat, minY: CGFloat, scale: CGFloat) {
        guard !screens.isEmpty else {
            return (NSPoint(x: 20, y: 20), 0, 0, 1)
        }

        let minX = screens.map { CGFloat($0.x) }.min() ?? 0
        let minY = screens.map { CGFloat($0.y) }.min() ?? 0
        let maxX = screens.map { CGFloat(Int64($0.x) + Int64($0.width)) }.max() ?? 1
        let maxY = screens.map { CGFloat(Int64($0.y) + Int64($0.height)) }.max() ?? 1
        let contentWidth = max(maxX - minX, 1)
        let contentHeight = max(maxY - minY, 1)
        let padding: CGFloat = 24
        let scale = min((bounds.width - padding * 2) / contentWidth, (bounds.height - padding * 2) / contentHeight)
        let safeScale = max(min(scale, 1.0), 0.05)
        let drawnWidth = contentWidth * safeScale
        let drawnHeight = contentHeight * safeScale
        return (
            NSPoint(x: (bounds.width - drawnWidth) / 2, y: (bounds.height - drawnHeight) / 2),
            minX,
            minY,
            safeScale
        )
    }
}

private struct ScreenFieldSet {
    var x: NSTextField
    var y: NSTextField
    var width: NSTextField
    var height: NSTextField
    var sensitivity: NSTextField
}

private final class ConfigWindowController: NSWindowController, NSWindowDelegate, NSTextFieldDelegate {
    private let device: ScreenHopperDevice
    var onClose: (() -> Void)?

    private var originalConfig: PersistentConfig?
    private var workingConfig: PersistentConfig?
    private var isBusy = false

    private let statusLabel = NSTextField(labelWithString: "Fetching configuration...")
    private let saveButton = NSButton(title: "Save", target: nil, action: nil)
    private let discardButton = NSButton(title: "Discard", target: nil, action: nil)
    private let unmappedCheckbox = NSButton(checkboxWithTitle: "Pass through unmapped usages", target: nil, action: nil)
    private let partialScrollField = NSTextField()
    private let intervalOverrideField = NSTextField()
    private let offscreenSensitivityField = NSTextField()
    private let cursorPlacementField = NSTextField()
    private let constraintControl = NSSegmentedControl(labels: ["None", "Box", "Visible"], trackingMode: .selectOne, target: nil, action: nil)

    private let trackingSpeedField = NSTextField()
    private let trackingSpeedSlider = NSSlider(value: 0.6875, minValue: 0, maxValue: 3, target: nil, action: nil)
    private let pointerResolutionField = NSTextField()
    private let frameRateField = NSTextField()
    private let fixedMultiplierField = NSTextField()
    private let placementToleranceField = NSTextField()

    private let layoutView = ScreenLayoutView()
    private var screenFields: [ScreenFieldSet] = []

    init(device: ScreenHopperDevice) {
        self.device = device

        let window = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 760, height: 680),
            styleMask: [.titled, .closable, .miniaturizable, .resizable],
            backing: .buffered,
            defer: false
        )
        window.title = "Screen Hopper Configuration"
        window.minSize = NSSize(width: 640, height: 560)
        window.center()
        window.isReleasedWhenClosed = false
        super.init(window: window)
        window.delegate = self
        buildUI()
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    func loadConfiguration() {
        setLoading(true, message: "Fetching configuration...")
        DispatchQueue.global(qos: .userInitiated).async {
            do {
                let config = try self.device.fetchPersistentConfig()
                DispatchQueue.main.async {
                    self.originalConfig = config
                    self.workingConfig = config
                    self.populate(from: config)
                    self.setLoading(false, message: "Configuration loaded. Mappings will be preserved unchanged.")
                }
            } catch {
                DispatchQueue.main.async {
                    self.setLoading(false, message: "Could not read configuration: \(error)")
                }
            }
        }
    }

    func windowWillClose(_ notification: Notification) {
        onClose?()
    }

    private func buildUI() {
        guard let contentView = window?.contentView else {
            return
        }

        configureMainFields()

        let root = NSStackView()
        root.orientation = .vertical
        root.spacing = 0
        root.translatesAutoresizingMaskIntoConstraints = false
        contentView.addSubview(root)

        NSLayoutConstraint.activate([
            root.leadingAnchor.constraint(equalTo: contentView.leadingAnchor),
            root.trailingAnchor.constraint(equalTo: contentView.trailingAnchor),
            root.topAnchor.constraint(equalTo: contentView.topAnchor),
            root.bottomAnchor.constraint(equalTo: contentView.bottomAnchor),
        ])

        root.addArrangedSubview(makeHeader())

        let scrollView = NSScrollView()
        scrollView.hasVerticalScroller = true
        scrollView.borderType = .noBorder
        scrollView.drawsBackground = false

        let stack = NSStackView()
        stack.orientation = .vertical
        stack.spacing = 14
        stack.edgeInsets = NSEdgeInsets(top: 18, left: 22, bottom: 18, right: 22)
        stack.translatesAutoresizingMaskIntoConstraints = false
        scrollView.documentView = stack

        NSLayoutConstraint.activate([
            stack.widthAnchor.constraint(equalTo: scrollView.widthAnchor),
        ])

        stack.addArrangedSubview(makeGeneralSection())
        stack.addArrangedSubview(makeScreensSection())
        stack.addArrangedSubview(makeMouseSection())
        root.addArrangedSubview(scrollView)

        root.addArrangedSubview(makeFooter())

        saveButton.target = self
        saveButton.action = #selector(save)
        discardButton.target = self
        discardButton.action = #selector(discard)
        saveButton.isEnabled = false

        let controls: [NSControl] = [
            unmappedCheckbox,
            partialScrollField,
            intervalOverrideField,
            offscreenSensitivityField,
            cursorPlacementField,
            constraintControl,
            trackingSpeedField,
            trackingSpeedSlider,
            pointerResolutionField,
            frameRateField,
            fixedMultiplierField,
            placementToleranceField,
        ]
        for control in controls {
            control.target = self
            control.action = #selector(controlChanged)
        }

        for fieldSet in screenFields {
            for field in [fieldSet.x, fieldSet.y, fieldSet.width, fieldSet.height, fieldSet.sensitivity] {
                field.target = self
                field.action = #selector(controlChanged)
                field.delegate = self
            }
        }

        layoutView.onChange = { [weak self] screens in
            guard let self else {
                return
            }
            self.workingConfig?.screens = screens
            self.populateScreenFields(screens)
            self.markDirty()
        }
    }

    private func configureMainFields() {
        for field in [
            partialScrollField,
            intervalOverrideField,
            offscreenSensitivityField,
            cursorPlacementField,
            pointerResolutionField,
            frameRateField,
            fixedMultiplierField,
            placementToleranceField,
        ] {
            styleNumericField(field, width: 104)
        }

        styleNumericField(trackingSpeedField, width: 84)
        constraintControl.selectedSegment = 0
        unmappedCheckbox.font = NSFont.systemFont(ofSize: 13)
    }

    private func makeHeader() -> NSView {
        let view = NSView()
        view.translatesAutoresizingMaskIntoConstraints = false
        view.heightAnchor.constraint(equalToConstant: 84).isActive = true

        let title = NSTextField(labelWithString: "Screen Hopper")
        title.font = NSFont.systemFont(ofSize: 24, weight: .semibold)
        title.translatesAutoresizingMaskIntoConstraints = false

        let subtitle = NSTextField(labelWithString: "Persistent device configuration")
        subtitle.font = NSFont.systemFont(ofSize: 13)
        subtitle.textColor = .secondaryLabelColor
        subtitle.translatesAutoresizingMaskIntoConstraints = false

        view.addSubview(title)
        view.addSubview(subtitle)

        NSLayoutConstraint.activate([
            title.leadingAnchor.constraint(equalTo: view.leadingAnchor, constant: 24),
            title.topAnchor.constraint(equalTo: view.topAnchor, constant: 18),
            subtitle.leadingAnchor.constraint(equalTo: title.leadingAnchor),
            subtitle.topAnchor.constraint(equalTo: title.bottomAnchor, constant: 4),
        ])

        return view
    }

    private func makeFooter() -> NSView {
        let view = NSView()
        view.translatesAutoresizingMaskIntoConstraints = false
        view.heightAnchor.constraint(equalToConstant: 64).isActive = true

        statusLabel.font = NSFont.systemFont(ofSize: 12)
        statusLabel.textColor = .secondaryLabelColor
        statusLabel.lineBreakMode = .byTruncatingTail
        statusLabel.translatesAutoresizingMaskIntoConstraints = false

        saveButton.bezelStyle = .rounded
        saveButton.keyEquivalent = "\r"
        discardButton.bezelStyle = .rounded
        saveButton.translatesAutoresizingMaskIntoConstraints = false
        discardButton.translatesAutoresizingMaskIntoConstraints = false

        view.addSubview(statusLabel)
        view.addSubview(discardButton)
        view.addSubview(saveButton)

        NSLayoutConstraint.activate([
            statusLabel.leadingAnchor.constraint(equalTo: view.leadingAnchor, constant: 24),
            statusLabel.centerYAnchor.constraint(equalTo: view.centerYAnchor),
            statusLabel.trailingAnchor.constraint(lessThanOrEqualTo: discardButton.leadingAnchor, constant: -16),
            saveButton.trailingAnchor.constraint(equalTo: view.trailingAnchor, constant: -24),
            saveButton.centerYAnchor.constraint(equalTo: view.centerYAnchor),
            discardButton.trailingAnchor.constraint(equalTo: saveButton.leadingAnchor, constant: -8),
            discardButton.centerYAnchor.constraint(equalTo: view.centerYAnchor),
        ])

        return view
    }

    private func makeGeneralSection() -> NSView {
        let stack = formStack()
        stack.addArrangedSubview(formRow("Cursor constraint", constraintControl, help: "How Screen Hopper constrains its internal cursor."))
        stack.addArrangedSubview(formRow("Partial scroll timeout", partialScrollField, suffix: "ms"))
        stack.addArrangedSubview(formRow("Interval override", intervalOverrideField, suffix: "ms"))
        stack.addArrangedSubview(formRow("Offscreen sensitivity", offscreenSensitivityField, suffix: "x"))
        stack.addArrangedSubview(formRow("Placement refresh", cursorPlacementField, suffix: "s"))
        stack.addArrangedSubview(unmappedCheckbox)
        return section(title: "Behavior", subtitle: "Runtime behavior that is saved on the device.", content: stack)
    }

    private func makeScreensSection() -> NSView {
        let outer = NSStackView()
        outer.orientation = .vertical
        outer.spacing = 12

        layoutView.translatesAutoresizingMaskIntoConstraints = false
        layoutView.heightAnchor.constraint(equalToConstant: 210).isActive = true
        outer.addArrangedSubview(layoutView)

        let screensStack = NSStackView()
        screensStack.orientation = .horizontal
        screensStack.spacing = 12
        screensStack.distribution = .fillEqually

        for index in 0..<screenCount {
            let fieldSet = ScreenFieldSet(
                x: numericField(),
                y: numericField(),
                width: numericField(),
                height: numericField(),
                sensitivity: numericField()
            )
            screenFields.append(fieldSet)
            screensStack.addArrangedSubview(screenEditor(index: index, fields: fieldSet))
        }

        outer.addArrangedSubview(screensStack)
        return section(title: "Screens", subtitle: "Drag screens in the preview, or type exact geometry below.", content: outer)
    }

    private func makeMouseSection() -> NSView {
        let stack = formStack()
        trackingSpeedSlider.translatesAutoresizingMaskIntoConstraints = false
        trackingSpeedSlider.widthAnchor.constraint(equalToConstant: 240).isActive = true

        let trackingStack = NSStackView(views: [trackingSpeedField, trackingSpeedSlider])
        trackingStack.orientation = .horizontal
        trackingStack.spacing = 10
        stack.addArrangedSubview(formRow("Tracking speed", trackingStack))
        stack.addArrangedSubview(formRow("Pointer resolution", pointerResolutionField, suffix: "counts/in"))
        stack.addArrangedSubview(formRow("Frame rate", frameRateField, suffix: "Hz"))
        stack.addArrangedSubview(formRow("Fixed multiplier", fixedMultiplierField))
        stack.addArrangedSubview(formRow("Placement tolerance", placementToleranceField, suffix: "px"))
        return section(title: "Mac Mouse Model", subtitle: "Constants used by the live cursor prediction and placement logic.", content: stack)
    }

    private func screenEditor(index: Int, fields: ScreenFieldSet) -> NSView {
        let stack = formStack(labelWidth: 74)
        stack.addArrangedSubview(formRow("X", fields.x, labelWidth: 74))
        stack.addArrangedSubview(formRow("Y", fields.y, labelWidth: 74))
        stack.addArrangedSubview(formRow("Width", fields.width, labelWidth: 74))
        stack.addArrangedSubview(formRow("Height", fields.height, labelWidth: 74))
        stack.addArrangedSubview(formRow("Sensitivity", fields.sensitivity, suffix: "x", labelWidth: 74))
        return section(title: "Screen \(index)", subtitle: "", content: stack, compact: true)
    }

    private func section(title: String, subtitle: String, content: NSView, compact: Bool = false) -> NSView {
        let box = NSView()
        box.wantsLayer = true
        box.layer?.cornerRadius = 12
        box.layer?.backgroundColor = NSColor.windowBackgroundColor.cgColor
        box.translatesAutoresizingMaskIntoConstraints = false

        let stack = NSStackView()
        stack.orientation = .vertical
        stack.spacing = compact ? 8 : 12
        stack.edgeInsets = NSEdgeInsets(top: compact ? 12 : 16, left: 16, bottom: 16, right: 16)
        stack.translatesAutoresizingMaskIntoConstraints = false
        box.addSubview(stack)

        let titleLabel = NSTextField(labelWithString: title)
        titleLabel.font = NSFont.systemFont(ofSize: compact ? 14 : 17, weight: .semibold)
        stack.addArrangedSubview(titleLabel)

        if !subtitle.isEmpty {
            let subtitleLabel = NSTextField(labelWithString: subtitle)
            subtitleLabel.font = NSFont.systemFont(ofSize: 12)
            subtitleLabel.textColor = .secondaryLabelColor
            subtitleLabel.lineBreakMode = .byWordWrapping
            stack.addArrangedSubview(subtitleLabel)
        }

        stack.addArrangedSubview(content)

        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: box.leadingAnchor),
            stack.trailingAnchor.constraint(equalTo: box.trailingAnchor),
            stack.topAnchor.constraint(equalTo: box.topAnchor),
            stack.bottomAnchor.constraint(equalTo: box.bottomAnchor),
        ])

        return box
    }

    private func formStack(labelWidth: CGFloat = 180) -> NSStackView {
        let stack = NSStackView()
        stack.orientation = .vertical
        stack.spacing = 8
        return stack
    }

    private func formRow(_ label: String, _ control: NSView, suffix: String? = nil, help: String? = nil, labelWidth: CGFloat = 180) -> NSView {
        let row = NSStackView()
        row.orientation = .horizontal
        row.alignment = .centerY
        row.spacing = 10

        let labelView = NSTextField(labelWithString: label)
        labelView.font = NSFont.systemFont(ofSize: 12, weight: .medium)
        labelView.textColor = .secondaryLabelColor
        labelView.alignment = .right
        labelView.translatesAutoresizingMaskIntoConstraints = false
        labelView.widthAnchor.constraint(equalToConstant: labelWidth).isActive = true

        row.addArrangedSubview(labelView)
        row.addArrangedSubview(control)

        if let suffix {
            let suffixLabel = NSTextField(labelWithString: suffix)
            suffixLabel.font = NSFont.systemFont(ofSize: 12)
            suffixLabel.textColor = .tertiaryLabelColor
            row.addArrangedSubview(suffixLabel)
        }

        if let help {
            row.toolTip = help
        }

        return row
    }

    private func numericField() -> NSTextField {
        let field = NSTextField()
        styleNumericField(field, width: 96)
        return field
    }

    private func styleNumericField(_ field: NSTextField, width: CGFloat) {
        field.alignment = .right
        field.font = NSFont.monospacedDigitSystemFont(ofSize: 13, weight: .regular)
        field.translatesAutoresizingMaskIntoConstraints = false
        field.widthAnchor.constraint(equalToConstant: width).isActive = true
    }

    private func populate(from config: PersistentConfig) {
        unmappedCheckbox.state = config.unmappedPassthrough ? .on : .off
        partialScrollField.stringValue = String(config.partialScrollTimeout / 1000)
        intervalOverrideField.stringValue = String(config.intervalOverride)
        offscreenSensitivityField.stringValue = decimalString(Double(config.offscreenSensitivity) / 1000.0)
        cursorPlacementField.stringValue = String(config.cursorPlacementIntervalSeconds)
        constraintControl.selectedSegment = Int(config.constraintMode)

        trackingSpeedField.stringValue = decimalString(config.mouse.trackingSpeed)
        trackingSpeedSlider.doubleValue = config.mouse.trackingSpeed
        pointerResolutionField.stringValue = decimalString(config.mouse.pointerResolution)
        frameRateField.stringValue = decimalString(config.mouse.frameRate)
        fixedMultiplierField.stringValue = decimalString(config.mouse.fixedMultiplier)
        placementToleranceField.stringValue = decimalString(config.mouse.placementTolerance)

        populateScreenFields(config.screens)
        layoutView.screens = config.screens
        layoutView.selectedIndex = 0
        updateSaveState()
    }

    private func populateScreenFields(_ screens: [ScreenConfig]) {
        for (index, screen) in screens.enumerated() where index < screenFields.count {
            screenFields[index].x.stringValue = String(screen.x)
            screenFields[index].y.stringValue = String(screen.y)
            screenFields[index].width.stringValue = String(screen.width)
            screenFields[index].height.stringValue = String(screen.height)
            screenFields[index].sensitivity.stringValue = decimalString(Double(screen.sensitivity) / 1000.0)
        }
    }

    private func collectConfig() -> PersistentConfig? {
        guard var config = workingConfig else {
            return nil
        }

        config.unmappedPassthrough = unmappedCheckbox.state == .on
        config.partialScrollTimeout = clampedUInt32(Int64((doubleValue(partialScrollField) * 1000.0).rounded()))
        config.intervalOverride = UInt8(clamping: uint32Value(intervalOverrideField))
        config.constraintMode = UInt8(max(0, min(2, constraintControl.selectedSegment)))
        config.offscreenSensitivity = clampedUInt32(Int64((doubleValue(offscreenSensitivityField) * 1000.0).rounded()))
        config.cursorPlacementIntervalSeconds = uint32Value(cursorPlacementField)
        config.mouse = MouseConfig(
            trackingSpeed: doubleValue(trackingSpeedField),
            pointerResolution: doubleValue(pointerResolutionField),
            frameRate: doubleValue(frameRateField),
            fixedMultiplier: doubleValue(fixedMultiplierField),
            placementTolerance: doubleValue(placementToleranceField)
        )

        var screens: [ScreenConfig] = []
        for fieldSet in screenFields {
            screens.append(
                ScreenConfig(
                    x: uint32Value(fieldSet.x),
                    y: uint32Value(fieldSet.y),
                    width: uint32Value(fieldSet.width),
                    height: uint32Value(fieldSet.height),
                    sensitivity: clampedUInt32(Int64((doubleValue(fieldSet.sensitivity) * 1000.0).rounded()))
                )
            )
        }
        config.screens = screens
        return config
    }

    @objc private func controlChanged(_ sender: Any?) {
        if sender as? NSSlider === trackingSpeedSlider {
            trackingSpeedField.stringValue = decimalString(trackingSpeedSlider.doubleValue)
        } else if sender as? NSTextField === trackingSpeedField {
            trackingSpeedSlider.doubleValue = doubleValue(trackingSpeedField)
        }

        if let config = collectConfig() {
            workingConfig = config
            layoutView.screens = config.screens
        }
        markDirty()
    }

    func controlTextDidEndEditing(_ obj: Notification) {
        controlChanged(obj.object)
    }

    @objc private func save() {
        guard let config = collectConfig() else {
            return
        }

        if let validationMessage = validationMessage(for: config) {
            statusLabel.stringValue = validationMessage
            updateSaveState()
            return
        }

        setLoading(true, message: "Saving configuration...")
        DispatchQueue.global(qos: .userInitiated).async {
            do {
                try self.device.savePersistentConfig(config)
                DispatchQueue.main.async {
                    self.originalConfig = config
                    self.workingConfig = config
                    self.window?.close()
                }
            } catch {
                DispatchQueue.main.async {
                    self.setLoading(false, message: "Could not save configuration: \(error)")
                }
            }
        }
    }

    @objc private func discard() {
        window?.close()
    }

    private func setLoading(_ loading: Bool, message: String) {
        isBusy = loading
        statusLabel.stringValue = message
        discardButton.isEnabled = !loading
        updateSaveState()
    }

    private func markDirty() {
        guard let originalConfig, let workingConfig else {
            return
        }
        if let validationMessage = validationMessage(for: workingConfig) {
            statusLabel.stringValue = validationMessage
        } else {
            statusLabel.stringValue = workingConfig == originalConfig ? "No changes." : "Unsaved changes."
        }
        updateSaveState()
    }

    private func updateSaveState() {
        guard let originalConfig, let workingConfig, !isBusy else {
            saveButton.isEnabled = false
            return
        }

        saveButton.isEnabled = workingConfig != originalConfig && validationMessage(for: workingConfig) == nil
    }

    private func validationMessage(for config: PersistentConfig) -> String? {
        if let fieldName = firstInvalidNumberFieldName() {
            return "\(fieldName) must be a valid number."
        }

        guard config.screens.count == screenCount else {
            return "Expected \(screenCount) screen definitions."
        }

        if config.partialScrollTimeout == 0 {
            return "Partial scroll timeout must be greater than zero."
        }
        if config.constraintMode > 2 {
            return "Cursor constraint must be None, Box, or Visible."
        }
        if config.offscreenSensitivity == 0 {
            return "Offscreen sensitivity must be greater than zero."
        }

        for (index, screen) in config.screens.enumerated() {
            if screen.width == 0 || screen.height == 0 {
                return "Screen \(index) width and height must be greater than zero."
            }
            if screen.sensitivity == 0 {
                return "Screen \(index) sensitivity must be greater than zero."
            }
        }

        if config.mouse.trackingSpeed < 0 || !config.mouse.trackingSpeed.isFinite {
            return "Tracking speed must be zero or greater."
        }
        if config.mouse.pointerResolution <= 0 || !config.mouse.pointerResolution.isFinite {
            return "Pointer resolution must be greater than zero."
        }
        if config.mouse.frameRate <= 0 || !config.mouse.frameRate.isFinite {
            return "Frame rate must be greater than zero."
        }
        if config.mouse.fixedMultiplier <= 0 || !config.mouse.fixedMultiplier.isFinite {
            return "Fixed multiplier must be greater than zero."
        }
        if config.mouse.placementTolerance < 0 || !config.mouse.placementTolerance.isFinite {
            return "Placement tolerance must be zero or greater."
        }

        return nil
    }

    private func firstInvalidNumberFieldName() -> String? {
        let fields: [(String, NSTextField)] = [
            ("Partial scroll timeout", partialScrollField),
            ("Interval override", intervalOverrideField),
            ("Offscreen sensitivity", offscreenSensitivityField),
            ("Placement refresh", cursorPlacementField),
            ("Tracking speed", trackingSpeedField),
            ("Pointer resolution", pointerResolutionField),
            ("Frame rate", frameRateField),
            ("Fixed multiplier", fixedMultiplierField),
            ("Placement tolerance", placementToleranceField),
        ]

        for (name, field) in fields where parsedFiniteDouble(field) == nil {
            return name
        }

        for (index, fieldSet) in screenFields.enumerated() {
            for (name, field) in [
                ("Screen \(index) X", fieldSet.x),
                ("Screen \(index) Y", fieldSet.y),
                ("Screen \(index) width", fieldSet.width),
                ("Screen \(index) height", fieldSet.height),
                ("Screen \(index) sensitivity", fieldSet.sensitivity),
            ] where parsedFiniteDouble(field) == nil {
                return name
            }
        }

        return nil
    }

    private func parsedFiniteDouble(_ field: NSTextField) -> Double? {
        let text = field.stringValue.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !text.isEmpty, let value = Double(text), value.isFinite else {
            return nil
        }
        return value
    }
}

private final class LiveSyncApp: NSObject, NSApplicationDelegate {
    private let options: Options
    private let locator = DeviceLocator()
    private let configReader: MouseConfigReader
    private var device: ScreenHopperDevice?
    private var lastConfig: MouseConfig?
    private var lastCursor: RuntimeCursor?
    private var timer: Timer?
    private var statusItem: NSStatusItem?
    private var statusMenuItem: NSMenuItem?
    private var lastSyncMenuItem: NSMenuItem?
    private var launchAtLoginMenuItem: NSMenuItem?
    private var configWindow: ConfigWindowController?

    init(options: Options) {
        self.options = options
        configReader = MouseConfigReader(options: options)
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.accessory)
        installMenu()
        timer = Timer.scheduledTimer(withTimeInterval: options.pollInterval, repeats: true) { [weak self] _ in
            self?.syncTick()
        }
        syncTick()
    }

    private func installMenu() {
        let item = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
        item.button?.image = makeFrogStatusIcon()
        item.button?.imagePosition = .imageOnly
        item.button?.toolTip = "Screen Hopper Live Sync"
        item.button?.setAccessibilityLabel("Screen Hopper Live Sync")

        let menu = NSMenu()
        let status = NSMenuItem(title: "Screen Hopper: disconnected", action: nil, keyEquivalent: "")
        status.isEnabled = false
        menu.addItem(status)

        let lastSync = NSMenuItem(title: "Last sync: never", action: nil, keyEquivalent: "")
        lastSync.isEnabled = false
        menu.addItem(lastSync)

        menu.addItem(.separator())
        let configureItem = NSMenuItem(title: "Configure...", action: #selector(openConfiguration), keyEquivalent: ",")
        configureItem.target = self
        menu.addItem(configureItem)

        let launchAtLoginItem = NSMenuItem(title: "Launch at Login", action: #selector(toggleLaunchAtLogin), keyEquivalent: "")
        launchAtLoginItem.target = self
        menu.addItem(launchAtLoginItem)

        let quitItem = NSMenuItem(title: "Quit", action: #selector(quit), keyEquivalent: "q")
        quitItem.target = self
        menu.addItem(quitItem)

        item.menu = menu
        statusItem = item
        statusMenuItem = status
        lastSyncMenuItem = lastSync
        launchAtLoginMenuItem = launchAtLoginItem
        updateLaunchAtLoginItem()
    }

    @objc private func quit() {
        NSApp.terminate(nil)
    }

    @objc private func toggleLaunchAtLogin() {
        guard #available(macOS 13.0, *) else {
            showAlert(title: "Launch at Login Unavailable", message: "Launch at Login requires macOS 13 or newer.")
            return
        }

        do {
            if SMAppService.mainApp.status == .enabled {
                try SMAppService.mainApp.unregister()
            } else {
                try SMAppService.mainApp.register()
            }
            updateLaunchAtLoginItem()
        } catch {
            showAlert(title: "Could Not Update Login Item", message: "\(error)")
        }
    }

    private func updateLaunchAtLoginItem() {
        guard #available(macOS 13.0, *) else {
            launchAtLoginMenuItem?.isEnabled = false
            launchAtLoginMenuItem?.state = .off
            return
        }

        launchAtLoginMenuItem?.state = SMAppService.mainApp.status == .enabled ? .on : .off
    }

    @objc private func openConfiguration() {
        if let configWindow {
            configWindow.showWindow(nil)
            NSApp.activate(ignoringOtherApps: true)
            return
        }

        if device == nil {
            device = locator.findRuntimeDevice()
        }

        guard let device else {
            showAlert(title: "Screen Hopper Not Found", message: "Connect Screen Hopper and try again.")
            return
        }

        let controller = ConfigWindowController(device: device)
        controller.onClose = { [weak self] in
            self?.configWindow = nil
        }
        configWindow = controller
        controller.showWindow(nil)
        NSApp.activate(ignoringOtherApps: true)
        controller.loadConfiguration()
    }

    private func syncTick() {
        if configWindow != nil {
            return
        }

        if device == nil {
            device = locator.findRuntimeDevice()
        }

        guard let device else {
            updateMenu(connected: false, message: "Screen Hopper: disconnected")
            return
        }

        do {
            let config = configReader.currentConfig()
            if config != lastConfig {
                try device.sendMouseConfig(config)
                lastConfig = config
            }

            let cursor = currentCursor(activeScreen: options.activeScreen)
            try device.sendCursor(cursor)
            lastCursor = cursor
            updateMenu(connected: true, message: menuSummary(config: config, cursor: cursor))
        } catch {
            self.device = nil
            updateMenu(connected: false, message: "Screen Hopper: reconnecting")
        }
    }

    private func updateMenu(connected: Bool, message: String) {
        statusItem?.button?.toolTip = connected ? "Screen Hopper Live Sync" : "Screen Hopper Live Sync: disconnected"
        statusMenuItem?.title = message

        let formatter = DateFormatter()
        formatter.timeStyle = .medium
        lastSyncMenuItem?.title = connected ? "Last sync: \(formatter.string(from: Date()))" : "Last sync: waiting"
    }

    private func menuSummary(config: MouseConfig, cursor: RuntimeCursor) -> String {
        String(
            format: "Screen Hopper: tracking %.4f, cursor %lld,%lld",
            config.trackingSpeed,
            cursor.x,
            cursor.y
        )
    }

    private func showAlert(title: String, message: String) {
        let alert = NSAlert()
        alert.messageText = title
        alert.informativeText = message
        alert.alertStyle = .warning
        alert.runModal()
    }
}

private func makeFrogStatusIcon() -> NSImage {
    let image = NSImage(size: NSSize(width: 18, height: 18))
    image.lockFocus()

    let bounds = NSRect(x: 0, y: 0, width: 18, height: 18)
    NSColor.clear.setFill()
    bounds.fill()

    NSColor.black.setFill()
    NSBezierPath(ovalIn: NSRect(x: 3.0, y: 3.4, width: 12.0, height: 10.4)).fill()
    NSBezierPath(ovalIn: NSRect(x: 4.0, y: 10.0, width: 4.6, height: 4.6)).fill()
    NSBezierPath(ovalIn: NSRect(x: 9.4, y: 10.0, width: 4.6, height: 4.6)).fill()

    if let context = NSGraphicsContext.current?.cgContext {
        context.setBlendMode(.clear)
        NSBezierPath(ovalIn: NSRect(x: 5.15, y: 11.2, width: 2.0, height: 2.0)).fill()
        NSBezierPath(ovalIn: NSRect(x: 10.85, y: 11.2, width: 2.0, height: 2.0)).fill()

        let smile = NSBezierPath()
        smile.move(to: NSPoint(x: 6.2, y: 7.2))
        smile.curve(
            to: NSPoint(x: 11.8, y: 7.2),
            controlPoint1: NSPoint(x: 7.4, y: 5.9),
            controlPoint2: NSPoint(x: 10.6, y: 5.9)
        )
        smile.lineWidth = 1.1
        smile.lineCapStyle = .round
        smile.stroke()

        context.setBlendMode(.normal)
    }

    NSColor.black.setFill()
    NSBezierPath(ovalIn: NSRect(x: 5.75, y: 11.75, width: 0.9, height: 0.9)).fill()
    NSBezierPath(ovalIn: NSRect(x: 11.35, y: 11.75, width: 0.9, height: 0.9)).fill()

    image.unlockFocus()
    image.isTemplate = true
    return image
}

private enum LiveSyncError: Error {
    case payloadTooLarge
    case hidSetReportFailed(IOReturn)
    case hidGetReportFailed(IOReturn)
    case incompatibleConfigVersion(UInt8)
    case invalidReport
    case invalidCRC
}

private func fixed16(_ value: Double) -> UInt32 {
    if !value.isFinite || value <= 0.0 {
        return 0
    }

    let scaled = (value * fixed16Scale).rounded()
    if scaled >= Double(UInt32.max) {
        return UInt32.max
    }
    return UInt32(scaled)
}

private func doubleFromFixed16(_ value: UInt32) -> Double {
    Double(value) / fixed16Scale
}

private func clampedUInt32(_ value: Int64) -> UInt32 {
    if value <= 0 {
        return 0
    }
    if value >= Int64(UInt32.max) {
        return UInt32.max
    }
    return UInt32(value)
}

private func decimalString(_ value: Double) -> String {
    guard value.isFinite else {
        return "0"
    }

    if value.rounded() == value {
        return String(format: "%.0f", value)
    }

    return String(format: "%.4f", value)
        .replacingOccurrences(of: "0+$", with: "", options: .regularExpression)
        .replacingOccurrences(of: "\\.$", with: "", options: .regularExpression)
}

private func doubleValue(_ field: NSTextField) -> Double {
    Double(field.stringValue.trimmingCharacters(in: .whitespacesAndNewlines)) ?? 0
}

private func uint32Value(_ field: NSTextField) -> UInt32 {
    clampedUInt32(Int64(doubleValue(field).rounded()))
}

private func currentCursor(activeScreen: Int8 = -1) -> RuntimeCursor {
    let point = CGEvent(source: nil)?.location ?? NSEvent.mouseLocation
    return RuntimeCursor(
        x: Int64(point.x.rounded()),
        y: Int64(point.y.rounded()),
        activeScreen: activeScreen
    )
}

private func parseOptions() -> Options {
    var options = Options()
    let args = Array(CommandLine.arguments.dropFirst())

    func takeValue(after index: Int) -> String? {
        guard index + 1 < args.count else {
            return nil
        }
        return args[index + 1]
    }

    var index = 0
    while index < args.count {
        let arg = args[index]
        switch arg {
        case "--active-screen":
            if let value = takeValue(after: index), let parsed = Int8(value) {
                options.activeScreen = parsed
                index += 1
            }
        case "--pointer-resolution":
            if let value = takeValue(after: index), let parsed = Double(value) {
                options.pointerResolution = parsed
                index += 1
            }
        case "--frame-rate":
            if let value = takeValue(after: index), let parsed = Double(value) {
                options.frameRate = parsed
                index += 1
            }
        case "--fixed-multiplier":
            if let value = takeValue(after: index), let parsed = Double(value) {
                options.fixedMultiplier = parsed
                index += 1
            }
        case "--placement-tolerance":
            if let value = takeValue(after: index), let parsed = Double(value) {
                options.placementTolerance = parsed
                index += 1
            }
        case "--poll-interval":
            if let value = takeValue(after: index), let parsed = Double(value), parsed > 0 {
                options.pollInterval = parsed
                index += 1
            }
        default:
            break
        }
        index += 1
    }

    return options
}

private extension Data {
    mutating func appendUInt8(_ value: UInt8) {
        append(value)
    }

    mutating func appendUInt32LE(_ value: UInt32) {
        var littleEndian = value.littleEndian
        Swift.withUnsafeBytes(of: &littleEndian) { bytes in
            append(contentsOf: bytes)
        }
    }

    mutating func appendInt32LE(_ value: Int32) {
        var littleEndian = value.littleEndian
        Swift.withUnsafeBytes(of: &littleEndian) { bytes in
            append(contentsOf: bytes)
        }
    }

    mutating func appendInt64LE(_ value: Int64) {
        var littleEndian = value.littleEndian
        Swift.withUnsafeBytes(of: &littleEndian) { bytes in
            append(contentsOf: bytes)
        }
    }

    func readUInt32LE(at offset: Int) -> UInt32 {
        var value: UInt32 = 0
        for i in 0..<4 {
            value |= UInt32(self[offset + i]) << UInt32(i * 8)
        }
        return value
    }

    func readInt32LE(at offset: Int) -> Int32 {
        Int32(bitPattern: readUInt32LE(at: offset))
    }
}

@main
private struct ScreenHopperLiveMain {
    private static let delegate = LiveSyncApp(options: parseOptions())

    static func main() {
        let app = NSApplication.shared
        app.delegate = delegate
        app.run()
    }
}
