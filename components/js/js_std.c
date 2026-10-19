/*
 * Watch OS — minimal QuickJS "std" helpers.
 *
 * In this QuickJS version Math, JSON, Date, Map, Set, Promise, Reflect and
 * Proxy are all registered by the core engine (quickjs.c), so we do NOT
 * compile quickjs-libc.c (which would also pull in a full POSIX "os" module
 * that has no place on a FreeRTOS sandbox).  This file provides only the few
 * helpers apps actually need:
 *   - js_std_add_helpers(): console.log, global print, performance.now
 *   - js_std_dump_error():   print an uncaught exception
 *   - js_std_loop():         drain pending promise jobs
 */
#include <stdio.h>
#include <stdint.h>

#include "esp_timer.h"

#include "quickjs/quickjs.h"

/* print(...) — stringify each argument to stdout. */
static JSValue js_print(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv)
{
    int i;
    const char *str;
    (void)this_val;

    for (i = 0; i < argc; i++) {
        if (i != 0)
            putchar(' ');
        str = JS_ToCString(ctx, argv[i]);
        if (!str)
            return JS_EXCEPTION;
        fputs(str, stdout);
        JS_FreeCString(ctx, str);
    }
    putchar('\n');
    return JS_UNDEFINED;
}

/* console.log(...) — same as print, but flushes so log lines survive a crash. */
static JSValue js_console_log(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    JSValue ret = js_print(ctx, this_val, argc, argv);
    fflush(stdout);
    return ret;
}

/* performance.now() — monotonic milliseconds (ESP timer). */
static JSValue js_performance_now(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    return JS_NewFloat64(ctx, (double)esp_timer_get_time() / 1000.0);
}

void js_std_add_helpers(JSContext *ctx, int argc, char **argv)
{
    JSValue global_obj, console, performance;
    (void)argc; (void)argv;

    global_obj = JS_GetGlobalObject(ctx);

    console = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, console, "log",
                      JS_NewCFunction(ctx, js_console_log, "log", 1));
    JS_SetPropertyStr(ctx, global_obj, "console", console);

    performance = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, performance, "now",
                      JS_NewCFunction(ctx, js_performance_now, "now", 0));
    JS_SetPropertyStr(ctx, global_obj, "performance", performance);

    JS_SetPropertyStr(ctx, global_obj, "print",
                      JS_NewCFunction(ctx, js_print, "print", 1));

    JS_FreeValue(ctx, global_obj);
}

void js_std_dump_error(JSContext *ctx)
{
    JSValue exception_val;
    const char *str;

    exception_val = JS_GetException(ctx);
    str = JS_ToCString(ctx, exception_val);
    if (str) {
        fprintf(stderr, "Uncaught JS exception: %s\n", str);
        JS_FreeCString(ctx, str);
    } else {
        fprintf(stderr, "Uncaught JS exception (non-string value)\n");
    }
    JS_FreeValue(ctx, exception_val);
}

/* Run pending promise jobs.  Called periodically from the app loop; there is
 * no host poll function, so a single drain is enough per call. */
void js_std_loop(JSContext *ctx)
{
    int err;

    for (;;) {
        err = JS_ExecutePendingJob(JS_GetRuntime(ctx), NULL);
        if (err <= 0) {
            if (err < 0)
                js_std_dump_error(ctx);
            break;
        }
    }
}
