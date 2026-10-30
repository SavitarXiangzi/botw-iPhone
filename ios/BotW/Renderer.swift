import MetalKit
import simd

/// M0 renderer: proves the Metal path on device (triangle + constant buffer +
/// a real frame pump), and drives the C core through its ABI so the boundary
/// is exercised on every frame rather than only at launch.
///
/// The triangle is deliberately not the point.  What matters is that
/// `botw_core` is created with the same MTLDevice/queue the view uses, that
/// `botw_core_nvn_stats` reports the generated NVN registry on device, and
/// that `botw_core_frame` is called at display rate.  When the ARM64 core is
/// linked in, this file does not change.
final class Renderer: NSObject, MTKViewDelegate {
    private let device: MTLDevice
    private let queue: MTLCommandQueue
    private let pipeline: MTLRenderPipelineState
    private var time: Float = 0
    private(set) var frameIndex: UInt64 = 0

    private var core: OpaquePointer?
    private(set) var stats = Stats()

    struct Stats {
        var backend = "none"
        var nvnImplemented = 0
        var nvnTotal = 0
        var coreError = ""
    }

    struct Uniforms { var time: Float; var aspect: Float }

    init?(view: MTKView) {
        guard let dev = view.device ?? MTLCreateSystemDefaultDevice(),
              let q = dev.makeCommandQueue() else { return nil }
        device = dev
        queue = q
        view.device = dev
        view.colorPixelFormat = .bgra8Unorm
        view.clearColor = MTLClearColor(red: 0.02, green: 0.03, blue: 0.05, alpha: 1)

        let src = """
        #include <metal_stdlib>
        using namespace metal;

        struct Uniforms { float time; float aspect; };

        struct VOut { float4 pos [[position]]; float2 uv; };

        vertex VOut v_main(uint vid [[vertex_id]], constant Uniforms &u [[buffer(0)]]) {
            float2 p[3] = { float2(0.0, 0.6), float2(-0.6, -0.5), float2(0.6, -0.5) };
            float2 v = p[vid];
            float s = sin(u.time), c = cos(u.time);
            v = float2(v.x * c - v.y * s, v.x * s + v.y * c);
            v.x /= u.aspect;
            VOut o;
            o.pos = float4(v, 0.0, 1.0);
            o.uv = v * 0.5 + 0.5;
            return o;
        }

        fragment float4 f_main(VOut in [[stage_in]]) {
            return float4(in.uv.x, in.uv.y, 0.6, 1.0);
        }
        """
        guard let lib = try? dev.makeLibrary(source: src, options: nil),
              let vfn = lib.makeFunction(name: "v_main"),
              let ffn = lib.makeFunction(name: "f_main") else { return nil }

        let desc = MTLRenderPipelineDescriptor()
        desc.vertexFunction = vfn
        desc.fragmentFunction = ffn
        desc.colorAttachments[0].pixelFormat = view.colorPixelFormat
        guard let p = try? dev.makeRenderPipelineState(descriptor: desc) else { return nil }
        pipeline = p

        super.init()

        // The C core gets the very same device/queue the view renders with.
        var cdesc = botw_core_desc()
        cdesc.width = UInt32(max(view.drawableSize.width, 1))
        cdesc.height = UInt32(max(view.drawableSize.height, 1))
        cdesc.metal_device = Unmanaged.passUnretained(dev).toOpaque()
        cdesc.metal_command_queue = Unmanaged.passUnretained(q).toOpaque()
        cdesc.romfs_root = nil
        if let c = botw_core_create(&cdesc) {
            core = c
            var s = botw_core_nvn_stats()
            botw_core_nvn_stats_get(c, &s)
            stats.backend = s.backend.map { String(cString: $0) } ?? "?"
            stats.nvnImplemented = Int(s.implemented)
            stats.nvnTotal = Int(s.total)
            stats.coreError = String(cString: botw_core_last_error(c))
        } else {
            stats.coreError = "botw_core_create failed"
        }

        // NOTE: the delegate is installed by MetalView's coordinator (it owns
        // the Pump); setting view.delegate = self here would be overwritten.
        // 60 is the honest default: the guest's own frame pacing is what the
        // NVN present interval controls, not this number.
        view.preferredFramesPerSecond = 60
        view.isPaused = false
        view.enableSetNeedsDisplay = false
    }

    deinit {
        if let c = core { botw_core_destroy(c) }
    }

    func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {
        guard let c = core else { return }
        _ = c
        var s = botw_core_nvn_stats()
        botw_core_nvn_stats_get(c, &s)
        stats.nvnImplemented = Int(s.implemented)
    }

    func draw(in view: MTKView) {
        guard let rpd = view.currentRenderPassDescriptor,
              let drawable = view.currentDrawable,
              let cb = queue.makeCommandBuffer(),
              let enc = cb.makeRenderCommandEncoder(descriptor: rpd) else { return }
        time += 1.0 / 60.0
        frameIndex &+= 1

        if let c = core {
            _ = botw_core_frame(c, 1.0 / 60.0)
        }

        var u = Uniforms(time: time,
                         aspect: Float(max(view.drawableSize.width, 1) / max(view.drawableSize.height, 1)))
        enc.setRenderPipelineState(pipeline)
        enc.setVertexBytes(&u, length: MemoryLayout<Uniforms>.stride, index: 0)
        enc.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
        enc.endEncoding()
        cb.present(drawable)
        cb.commit()
    }
}
