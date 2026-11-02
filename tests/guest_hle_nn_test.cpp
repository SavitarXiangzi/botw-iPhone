/* nn::oe HLE: every import the 1.6.0 main image asks for is claimed by name,
 * the answers are the state we set, and the push-style setters round-trip.
 *
 * The name list below is copied from `reloc_run ... --names` on the real
 * main.patched.bin.  If a digit in a mangled name is wrong, the string here
 * still matches the string in guest_hle_nn.c and this test passes while the
 * real image keeps hitting the stub.  That is why the same list is also
 * checked against a real image in the CI guest-core step (--hle reports 0
 * remaining nn::oe stubs); this test only proves the registry wiring.
 */
#include "guest_hle.h"
#include "guest_hle_nn.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::printf("guest_hle_nn_test: FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    g_fail++; } } while (0)

static const char* const kOeNames[BOTW_HLE_OE_COUNT] = {
    "_ZN2nn2oe10InitializeEv",
    "_ZN2nn2oe15EnableRecordingEv",
    "_ZN2nn2oe15SetCpuBoostModeENS0_12CpuBoostModeE",
    "_ZN2nn2oe16GetOperationModeEv",
    "_ZN2nn2oe17FinishStartupLogoEv",
    "_ZN2nn2oe18GetDesiredLanguageEv",
    "_ZN2nn2oe18GetPerformanceModeEv",
    "_ZN2nn2oe20GetCurrentFocusStateEv",
    "_ZN2nn2oe20SetFocusHandlingModeENS0_17FocusHandlingModeE",
    "_ZN2nn2oe21TryPopLaunchParameterEPmPvm",
    "_ZN2nn2oe24GetExpectedVolumeBalanceEPfS1_",
    "_ZN2nn2oe24SetExpectedVolumeBalanceEff",
    "_ZN2nn2oe25TryPopNotificationMessageEPj",
    "_ZN2nn2oe27SetPerformanceConfigurationENS0_15PerformanceModeEi",
    "_ZN2nn2oe28SetResumeNotificationEnabledEb",
    "_ZN2nn2oe31EnterExitRequestHandlingSectionEv",
    "_ZN2nn2oe31LeaveExitRequestHandlingSectionEv",
    "_ZN2nn2oe38SetUserInactivityDetectionTimeExtendedEb",
    "_ZN2nn2oe42SetOperationModeChangedNotificationEnabledEb",
    "_ZN2nn2oe44SetPerformanceModeChangedNotificationEnabledEb",
};

static botw_hle_table g_hle;
static botw_hle_nn_state g_state;

