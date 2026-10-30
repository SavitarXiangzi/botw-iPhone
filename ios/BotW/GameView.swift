import Observation
import SwiftUI
import UIKit
import QuartzCore

/// Debug HUD.  Deliberately tiny: it exists so the number of NVN entry points
/// the backend actually owns, and the number of frames that reached the
/// display, are visible on a real device next to the code that produces them.
@Observable
final class HUDModel {
    var backend = "…"
    var implemented = 0
    var total = 0
    var presents = 0
    var error = "…"
}

/// A UIView whose backing layer is a CAMetalLayer.
///
/// The layer is the surface the NVN window presents into, so it has to be a
/// real on-screen layer owned by the view hierarchy.  drawableSize is set here
/// because CAMetalLayer does not follow bounds/resize on its own the way
/// MTKView does.
final class MetalView: UIView {
    override class var layerClass: AnyClass { CAMetalLayer.self }

    var metalLayer: CAMetalLayer { layer as! CAMetalLayer }

    override func layoutSubviews() {
        super.layoutSubviews()
        let scale = traitCollection.displayScale > 0 ? traitCollection.displayScale : UIScreen.main.scale
        metalLayer.contentsScale = scale
        metalLayer.drawableSize = CGSize(width: bounds.width * scale, height: bounds.height * scale)
    }
}

struct GameView: UIViewRepresentable {
    let hud: HUDModel

    func makeCoordinator() -> Coordinator { Coordinator() }

    func makeUIView(context: Context) -> MetalView {
        let view = MetalView(frame: .zero)
        view.backgroundColor = .black
        // The driver must outlive makeUIView; MTKView taught us that a weak
        // delegate quietly deallocates and the screen stays black.
        context.coordinator.driver = CoreDriver(view: view, hud: hud)
        return view
    }

    func updateUIView(_ uiView: MetalView, context: Context) {}

    final class Coordinator {
        var driver: CoreDriver?
    }
}

struct GameViewRoot: View {
    @State private var hud = HUDModel()

    var body: some View {
        ZStack(alignment: .topLeading) {
            GameView(hud: hud)
            VStack(alignment: .leading, spacing: 2) {
                Text("BOTW iOS · shell")
                    .fontWeight(.semibold)
                Text("nvn \(hud.implemented)/\(hud.total) on \(hud.backend)")
                Text("presents \(hud.presents)")
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