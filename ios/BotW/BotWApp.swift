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
            let line = "BOTW_SELFTEST rc=\(rc)\n"
            // The console is attached by simctl after launch, so a process
            // that exits in milliseconds can lose its stdout.  A file in the
            // app container survives that race and is read by CI afterwards.
            if let dir = FileManager.default.urls(for: .documentDirectory,
                                                  in: .userDomainMask).first {
                try? line.write(to: dir.appendingPathComponent("selftest.txt"),
                                atomically: true, encoding: .utf8)
            }
            print(line, terminator: "")
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