int main() {
    botw_hle_init(&g_hle);
    CHECK(g_hle.error[0] == '\0');

    for (unsigned i = 0; i < BOTW_HLE_OE_COUNT; i++) {
        CHECK(botw_hle_lookup(&g_hle, kOeNames[i]) != nullptr);
    }
    /* The count is libc + nn::oe; no nn::oe name may have landed on a stub. */
    CHECK(botw_hle_implementation_count(&g_hle) >= 46 + BOTW_HLE_OE_COUNT);
    CHECK(botw_hle_stub_count(&g_hle) == 0);

    /* Explicit state: register it, then drive it. */
    botw_hle_nn_reset(&g_state);
    CHECK(g_state.operation_mode == BOTW_OE_MODE_HANDHELD);
    CHECK(g_state.performance_mode == BOTW_OE_PERF_BOOST);
    CHECK(g_state.focus_state == BOTW_OE_FOCUS_FOCUSED);
    CHECK(g_state.desired_language == BOTW_OE_LANG_AMERICAN_ENGLISH);
    CHECK(botw_hle_nn_register(&g_hle, &g_state) == 0);

    g_state.operation_mode = BOTW_OE_MODE_CONSOLE;
    g_state.desired_language = BOTW_OE_LANG_SIMPLIFIED_CHINESE;
    g_state.focus_state = BOTW_OE_FOCUS_FOCUSED;

    typedef uint32_t (*query_fn)(void);
    CHECK(((query_fn)botw_hle_lookup(&g_hle, "_ZN2nn2oe16GetOperationModeEv"))() ==
          BOTW_OE_MODE_CONSOLE);
    CHECK(((query_fn)botw_hle_lookup(&g_hle, "_ZN2nn2oe18GetDesiredLanguageEv"))() ==
          BOTW_OE_LANG_SIMPLIFIED_CHINESE);
    CHECK(((query_fn)botw_hle_lookup(&g_hle, "_ZN2nn2oe18GetPerformanceModeEv"))() ==
          BOTW_OE_PERF_BOOST);
    CHECK(((query_fn)botw_hle_lookup(&g_hle, "_ZN2nn2oe20GetCurrentFocusStateEv"))() ==
          BOTW_OE_FOCUS_FOCUSED);

    /* A launch parameter must be a clean miss, not "a parameter of length 0". */
    typedef bool (*pop_launch_fn)(uint64_t*, void*, uint64_t);
    uint64_t out_size = 0xdeadbeefu;
    unsigned char buf[16];
    CHECK(((pop_launch_fn)botw_hle_lookup(&g_hle, "_ZN2nn2oe21TryPopLaunchParameterEPmPvm"))(
              &out_size, buf, sizeof buf) == false);
    CHECK(out_size == 0);

    typedef bool (*pop_msg_fn)(uint32_t*);
    uint32_t msg = 0xffffffffu;
    CHECK(((pop_msg_fn)botw_hle_lookup(&g_hle, "_ZN2nn2oe25TryPopNotificationMessageEPj"))(&msg) ==
          false);

    /* Volume balance round-trips through float registers. */
    typedef void (*set_vol_fn)(float, float);
    typedef void (*get_vol_fn)(float*, float*);
    ((set_vol_fn)botw_hle_lookup(&g_hle, "_ZN2nn2oe24SetExpectedVolumeBalanceEff"))(0.25f, 0.75f);
    float left = -1.0f, right = -1.0f;
    ((get_vol_fn)botw_hle_lookup(&g_hle, "_ZN2nn2oe24GetExpectedVolumeBalanceEPfS1_"))(&left, &right);
    CHECK(left == 0.25f);
    CHECK(right == 0.75f);

    /* Exit-request sections nest and never underflow. */
    typedef void (*void_fn)(void);
    void_fn enter = (void_fn)botw_hle_lookup(&g_hle, "_ZN2nn2oe31EnterExitRequestHandlingSectionEv");
    void_fn leave = (void_fn)botw_hle_lookup(&g_hle, "_ZN2nn2oe31LeaveExitRequestHandlingSectionEv");
    enter();
    enter();
    CHECK(g_state.exit_request_sections == 2);
    leave();
    leave();
    leave();
    CHECK(g_state.exit_request_sections == 0);

    /* Setters that only store state. */
    typedef void (*bool_fn)(bool);
    ((bool_fn)botw_hle_lookup(&g_hle, "_ZN2nn2oe28SetResumeNotificationEnabledEb"))(true);
    CHECK(g_state.resume_notification == 1);
    ((bool_fn)botw_hle_lookup(&g_hle, "_ZN2nn2oe38SetUserInactivityDetectionTimeExtendedEb"))(true);
    CHECK(g_state.inactivity_extended == 1);
    ((void_fn)botw_hle_lookup(&g_hle, "_ZN2nn2oe17FinishStartupLogoEv"))();
    CHECK(g_state.startup_logo_finished == 1);

    /* The performance configuration returns nn::Result 0 in w0 and switches
     * the mode it was told about. */
    typedef uint32_t (*perf_cfg_fn)(unsigned, int);
    CHECK(((perf_cfg_fn)botw_hle_lookup(&g_hle, "_ZN2nn2oe27SetPerformanceConfigurationENS0_15PerformanceModeEi"))(
              BOTW_OE_PERF_NORMAL, 7) == 0);
    CHECK(g_state.performance_mode == BOTW_OE_PERF_NORMAL);

    CHECK(g_state.calls > 0);

    /* Anything else still takes the stub path. */
    CHECK(botw_hle_lookup(&g_hle, "_ZN2nn2oe10NoSuchThingEv") == nullptr);
    CHECK(botw_hle_resolve(&g_hle, "_ZN2nn2oe10NoSuchThingEv") ==
          (void*)(uintptr_t)&botw_hle_stub);

    if (g_fail) {
        std::printf("guest_hle_nn_test: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("guest_hle_nn_test: ok (%u nn::oe names, state-driven answers, clean misses)\n",
                BOTW_HLE_OE_COUNT);
    return 0;
}
