/*
 *                                  CAN SPY
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
 *   + forwarding CAN frames between CAN1 and CAN2 in both directions.
 *   + filtering CAN frames
 *   + mirroring CAN frames on a serial BUS
 *   + injecting CAN frames received by the serial BUS
 *
 */

#include "libc/syscall.h"
#include "libc/types.h"
#include "libc/stdio.h"
#include "libc/string.h"

#include "generated/button.h"
#include "generated/led_red.h"
#include "generated/led_green.h"
#include "libcan.h"
#include "libusart.h"


/* devices */
device_t button, leds;
int desc_button, desc_leds;
usart_config_t usart_config;
usart_map_mode_t map_mode;
can_context_t can1_ctx, can2_ctx;


/* To avoid loosing CAN frames in case of suspension of the main task,
 * we let the local interrupt routine store the incoming frames in a
 * circular buffer, managed as a FIFO.
 */
#define CAN_BUFFER_SIZE 8

typedef struct {
  can_header_t head;
  can_data_t   body;
} can_frame_t;

can_frame_t can1_rx_buffer[CAN_BUFFER_SIZE];
can_frame_t can2_rx_buffer[CAN_BUFFER_SIZE];

/* indexes, modulo the buffer size, to manage the FIFO.
 * rx_in points to the first free entry and rx_out to the first CAN frame.
 * rx_in is increased by the ISR after an insert and if it reaches rx_out
 * we drop that frame (the oldest one) by increasing also rx_out.
 * rx_out is increased by the task after a retrieve.
 * The buffer is considered void if rx_out = rx_in.
 */
unsigned char can1_rx_in  = 0;
unsigned char can1_rx_out = 0;
unsigned char can2_rx_in  = 0;
unsigned char can2_rx_out = 0;


void inc_mod (unsigned char *index) {
  if (*index == CAN_BUFFER_SIZE-1) {
    *index = 0;
  } else {
    (*index)++;
  }
}

void buffer_can_frame(can_port_t port, can_fifo_t fifo) {
  mbed_error_t mret;
  switch (port) {
    case CAN_PORT_1:
      mret = can_receive(&can1_ctx, fifo, &can1_rx_buffer[can1_rx_in].head,
                                          &can1_rx_buffer[can1_rx_in].body);
      if (mret == CAN_ERROR_NONE) {
        inc_mod(&can1_rx_in);
        if (can1_rx_in == can1_rx_out) {
          // Oups the buffer is full ! We drop one entry.
          inc_mod(&can1_rx_out);
        }
      }
      break;
    case CAN_PORT_2:
      mret = can_receive(&can2_ctx, fifo, &can2_rx_buffer[can2_rx_in].head,
                                          &can2_rx_buffer[can2_rx_in].body);
      if (mret == CAN_ERROR_NONE) {
        inc_mod(&can2_rx_in);
        if (can2_rx_in == can2_rx_out) {
          inc_mod(&can2_rx_out);
        }
      }
      break;
    case CAN_PORT_3:
      break;
  }
}

/* If there is a new CAN frame available in the buffer on port "port"
 * retrieve it from the buffer and copy it at the places pointed by
 * "head" and "body" and return true.
 * if nothing is available, return false.
 */
bool retrieve_available_can_frame(can_port_t port,
                                  can_header_t *head,
                                  can_data_t   *body) {
  switch (port) {
    case CAN_PORT_1:
      if (can1_rx_in != can1_rx_out) {
        *head = can1_rx_buffer[can1_rx_out].head;
        *body = can1_rx_buffer[can1_rx_out].body;
        inc_mod(&can1_rx_out);
        return true;
      }
      break;
    case CAN_PORT_2:
      if (can2_rx_in != can2_rx_out) {
        *head = can2_rx_buffer[can2_rx_out].head;
        *body = can2_rx_buffer[can2_rx_out].body;
        inc_mod(&can2_rx_out);
        return true;
      }
      break;
    case CAN_PORT_3:
      break;
  }
  return false;
}


/*
 * Interruption Routine (ISR) used for the button
 */
volatile bool button_pressed = false;       /* set by ISR */

void exti_button_handler ()
{
   uint64_t        clock;
   static uint64_t last_isr_clk;
   e_syscall_ret   sret;

   /* get the time elapsed since boot */
   sret = sys_get_systick(&clock, PREC_MILLI);
   if (sret == SYS_E_DONE) {
       /* debouncing ... */
       if (clock - last_isr_clk > 20) {
          last_isr_clk = clock;
          button_pressed = true;
       }
   }
}

/*
 * Interruption Routine : this is called by the CAN ISR routine
 */
