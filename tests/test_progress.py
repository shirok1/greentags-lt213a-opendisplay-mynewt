"""Run the firmware's queue/progress functions with a deterministic fake clock."""
from pathlib import Path
import subprocess
import tempfile
import unittest


class ProgressTests(unittest.TestCase):
    def test_idle_wait_callback_and_slow_notification_progress(self):
        source = Path('apps/opendisplay/src/main.c').read_text()
        send = source[source.index('static int send_response('):source.index('static void command_done(')]
        run = source[source.index('static void __attribute__((noreturn)) run_events('):source.index('static void display_main(')]
        harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <setjmp.h>
#define OS_TICKS_PER_SEC 128
#define OS_TIMEOUT_NEVER UINT32_MAX
struct os_event { void (*ev_cb)(struct os_event *); };
struct os_eventq { int unused; };
struct command { uint32_t generation; };
static struct { uint8_t data[256]; size_t len; uint32_t generation, retries;
                struct os_event event; int finished, result; } tx;
static uint32_t now, last_checkin, send_time;
static unsigned polls, sends;
static jmp_buf done;
static struct os_eventq queue;
static void os_sanity_task_checkin(void *task) { (void)task; last_checkin=now; }
static struct os_eventq *os_eventq_dflt_get(void) { return &queue; }
static void os_eventq_put(struct os_eventq *q, struct os_event *e) { (void)q; (void)e; }
static void os_sem_pend(int *sem, uint32_t timeout) {
    (void)sem; assert(timeout==OS_TIMEOUT_NEVER);
    assert(now-last_checkin<120*OS_TICKS_PER_SEC);
    now+=send_time; ++sends;
}
static struct os_event *os_eventq_poll(struct os_eventq **q, int n, uint32_t timeout);
''' + send + run + r'''
static void callback(struct os_event *ev) {
    (void)ev; assert(last_checkin==now); /* 60 s wait must not age the callback. */
    now+=90*OS_TICKS_PER_SEC; /* Three bounded BUSY/cleanup waits. */
}
static struct os_event *os_eventq_poll(struct os_eventq **q, int n, uint32_t timeout) {
    static struct os_event ev={callback};
    assert(*q==&queue && n==1 && timeout==60*OS_TICKS_PER_SEC);
    assert(last_checkin==now); /* Includes completion and empty-queue paths. */
    now+=timeout;
    if (++polls==3) longjmp(done,1);
    return polls==1 ? NULL : &ev;
}
int main(void) {
    if (!setjmp(done)) run_events(&queue);
    struct command c={1}; uint8_t byte=0;
    now=last_checkin=0; send_time=5*OS_TICKS_PER_SEC;
    for (unsigned i=0;i<257;i++) { /* Full config at MTU 23 with backpressure. */
        assert(send_response(&byte,1,&c)==0);
        assert(last_checkin==now);
    }
    tx.result=-1;
    uint32_t before=last_checkin;
    assert(send_response(&byte,1,&c)==-1 && last_checkin==before && sends==258);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            c = Path(directory) / 'progress.c'
            c.write_text(harness)
            exe = Path(directory) / 'progress'
            subprocess.run(['cc', '-std=c99', '-Wall', '-Wextra', '-Werror', str(c), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)
