/* nn::oe (operating environment) HLE.
 *
 * These are the 20 nn::oe imports in the 1.6.0 main image.  Every one of them
 * answers a question about the console: which mode it is in, which language
 * the user picked, whether the app is focused, how loud the two outputs are.
 * A shared zero-return stub already "works" for most of them because 0 is a
 * legal answer, but it is the wrong answer: 0 means handheld + American
 * English + not focused.  This layer holds explicit state instead, so the
 * host can drive those answers and a test can prove they come back out.
 *
 * Everything here is plain data and no OS calls.  The iOS shell owns the
 * state; nothing in this file knows what a UIWindow is.
 *
 * ABI notes:
 *   - nn::Result is a 4-byte struct; it comes back in w0, and 0 is success.
 *   - nn::oe::*Mode / LanguageCode / FocusState are 4-byte enums, in w0.
 *   - bool returns use w0 with 0/1.
 *   - float arguments use s0/s1, identical on Horizon and Apple.
 *   - No function here takes or returns a by-value struct with more than one
 *     member, so the Horizon and AAPCS64 calling conventions agree.
 */
#ifndef BOTW_GUEST_HLE_NN_H
#define BOTW_GUEST_HLE_NN_H

#include <stdint.h>

#include "guest_hle.h"

#ifdef __cplusplus
extern "C" {
#endif

/* nn::oe::OperationMode */
#define BOTW_OE_MODE_HANDHELD 0u
#define BOTW_OE_MODE_CONSOLE  1u

/* nn::oe::PerformanceMode */
#define BOTW_OE_PERF_NORMAL 0u
#define BOTW_OE_PERF_BOOST  1u

/* nn::oe::FocusState */
#define BOTW_OE_FOCUS_NOT_FOCUSED 0u
#define BOTW_OE_FOCUS_FOCUSED     1u

/* nn::oe::LanguageCode.  Values 0..16 are the ones the 1.6.0 SDK knows; 17
 * and up were added later and are not exposed here. */
#define BOTW_OE_LANG_AMERICAN_ENGLISH        0u
#define BOTW_OE_LANG_BRITISH_ENGLISH         1u
#define BOTW_OE_LANG_JAPANESE                2u
#define BOTW_OE_LANG_FRENCH                  3u
#define BOTW_OE_LANG_GERMAN                  4u
#define BOTW_OE_LANG_LATIN_AMERICAN_SPANISH  5u
#define BOTW_OE_LANG_SPANISH                 6u
#define BOTW_OE_LANG_ITALIAN                 7u
#define BOTW_OE_LANG_DUTCH                   8u
#define BOTW_OE_LANG_CANADIAN_FRENCH         9u
#define BOTW_OE_LANG_PORTUGUESE             10u
#define BOTW_OE_LANG_RUSSIAN                11u
#define BOTW_OE_LANG_KOREAN                 12u
#define BOTW_OE_LANG_TRADITIONAL_CHINESE    13u
#define BOTW_OE_LANG_SIMPLIFIED_CHINESE     14u
#define BOTW_OE_LANG_BRAZILIAN_PORTUGUESE   15u
#define BOTW_OE_LANG_POLISH                 16u

typedef struct {
    /* Answers the game polls. */
    unsigned operation_mode;      /* BOTW_OE_MODE_* */
    unsigned performance_mode;    /* BOTW_OE_PERF_* */
    unsigned focus_state;         /* BOTW_OE_FOCUS_* */
    unsigned desired_language;    /* BOTW_OE_LANG_* */
    float    volume[2];           /* expected balance of the two outputs */

    /* Settings the game pushes at us.  Stored, never acted on: the console
     * has no equivalent of "boost the CPU", and a phone has no startup logo. */
    unsigned focus_handling_mode;
    unsigned cpu_boost_mode;
    unsigned resume_notification;
    unsigned operation_mode_notification;
    unsigned performance_mode_notification;
    unsigned inactivity_extended;
    unsigned recording_enabled;
    unsigned startup_logo_finished;

    /* Exit-request sections nest; leaving one too many is a guest bug we want
     * to see rather than silently absorb. */
    unsigned exit_request_sections;

    /* Observability.  A port that "runs" while never asking for a language is
     * not the same as one that asked and got the right answer. */
    unsigned long calls;
    unsigned long launch_parameter_pops;
    unsigned long notification_pops;
} botw_hle_nn_state;

/* Defaults: handheld, boost, focused, American English, unity volume. */
void botw_hle_nn_reset(botw_hle_nn_state* state);

/* The singleton used when register() is given NULL. */
botw_hle_nn_state* botw_hle_nn_default(void);

/* Registers the nn::oe implementations in `hle`.  `state` may be NULL to use
 * the singleton.  Returns 0, or -1 with hle->error set. */
int botw_hle_nn_register(botw_hle_table* hle, botw_hle_nn_state* state);

/* How many nn::oe names this layer claims.  The test asserts it equals the
 * number of nn::oe imports in the real image. */
#define BOTW_HLE_OE_COUNT 20u

#ifdef __cplusplus
}
#endif
#endif /* BOTW_GUEST_HLE_NN_H */
