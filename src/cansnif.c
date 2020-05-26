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

#include "libusart.h"
/* devices */
usart_config_t usart_config;
usart_map_mode_t map_mode;

/*******************************************************************************
 *  MAIN
 ******************************************************************************/

int _main(uint32_t my_id)
{
    e_syscall_ret   sret;

    printf("Hello, I'm the CANSNIF task. My id is %x\n", my_id);

    /*
     * Configuring the USART to emit the frames in ASCII toward an slcand
     */
    memset(&usart_config, 0, sizeof(usart_config_t));

    /* for sys init (or "early init") */
    /* getc and putc handler in config are set by thE USART driver */
    usart_config.usart = 2, /* in case one is reserved for printf */
    usart_config.mode = UART;
    static cb_usart_getc_t getc_ptr = NULL;
    static cb_usart_putc_t putc_ptr = NULL;
    usart_config.callback_usart_getc_ptr = &getc_ptr;
    usart_config.callback_usart_putc_ptr = &putc_ptr;

    /* for start (or "init") */
    usart_config.set_mask = USART_SET_ALL;
    usart_config.baudrate = 115200; // bit/s
    usart_config.word_length = USART_CR1_M_8;
    usart_config.parity = USART_CR1_PCE_DIS;
    usart_config.stop_bits = USART_CR2_STOP_1BIT;
    usart_config.hw_flow_control = USART_CR3_CTSE_CTS_DIS | USART_CR3_RTSE_RTS_DIS;
    usart_config.options_cr1 = USART_CR1_TE_EN | USART_CR1_RE_EN
                               | USART_CR1_RXNEIE_EN;
    usart_config.options_cr2 = 0;
    usart_config.callback_irq_handler = NULL;

    sret = usart_early_init(&usart_config, USART_MAP_AUTO);
    if (sret) {
        printf("Error: sys_init(USART) %s\n", strerror(sret));
    } else {
        printf("sys_init(USART) - success\n");
    }

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

    sret = usart_init(&usart_config);
    if (sret) {
      printf("Error during USART initialization %d\n", sret);
    } else {
      printf("USART initialized with success\n");
    }


        /*************
         * Main task *
         *************/

    bool lost_frame = false;

    while (1) {

        /* If frames where lost in buffering, signal it */
        if (lost_frame) {
          lost_frame = false;
          printf("At least one frame was lost.\n");
        }

        logsize_t size;
        char text[128];

        /* if there is a frame to collect, let's do it */
        size = sizeof(text);
        sret = sys_ipc(IPC_RECV_ASYNC, &spy_id, &size, text);
        switch (sret) {
          case SYS_E_DENIED:
              printf("is not allowed to communicate with CANSPY\n");
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
              usart_write(usart_config.usart, text, size);
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
