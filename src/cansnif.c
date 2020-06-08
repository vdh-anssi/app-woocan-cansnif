/*
 *                              CAN SPY's SNIFFER
 *
 * This is strongly inspired by the work of Arnaud Lebrun and Jonathan-
 * Christopher Demay, published in 2016, at Airbus Defense and Space.
 *
 * see their paper "CANSPY: a Platform for Auditing CAN Devices".
 *
 * A CANSPY is typically installed as a man-in-the-middle device between a CAN
 * network and a targeted ECU or as a gateway between two CAN networks.
 *
 * It provides the following services :
 *   + mirroring CAN frames on a serial BUS
 *
 */

#include "libc/syscall.h"
#include "libc/types.h"
#include "libc/stdio.h"
#include "libc/string.h"
#include "libconsole.h"

/*******************************************************************************
 *  MAIN
 ******************************************************************************/

int _main(uint32_t my_id)
{
    e_syscall_ret   sret;

    printf("Hello, I'm the CANSNIF task. My id is %x\n", my_id);

   /*
    * Configuring the communication link to the CAN SPY task.
    */
    uint8_t spy_id;
    sret = sys_init(INIT_GETTASKID, "CANSPY", &spy_id);
    if (sret != SYS_E_DONE) {
        printf("Error: couldn't retrieve CANSPY's task id\n");
    } else {
        printf("sees CANSPY as %d\n", spy_id);
    }

    /* Declare the console to the kernel */
    mbed_error_t mbed_err;
    mbed_err = console_early_init(1, 115200);
    if (mbed_err) {
        printf("Error: in console early init: %d\n", mbed_err);
    } else {
        printf("Console early init - success\n");
    }


    /*
     * Device's and ressources registrations are finished !
     */

    sret = sys_init(INIT_DONE);
    if (sret) {
        printf("Error sys_init DONE: %s\n", strerror(sret));
        return 1;
    }
    printf("init done.\n");

    /*
     * Device's initialization
     */
    mbed_err = console_init();
    if (mbed_err) {
       printf("Error: in console init: %d\n", mbed_err);
    } else {
       printf("Console init - success\n");
    }




        /*************
         * Main task *
         *************/

    bool lost_frame = false;
    bool just_once  = true;

    while (1) {

        /* If frames where lost in buffering, signal it */
        if (lost_frame) {
          lost_frame = false;
          printf("At least one frame was lost.\n");
        }

        logsize_t size;
        char text[128];

        if (just_once) {
          console_log("CAN sniffer is ready to transmit\n");
          printf("says 'I am ready to transmit !'\n");
          just_once = false;
        }

        /* if there is a frame to collect, let's do it */
        size = sizeof(text);
        sret = sys_ipc(IPC_RECV_SYNC, &spy_id, &size, text);
        switch (sret) {
          case SYS_E_DENIED:
              printf("is not allowed to communicate with CANSPY\n");
              console_log("CANsnif is not allowed to communicate with CANspy\n");
              break;
          case SYS_E_INVAL:
              printf("IPC arguments are invalid\n");
              break;
          case SYS_E_MAX:
              printf("Unkown IPC error\n");
              break;
          case SYS_E_DONE:
             /* 1. Mirror the frame to the serial port */
              text[size] = 0; /* Needed by printf at least */
              console_log("%s\n", text);
              break;
          case SYS_E_BUSY:
          /* Do nothing */
          break;
        }

        /* if there are commands comming from above, treat them */


        /* Yield until the kernel awakes us for a new CAN frame */
        sys_sleep(SLEEP_MODE_DEEP, 100);
    }

    return 0;
}