volatile can_port_t port_id  = CAN_PORT_1;  /* set by ISR */
volatile can_event_t last_event;            /* set by ISR */
volatile can_error_t can_error_nb;          /* set by ISR */
volatile uint32_t nb_IT      = 0;           /* set by ISR */
volatile bool emit_aborted   = false;       /* set by ISR */
volatile bool error_occurred = false;       /* set by ISR */
volatile can_fifo_t fifo     = CAN_FIFO_0;  /* set by ISR */
volatile uint32_t events[13] = { 0 };       /* set by ISR */

mbed_error_t can_event(can_event_t event, can_port_t port, can_error_t errcode)
{
    last_event = event;
    port_id    = port;
    nb_IT++;
    events[event]++;

   emit_aborted   = false;
   error_occurred = false;

    switch (event) {
      case CAN_EVENT_RX_FIFO0_MSG_PENDING:
          buffer_can_frame(port, CAN_FIFO_0);
          break;
      case CAN_EVENT_RX_FIFO0_FULL:
          buffer_can_frame(port, CAN_FIFO_0);
          buffer_can_frame(port, CAN_FIFO_0);
          buffer_can_frame(port, CAN_FIFO_0);
          break;
      case CAN_EVENT_RX_FIFO1_MSG_PENDING:
          buffer_can_frame(port, CAN_FIFO_1);
          break;
      case CAN_EVENT_RX_FIFO1_FULL:
          buffer_can_frame(port, CAN_FIFO_1);
          buffer_can_frame(port, CAN_FIFO_1);
          buffer_can_frame(port, CAN_FIFO_1);
          break;
      case CAN_EVENT_TX_MBOX0_ABORT:
      case CAN_EVENT_TX_MBOX1_ABORT:
      case CAN_EVENT_TX_MBOX2_ABORT:
          emit_aborted = true;
          can_error_nb = errcode;
          break;
      case CAN_EVENT_ERROR:
          error_occurred = true;
          can_error_nb = errcode;
          break;
      default:
          break;
    }
   return MBED_ERROR_NONE;
}


 /*
  * Dump CAN Frame
  */
void dump_CAN_frame (can_header_t *head, can_data_t *body)
{
  char buffer[10+8*3];
  int n = 0;

  // 1. ID in decimal
  if (head->IDE == CAN_ID_STD) {
    n = sprintf(buffer, "t@%i", head->id.std);
  } else {
    n = sprintf(buffer, "T@%i", head->id.ext);
  }

  // 2. length
  n = n + sprintf(buffer+n, " [%d]", head->DLC);

  // 3. raw data in hexa
  for (int i = 0; i < head->DLC; i++) {
    n = n + sprintf(buffer+n, " %02x", body->data[i]);
  }
  printf("%s\n",buffer);
}

/*******************************************************************************
 *  MAIN
 ******************************************************************************/

