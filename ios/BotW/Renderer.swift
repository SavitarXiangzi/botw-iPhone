import Foundation
import Metal
import QuartzCore

/// Drives the C core one frame at a time.
///
/// There is no MTKView here on purpose.  The NVN window has to own the layer
/// the app displays, otherwise the guest's swapchain presents into a surface
/// nobody can see -- which is exactly what the first version of this shell did
/// (MTKView drew a triangle on its own layer while the NVN window made a
/// private one that went nowhere).  So the view is a plain UIView backed by a
/// CAMetalLayer, and that layer is handed to botw_core at creation time.
final class CoreDriver: NSObject {
    struct Stats {
        var backend = "none"
        var nvnImplemented = 0
        var nvnTotal = 0
        var presents = 0
        var coreError = ""
    }

    private(set) var core: OpaquePointer?
    private(set) var stats = Stats()

    private let hud: HUDModel
    /// Kept alive here because the core borrows it (__bridge, no retain).
    private var queue: MTLCommandQueue?
    private var link: CADisplayLink?
    private var lastTimestamp: CFTimeInterval = 0
    private var ticksSincePublish = 0

    init(view: MetalView, hud: HUDModel) {
        self.hud = hud
        super.init()

        guard let device = view.metalLayer.device ?? MTLCreateSystemDefaultDevice(),
              let queue = device.makeCommandQueue() else {
            stats.coreError = "no Metal device/queue"
            publish()
            return
        }
        self.queue = queue
        view.metalLayer.device = device
        view.metalLayer.pixelFormat = .bgra8Unorm
        // The guest's target is blitted in, so the drawable must not be
        // framebuffer-only.
        view.metalLayer.framebufferOnly = false
        view.metalLayer.isOpaque = true

        var desc = botw_core_desc()
        desc.width = UInt32(max(view.metalLayer.drawableSize.width, 1))
        desc.height = UInt32(max(view.metalLayer.drawableSize.height, 1))
        desc.metal_device = Unmanaged.passUnretained(device).toOpaque()
        // Same device and queue the view renders with; the core borrows both.
        desc.metal_command_queue = Unmanaged.passUnretained(queue).toOpaque()
        desc.native_layer = Unmanaged.passUnretained(view.metalLayer).toOpaque()
        desc.romfs_root = nil

        guard let c = botw_core_create(&desc) else {
            stats.coreError = "botw_core_create failed"
            publish()
            return
        }
        core = c
        publish()

        let link = CADisplayLink(target: self, selector: #selector(tick(_:)))
        link.preferredFrameRateRange = CAFrameRateRange(minimum: 30, maximum: 120, preferred: 60)
        link.add(to: .main, forMode: .common)
        self.link = link
    }

    deinit {
        link?.invalidate()
        if let c = core { botw_core_destroy(c) }
    }

    @objc private func tick(_ link: CADisplayLink) {
        guard let c = core else { return }
        var dt = lastTimestamp > 0 ? link.timestamp - lastTimestamp : 1.0 / 60.0
        lastTimestamp = link.timestamp
        if dt <= 0 || dt > 0.25 { dt = 1.0 / 60.0 }

        _ = botw_core_frame(c, dt)

        ticksSincePublish += 1
        if ticksSincePublish >= 15 {
            ticksSincePublish = 0
            publish()
        }
    }

    /// Copies the core's counters into the HUD.  Cheap, but not per-frame:
    /// @Observable mutations trigger SwiftUI invalidation.
    private func publish() {
        var s = botw_core_nvn_stats()
        if let c = core { botw_core_nvn_stats_get(c, &s) }

        stats.backend = s.backend.map { String(cString: $0) } ?? "?"
        stats.nvnImplemented = Int(s.implemented)
        stats.nvnTotal = Int(s.total)
        stats.presents = Int(s.presents)
        // Only overwrite the recorded error when there is a core to ask;
        // otherwise keep whatever the initialisation failure said.
        if let c = core {
            stats.coreError = String(cString: botw_core_last_error(c))
        }

        hud.backend = stats.backend
        hud.implemented = stats.nvnImplemented
        hud.total = stats.nvnTotal
        hud.presents = stats.presents
        hud.error = stats.coreError
    }
}