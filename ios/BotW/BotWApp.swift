import Darwin
import Foundation
import SwiftUI

@main
struct BotWApp: App {
    init() {
        // Debug hook for CI: run the replay layer's self-test inside a real
        // app process and leave.  `simctl launch --console-pty` captures the
        // line below, so a renderer regression fails the workflow instead of
        // only failing a build.
        if ProcessInfo.processInfo.environment["BOTW_SELFTEST"] == "1" {
            let rc = botw_core_selftest()
            print("BOTW_SELFTEST rc=\(rc)")
            fflush(stdout)
            exit(rc == 0 ? 0 : 1)
        }
    }

    var body: some Scene {
        WindowGroup {
            GameViewRoot()
                .ignoresSafeArea()
                .statusBarHidden(true)
                .persistentSystemOverlays(.hidden)
        }
    }
}
