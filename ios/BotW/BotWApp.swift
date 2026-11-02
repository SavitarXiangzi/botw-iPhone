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
        // Either form works: an environment variable (simctl strips the
        // SIMCTL_CHILD_ prefix) or a launch argument.
        let env = ProcessInfo.processInfo.environment["BOTW_SELFTEST"]
        if env == "1" || CommandLine.arguments.contains("--selftest") {
            // The console is attached by simctl after launch, so a process
            // that exits in milliseconds can lose its stdout.  A file in the
            // app container survives that race and is read by CI afterwards.
            // "start" is written before the test so a crash inside it is
            // distinguishable from the hook never running.
            let dir = FileManager.default.urls(for: .documentDirectory,
                                               in: .userDomainMask).first
            func report(_ text: String) {
                if let dir = dir {
                    try? text.write(to: dir.appendingPathComponent("selftest.txt"),
                                    atomically: true, encoding: .utf8)
                }
                print(text, terminator: "")
                fflush(stdout)
            }
            report("BOTW_SELFTEST start\n")
            // Two independent checks: the guest core actually enters native
            // AArch64 code, then the Metal replay layer renders and reads back
            // its synthetic frame.  The combined rc is what CI asserts.
            let guestRc = botw_core_guest_selftest()
            report("BOTW_GUEST_SELFTEST rc=\(guestRc)\n")
            let replayRc = botw_core_selftest()
            let rc = (guestRc == 0 && replayRc == 0) ? 0 : 1
            report("BOTW_SELFTEST rc=\(rc) (guest=\(guestRc) replay=\(replayRc))\n")
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
