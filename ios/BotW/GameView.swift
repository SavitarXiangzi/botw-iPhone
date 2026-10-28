import SwiftUI
import MetalKit

struct MetalView: UIViewRepresentable {
    func makeCoordinator() -> Coordinator { Coordinator() }

    func makeUIView(context: Context) -> MTKView {
        let v = MTKView(frame: .zero, device: MTLCreateSystemDefaultDevice())
        context.coordinator.renderer = Renderer(view: v)
        return v
    }

    func updateUIView(_ uiView: MTKView, context: Context) {}

    final class Coordinator {
        var renderer: Renderer?
    }
}

struct GameView: View {
    var body: some View {
        ZStack(alignment: .topLeading) {
            MetalView()
            Text("BOTW iOS · M0 renderer")
                .font(.system(size: 12, weight: .semibold, design: .monospaced))
                .foregroundStyle(.white.opacity(0.75))
                .padding(12)
        }
        .background(.black)
    }
}
