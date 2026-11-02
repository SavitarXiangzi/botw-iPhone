#include "guest_hle_nn.h"

#include <stdbool.h>
#include <string.h>

/* The implementations are registered under their Itanium-mangled names,
 * because that is what the rewritten image imports.  Keeping the strings in
 * one table makes a typo obvious: it shows up as a stub in --hle output, and
 * tests/guest_hle_nn_test.cpp looks each name up individually.
 *
 * The digit after `_ZN2nn2oe` is the length of the member name.  Recount it
 * when editing: a wrong length does not fail to compile, it just stops
 * matching. */
typedef struct {
    const char* name;
    void* fn;
} oe_entry;

static botw_hle_nn_state g_default_state;
static botw_hle_nn_state* g_state = &g_default_state;

void botw_hle_nn_reset(botw_hle_nn_state* state) {
    if (!state) state = &g_default_state;
    memset(state, 0, sizeof *state);
    state->operation_mode = BOTW_OE_MODE_HANDHELD;
    state->performance_mode = BOTW_OE_PERF_BOOST;
    state->focus_state = BOTW_OE_FOCUS_FOCUSED;
    state->desired_language = BOTW_OE_LANG_AMERICAN_ENGLISH;
    state->volume[0] = 1.0f;
    state->volume[1] = 1.0f;
}

botw_hle_nn_state* botw_hle_nn_default(void) {
    return &g_default_state;
}

/* --- nn::oe ------------------------------------------------------------- */

static void oe_Initialize(void) {
    g_state->calls++;
}

static void oe_FinishStartupLogo(void) {
    g_state->calls++;
    g_state->startup_logo_finished = 1u;
}

static void oe_EnableRecording(void) {
    g_state->calls++;
    g_state->recording_enabled = 1u;
}

static void oe_SetCpuBoostMode(unsigned mode) {
    g_state->calls++;
    g_state->cpu_boost_mode = mode;
}

static uint32_t oe_GetOperationMode(void) {
    g_state->calls++;
    return g_state->operation_mode;
}

static uint32_t oe_GetPerformanceMode(void) {
    g_state->calls++;
    return g_state->performance_mode;
}

static uint32_t oe_GetCurrentFocusState(void) {
    g_state->calls++;
    return g_state->focus_state;
}

static uint32_t oe_GetDesiredLanguage(void) {
    g_state->calls++;
    return g_state->desired_language;
}

static void oe_SetFocusHandlingMode(unsigned mode) {
    g_state->calls++;
    g_state->focus_handling_mode = mode;
}

/* The launch parameter is the home-menu hand-off (for example "start this
 * game with the DLC menu open").  A port has none, so the answer is a clean
 * "nothing to pop": false, with the size written as 0.  Returning success
 * with a zeroed buffer would look like a real parameter of length zero and
 * the caller would try to parse it. */
static bool oe_TryPopLaunchParameter(uint64_t* out_size, void* buf, uint64_t size) {
    (void)buf;
    (void)size;
    g_state->calls++;
    g_state->launch_parameter_pops++;
    if (out_size) *out_size = 0;
    return false;
}

/* Same shape for the notification queue. */
static bool oe_TryPopNotificationMessage(uint32_t* out_message) {
    g_state->calls++;
    g_state->notification_pops++;
    if (out_message) *out_message = 0;
    return false;
}

static void oe_GetExpectedVolumeBalance(float* out_left, float* out_right) {
    g_state->calls++;
    if (out_left) *out_left = g_state->volume[0];
    if (out_right) *out_right = g_state->volume[1];
}

static void oe_SetExpectedVolumeBalance(float left, float right) {
    g_state->calls++;
    g_state->volume[0] = left;
    g_state->volume[1] = right;
}

static uint32_t oe_SetPerformanceConfiguration(unsigned mode, int config) {
    (void)config;
    g_state->calls++;
    g_state->performance_mode = (mode == BOTW_OE_PERF_BOOST) ? BOTW_OE_PERF_BOOST
                                                             : BOTW_OE_PERF_NORMAL;
    return 0; /* nn::Result success */
}

static void oe_SetResumeNotificationEnabled(bool enabled) {
    g_state->calls++;
    g_state->resume_notification = enabled ? 1u : 0u;
}

static void oe_SetOperationModeChangedNotificationEnabled(bool enabled) {
    g_state->calls++;
    g_state->operation_mode_notification = enabled ? 1u : 0u;
}

