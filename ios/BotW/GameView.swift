import Observation
import SwiftUI
import MetalKit

/// Debug HUD.  Deliberately tiny: it exists so the number of NVN entry points
/// the backend actually owns is visible on a real device, next to the code
/// that generates that number.
@Observable
final class HUDModel {
    var backend = "…"
    var implemented = 0
    var total = 0
    var error = "…"
    var frames: UInt64 = 0
}

struct MetalView: UIViewRepresentable {
    let hud: HUDModel

    func makeCoordinator() -> Coordinator { Coordinator() }

    func makeUIView(context: Context) -> MTKView {
        let v = MTKView(frame: .zero, device: MTLCreateSystemDefaultDevice())
        let r = Renderer(view: v)
        context.coordinator.renderer = r
        if let r {
            hud.backend = r.stats.backend
            hud.implemented = r.stats.nvnImplemented
            hud.total = r.stats.nvnTotal
            hud.error = r.stats.coreError
            context.coordinator.hud = hud
            // MTKView.delegate is weak.  The Pump has to be owned by the
            // coordinator or it is deallocated before the first frame and the
            // view stays black forever.
            let pump = Pump(renderer: r, hud: hud)
            context.coordinator.pump = pump
            v.delegate = pump
        }
        return v
    }

    func updateUIView(_ uiView: MTKView, context: Context) {}

    final class Coordinator {
        var renderer: Renderer?
        var hud: HUDModel?
        var pump: Pump?
    }

    /// Forwards MTKViewDelegate callbacks to the renderer while occasionally
    /// refreshing the HUD off the render path.
    final class Pump: NSObject, MTKViewDelegate {
        private let renderer: Renderer
        private let hud: HUDModel
        private var tick = 0

        init(renderer: Renderer, hud: HUDModel) {
            self.renderer = renderer
            self.hud = hud
            super.init()
        }

        func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {
            renderer.mtkView(view, drawableSizeWillChange: size)
        }

        func draw(in view: MTKView) {
            renderer.draw(in: view)
            tick &+= 1
            if tick % 30 == 0 {
                hud.frames = renderer.frameIndex
                hud.implemented = renderer.stats.nvnImplemented
            }
        }
    }
}

struct GameView: View {
    @State private var hud = HUDModel()

    var body: some View {
        ZStack(alignment: .topLeading) {
            MetalView(hud: hud)
            VStack(alignment: .leading, spacing: 2) {
                Text("BOTW iOS · M0 renderer")
                    .fontWeight(.semibold)
                Text("nvn \(hud.implemented)/\(hud.total) on \(hud.backend)")
                Text("frame \(hud.frames)")
                Text(hud.error)
                    .foregroundStyle(.orange)
            }
            .font(.system(size: 11, design: .monospaced))
            .foregroundStyle(.white.opacity(0.8))
            .padding(12)
        }
        .background(.black)
    }
}