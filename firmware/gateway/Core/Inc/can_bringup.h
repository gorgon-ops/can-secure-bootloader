#ifndef CAN_BRINGUP_H
#define CAN_BRINGUP_H

#include "can_config.h"

/* NODE1 has the lower ID and must win arbitration when the two contend. */
#define CAN_BRINGUP_ID_NODE1 0x100U
#define CAN_BRINGUP_ID_NODE2 0x200U

void CanBringup_Init(void);
void CanBringup_Task(void);

#endif
