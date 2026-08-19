#ifndef NS16550_H_
#define NS16550_H_

#include <stdint.h>

void uartinit(void);
int uart_getchar_nonblock(void);    
int uart_getchar_block(void);       
void uartputc(int c);
int uart_puts(const char *str, unsigned short len);      // 异步阻塞发送字符串
void uartputc_sync(int c);
int uartputs_sync(const char *str, unsigned short len);  // 同步阻塞发送字符串
int uartgets_sync(char *str, unsigned short len);        // 异步阻塞读
int uart_gets(char *str, unsigned short len);            // 同步阻塞读
void uartintr(int irq, void *data);

#endif /* NS16550_H_ */