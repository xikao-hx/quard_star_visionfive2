#ifndef __UART_H__
#define __UART_H__

void serial_init(void);
int serial_print(char *fmt, ...);
void uart_poll_init(void);
void uart_poll_puts(const char *str);

#endif