static void oe_SetPerformanceModeChangedNotificationEnabled(bool enabled) {
    g_state->calls++;
    g_state->performance_mode_notification = enabled ? 1u : 0u;
}

static void oe_SetUserInactivityDetectionTimeExtended(bool extended) {
    g_state->calls++;
    g_state->inactivity_extended = extended ? 1u : 0u;
}

static void oe_EnterExitRequestHandlingSection(void) {
    g_state->calls++;
    g_state->exit_request_sections++;
}

static void oe_LeaveExitRequestHandlingSection(void) {
    g_state->calls++;
    if (g_state->exit_request_sections) g_state->exit_request_sections--;
}

static const oe_entry g_oe_entries[BOTW_HLE_OE_COUNT] = {
    { "_ZN2nn2oe10InitializeEv", (void*)(uintptr_t)&oe_Initialize },
    { "_ZN2nn2oe15EnableRecordingEv", (void*)(uintptr_t)&oe_EnableRecording },
    { "_ZN2nn2oe15SetCpuBoostModeENS0_12CpuBoostModeE", (void*)(uintptr_t)&oe_SetCpuBoostMode },
    { "_ZN2nn2oe16GetOperationModeEv", (void*)(uintptr_t)&oe_GetOperationMode },
    { "_ZN2nn2oe17FinishStartupLogoEv", (void*)(uintptr_t)&oe_FinishStartupLogo },
    { "_ZN2nn2oe18GetDesiredLanguageEv", (void*)(uintptr_t)&oe_GetDesiredLanguage },
    { "_ZN2nn2oe18GetPerformanceModeEv", (void*)(uintptr_t)&oe_GetPerformanceMode },
    { "_ZN2nn2oe20GetCurrentFocusStateEv", (void*)(uintptr_t)&oe_GetCurrentFocusState },
    { "_ZN2nn2oe20SetFocusHandlingModeENS0_17FocusHandlingModeE", (void*)(uintptr_t)&oe_SetFocusHandlingMode },
    { "_ZN2nn2oe21TryPopLaunchParameterEPmPvm", (void*)(uintptr_t)&oe_TryPopLaunchParameter },
    { "_ZN2nn2oe24GetExpectedVolumeBalanceEPfS1_", (void*)(uintptr_t)&oe_GetExpectedVolumeBalance },
    { "_ZN2nn2oe24SetExpectedVolumeBalanceEff", (void*)(uintptr_t)&oe_SetExpectedVolumeBalance },
    { "_ZN2nn2oe25TryPopNotificationMessageEPj", (void*)(uintptr_t)&oe_TryPopNotificationMessage },
    { "_ZN2nn2oe27SetPerformanceConfigurationENS0_15PerformanceModeEi", (void*)(uintptr_t)&oe_SetPerformanceConfiguration },
    { "_ZN2nn2oe28SetResumeNotificationEnabledEb", (void*)(uintptr_t)&oe_SetResumeNotificationEnabled },
    { "_ZN2nn2oe31EnterExitRequestHandlingSectionEv", (void*)(uintptr_t)&oe_EnterExitRequestHandlingSection },
    { "_ZN2nn2oe31LeaveExitRequestHandlingSectionEv", (void*)(uintptr_t)&oe_LeaveExitRequestHandlingSection },
    { "_ZN2nn2oe38SetUserInactivityDetectionTimeExtendedEb", (void*)(uintptr_t)&oe_SetUserInactivityDetectionTimeExtended },
    { "_ZN2nn2oe42SetOperationModeChangedNotificationEnabledEb", (void*)(uintptr_t)&oe_SetOperationModeChangedNotificationEnabled },
    { "_ZN2nn2oe44SetPerformanceModeChangedNotificationEnabledEb", (void*)(uintptr_t)&oe_SetPerformanceModeChangedNotificationEnabled },
};

int botw_hle_nn_register(botw_hle_table* hle, botw_hle_nn_state* state) {
    size_t i;

    if (!hle) return -1;
    if (!state) {
        botw_hle_nn_reset(&g_default_state);
        state = &g_default_state;
    }
    g_state = state;
    for (i = 0; i < BOTW_HLE_OE_COUNT; i++) {
        if (botw_hle_register(hle, g_oe_entries[i].name, g_oe_entries[i].fn) != 0) return -1;
    }
    return 0;
}