int _main(uint32_t my_id)
{
    e_syscall_ret   sret;

    printf("Hello, I'm the CANSPY task. My id is %x\n", my_id);

    /* Configuring the button GPIOs
     * See sample button app for details */

    /* Zeroing the structure to avoid improper values detected by the kernel */
    memset(&button, 0, sizeof(button));
    strncpy(button.name, "BUTTON", sizeof(button.name));

    button.gpio_num = 1;    /* Number of configured GPIO */

    button.gpios[0].kref.port = button_dev_infos.gpios[BUTTON].port;
    button.gpios[0].kref.pin  = button_dev_infos.gpios[BUTTON].pin;
    button.gpios[0].mask      = GPIO_MASK_SET_MODE | GPIO_MASK_SET_PUPD |
                                GPIO_MASK_SET_TYPE | GPIO_MASK_SET_SPEED|
                                GPIO_MASK_SET_EXTI;
    button.gpios[0].mode      = GPIO_PIN_INPUT_MODE;
    button.gpios[0].pupd      = GPIO_PULLDOWN;
    button.gpios[0].type      = GPIO_PIN_OTYPER_PP;
    button.gpios[0].speed     = GPIO_PIN_LOW_SPEED;
    button.gpios[0].exti_trigger = GPIO_EXTI_TRIGGER_RISE;
    button.gpios[0].exti_handler = (user_handler_t) exti_button_handler;

    sret = sys_init(INIT_DEVACCESS, &button, &desc_button);
    if (sret) {
        printf("Error: sys_init(button) %s\n", strerror(sret));
    } else {
        printf("sys_init(button) - success\n");
    }

    /*
     * Configuring the LED GPIOs. Note: the related clocks are automatically set
     * by the kernel.
     * We configure 2 GPIOs here corresponding to the STM32 Discovery F407 LEDs
     * See the datasheet of the board here for more information:
     * https://www.st.com/content/ccc/resource/technical/document/user_manual/70/fe/4a/3f/e7/e1/4f/7d/DM00039084.pdf/files/DM00039084.pdf/jcr:content/translations/en.DM00039084.pdf
     *
     * NOTE: since we do not need an ISR handler for the LED gpios, we do not
     * configure it (we only need to synchronously set the LEDs)
     */
    memset(&leds, 0, sizeof(leds));
    strncpy(leds.name, "LEDs", sizeof(leds.name));

    leds.gpio_num = 2;    /* Number of configured GPIO */

    leds.gpios[0].kref.port = led_red_dev_infos.gpios[LED_RED].port;
    leds.gpios[0].kref.pin  = led_red_dev_infos.gpios[LED_RED].pin;
    leds.gpios[0].mask      = GPIO_MASK_SET_MODE | GPIO_MASK_SET_PUPD |
                              GPIO_MASK_SET_TYPE | GPIO_MASK_SET_SPEED;
    leds.gpios[0].mode      = GPIO_PIN_OUTPUT_MODE;
    leds.gpios[0].pupd      = GPIO_PULLDOWN;
    leds.gpios[0].type      = GPIO_PIN_OTYPER_PP;
    leds.gpios[0].speed     = GPIO_PIN_HIGH_SPEED;

    leds.gpios[1].kref.port = led_green_dev_infos.gpios[LED_GREEN].port;
    leds.gpios[1].kref.pin  = led_green_dev_infos.gpios[LED_GREEN].pin;
    leds.gpios[1].mask      = GPIO_MASK_SET_MODE | GPIO_MASK_SET_PUPD |
                              GPIO_MASK_SET_TYPE | GPIO_MASK_SET_SPEED;
    leds.gpios[1].mode      = GPIO_PIN_OUTPUT_MODE;
    leds.gpios[1].pupd      = GPIO_PULLDOWN;
    leds.gpios[1].type      = GPIO_PIN_OTYPER_PP;
    leds.gpios[1].speed     = GPIO_PIN_HIGH_SPEED;

    sret = sys_init(INIT_DEVACCESS, &leds, &desc_leds);
    if (sret) {
        printf("Error: sys_init(leds) %s\n", strerror(sret));
    } else {
        printf("sys_init(leds) - success\n");
    }

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
     * Configuring the two Controller Area Network (CAN) peripherals.
     */

     mbed_error_t mret;

     /* Zeroing the structure to avoid improper values detected by the kernel */
     memset(&can1_ctx, 0, sizeof(can_context_t));
     memset(&can2_ctx, 0, sizeof(can_context_t));
     can1_ctx.id = CAN_PORT_1;
     can2_ctx.id = CAN_PORT_2;


     /* CAN 1 */
     can1_ctx.mode = CAN_MODE_NORMAL;
     can1_ctx.access = CAN_ACCESS_IT;
     can1_ctx.timetrigger  = false;    /* Time triggered communication mode */
     can1_ctx.autobusoff   = false;    /* automatic bus-off ? */
     can1_ctx.autowakeup   = true;     /* wake up from sleep on event ? */
     can1_ctx.autoretrans  = true;     /* auto retransmission ? */
     can1_ctx.rxfifolocked = false;    /* lock Rx Fifo locked against overrun ?*/
     can1_ctx.txfifoprio   = true;     /* Tx FIFO respects chronology ? */
     can1_ctx.bit_rate     = CAN_SPEED_1MHZ;

     /* CAN 2 */
     can2_ctx.mode = CAN_MODE_NORMAL;
     can2_ctx.access = CAN_ACCESS_IT;
     can2_ctx.timetrigger  = false;    /* Time triggered communication mode */
     can2_ctx.autobusoff   = false;    /* automatic bus-off ? */
     can2_ctx.autowakeup   = true;     /* wake up from sleep on event ? */
     can2_ctx.autoretrans  = true;     /* auto retransmission ? */
     can2_ctx.rxfifolocked = false;    /* lock Rx Fifo locked against overrun ?*/
     can2_ctx.txfifoprio   = true;     /* Tx FIFO respects chronology ? */
     can2_ctx.bit_rate     = CAN_SPEED_1MHZ;

     for (int i = 1; i < 3; i++) {
       if (i == 1) {
          mret = can_declare(&can1_ctx);
       } else {
          mret = can_declare(&can2_ctx);
       }
       if (mret) {
           printf("Error: sys_init(CAN%d) %d\n", i, mret);
       } else {
           printf("sys_init(CAN%d) - success\n",i);
       }
     }

    /*
     * Devices and ressources registration is finished !
     */

    sret = sys_init(INIT_DONE);
    if (sret) {
        printf("Error sys_init DONE: %s\n", strerror(sret));
        return 1;
    }
    printf("init done.\n");

    /*
     * Devices' initialization
     */

    for (int i = 1; i < 3; i++) {
       if (i == 1) {
          mret = can_initialize(&can1_ctx);
       } else {
          mret = can_initialize(&can2_ctx);
       }
       if (mret) {
          printf("Error during CAN%d initialization %d\n", i, mret);
       } else {
          printf("CAN%d initialized with success\n",i);
       }
    }

    for (int i = 1; i < 3; i++) {
       if (i == 1) {
          mret = can_start(&can1_ctx);
       } else {
          mret = can_start(&can2_ctx);
       }
       if (mret) {
          printf("Error during CAN%d start %d\n", i, mret);
       } else {
          printf("CAN%d started with success\n",i);
       }
    }

    sret = usart_init(&usart_config);
    if (sret) {
      printf("Error during USART initialization %d\n", sret);
    } else {
      printf("USART initialized with success\n");
    }


        /*************
         * Main task *
         *************/

    bool verbose  = false;       /* Shall we report frames to the SLCAN ? */

    /* Led state */
    typedef enum {OFF = 0, ON = 1} led_state_t;
    led_state_t red_state   = OFF; /* Red   is ON iff in verbose mode */
    led_state_t green_state = OFF; /* Green is ON while a frame is processed */

    can_header_t head;
    can_data_t body;

    while (1) {

        /* Manage button for verbose mode */
        if (button_pressed == true) {
            if (verbose == false) {
                verbose = true;
                red_state = ON;
            } else {
                verbose = false;
                red_state = OFF;
            }
            sret = sys_cfg(CFG_GPIO_SET, (uint8_t) leds.gpios[0].kref.val, red_state);
            if (sret != SYS_E_DONE) {
                printf("sys_cfg(red led): failed\n");
                return 1;
            }
        }

        /* if there is an error on the CAN bus, signal it */
        if (emit_aborted) {
           emit_aborted = false;
           verbose = false;
           printf("CAN event, emit aborted: %d ! \n", can_error_nb);
        } else
        if (error_occurred) {
          error_occurred = false;
          verbose = false;
          printf("CAN event, error: %d\n", can_error_nb);
        }

        /* if there is a frame to collect, let's do it */
        for (can_port_t port = CAN_PORT_1; port < CAN_PORT_3; port++ ) {
          if (retrieve_available_can_frame(port, &head, &body) == true) {

            /* signal traffic */
            green_state = ON;
            sret = sys_cfg(CFG_GPIO_SET, (uint8_t) leds.gpios[1].kref.val, green_state);
            if (sret != SYS_E_DONE) {
               printf("sys_cfg(green led): failed\n");
              return 1;
            }

            /* 1. Mirror it to the serial port using the Serial Line CAN
                  interface (SLCAN) driver syntax. */
            if (verbose) {
              /* DEBUG ONLY */
              printf("CAN%d received (IT %d, FIFO %d):\n", port, nb_IT, fifo);
              dump_CAN_frame (&head, &body);

              char buffer[10+8*2+5];
              int n = 0;

              // 1. ID in decimal
              if (head.IDE == CAN_ID_STD) {
                if (head.RTR) {
                   n = sprintf(buffer, "r%03x", head.id.std);
                } else {
                   n = sprintf(buffer, "t%03x", head.id.std);
                }
              } else {
                if (head.RTR) {
                   n = sprintf(buffer, "R%08x", head.id.std);
                } else {
                   n = sprintf(buffer, "T%08x", head.id.std);
                }
              }

              // 2. length
              n = n + sprintf(buffer+n, "%01d", head.DLC);

              // 3. raw data in hexa
              for (int i = 0; i < head.DLC; i++) {
                n = n + sprintf(buffer+n, "%02x", body.data[i]);
              }
              n = n + sprintf(buffer+n, "\r");

              usart_write(usart_config.usart, buffer, n);
            }

            /* 2. Forward it to the other CAN bus */
            can_mbox_t *mbox = NULL;
            if (port == CAN_PORT_1) {
              mret = can_xmit(&can2_ctx, &head, &body, mbox);
            } else {
              mret = can_xmit(&can1_ctx, &head, &body, mbox);
            }
            if (mret) {
              printf("Error: CAN%d Xmit %d\n", port, mret);
            }
          }
        }

      green_state = OFF;
      sret = sys_cfg(CFG_GPIO_SET, (uint8_t) leds.gpios[1].kref.val, green_state);
      if (sret != SYS_E_DONE) {
          printf("sys_cfg(green led): failed\n");
          return 1;
      }

      /* Yield until the kernel awakes us for a new CAN frame */
      sys_sleep(SLEEP_MODE_DEEP, 100);
    }

    return 0;
}