import SwiftUI

@main
struct BotWApp: App {
    var body: some Scene {
        WindowGroup {
            GameViewRoot()
                .ignoresSafeArea()
                .statusBarHidden(true)
                .persistentSystemOverlays(.hidden)
        }
    }
}
