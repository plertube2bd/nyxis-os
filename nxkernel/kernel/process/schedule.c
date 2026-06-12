#include "schedule.h"

#include "kernel/process/process.h"
#include "kernel/paging/paging.h"
#include "nyxis.h"

void round_robin_schedule(void) {
    if (!current_process)
        return;

    process_t* proc = current_process->next ? current_process->next : process_list;
    while (proc && proc != current_process) {
        if (proc->state == PROCESS_READY) {
            if (current_process->state == PROCESS_RUNNING) {
                current_process->state = PROCESS_READY;
            }
            process_switch(proc);
            return;
        }
        proc = proc->next ? proc->next : process_list;
    }
}

// Simple round-robin scheduler
void schedule(void) {
    round_robin_schedule();
}