#import <XCTest/XCTest.h>

#include "nvn_metal.h"

/* The replay layer's only executed test.
 *
 * Everything else in this repository is checked by compilation or by pure
 * functions.  This one runs on a real Metal device -- the iOS Simulator on
 * CI -- and drives one complete frame through the guest-facing entry points,
 * then reads the centre pixel back.  It is the reason "the Metal half works"
 * is a measurement rather than a claim.
 *
 * It does NOT prove that a guest shader can be run: the program it draws is
 * the built-in MSL in nvn_shader.c, reached through the same
 * nvngfx_shader_to_msl() seam a translator will plug into. */
@interface MetalReplayTests : XCTestCase
@end

@implementation MetalReplayTests

- (void)testOneDrawProducesTheExpectedPixel {
    int rc = nvngfx_metal_selftest();
    XCTAssertEqual(rc, 0,
                   @"nvngfx_metal_selftest failed (bit %d): 1 no device, 2 not ready, "
                   @"4 texture, 8 program, 16 recording, 32 pipeline, 64 draw, 128 pixel",
                   rc);
}

- (void)testRepeatedFramesAreStable {
    /* A second run catches state that leaked from the first one through the
     * backend's global tables (a stale texture, a stale pipeline). */
    for (int i = 0; i < 3; ++i) {
        XCTAssertEqual(nvngfx_metal_selftest(), 0, @"frame %d failed", i);
    }
}

@end