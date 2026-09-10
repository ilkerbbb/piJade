#ifndef IDLETIMER_H_
#define IDLETIMER_H_

#include <stdbool.h>
#include <stdint.h>

void idletimer_init(void);
// BBB-AIRGAP: libjade must request idle-task shutdown before blocking teardown work starts.
void idletimer_request_stop(void);
void idletimer_stop(void);
void idletimer_set_min_timeout_secs(uint16_t min_timeout_secs);
bool idletimer_register_activity(bool is_ui);
// BBB-AIRGAP: call after anything that shortens the wait the idle task is currently sleeping out.
void idletimer_recheck(void);

#endif /* IDLETIMER_H_ */
