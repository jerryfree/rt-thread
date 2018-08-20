/*
 * File      : at_cli.c
 * This file is part of RT-Thread RTOS
 * COPYRIGHT (C) 2006 - 2018, RT-Thread Development Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Change Logs:
 * Date           Author       Notes
 * 2018-04-02     armink       first version
 */

#include <at.h>
#include <stdio.h>
#include <string.h>

#include <rtthread.h>
#include <rtdevice.h>
#include <rthw.h>

#ifdef AT_USING_CLI

#define AT_CLI_FIFO_SIZE                      256

static struct rt_semaphore console_rx_notice;
static struct rt_ringbuffer *console_rx_fifo = RT_NULL;
static rt_err_t (*odev_rx_ind)(rt_device_t dev, rt_size_t size) = RT_NULL;

#ifdef AT_USING_CLIENT
static struct rt_semaphore client_rx_notice;
static struct rt_ringbuffer *client_rx_fifo = RT_NULL;
#endif 

static char console_getchar(void)
{
    char ch;

    rt_sem_take(&console_rx_notice, RT_WAITING_FOREVER);
    rt_ringbuffer_getchar(console_rx_fifo, (rt_uint8_t *)&ch);

    return ch;
}

static rt_err_t console_getchar_rx_ind(rt_device_t dev, rt_size_t size)
{
    uint8_t ch;
    rt_size_t i;

    for (i = 0; i < size; i++)
    {
        /* read a char */
        if (rt_device_read(dev, 0, &ch, 1))
        {
            rt_ringbuffer_put_force(console_rx_fifo, &ch, 1);
            rt_sem_release(&console_rx_notice);
        }
    }

    return RT_EOK;
}

void at_cli_init(void)
{
    rt_base_t int_lvl;
    rt_device_t console;

    rt_sem_init(&console_rx_notice, "at_cli_notice", 0, RT_IPC_FLAG_FIFO);

    /* create RX FIFO */
    console_rx_fifo = rt_ringbuffer_create(AT_CLI_FIFO_SIZE);
    /* created must success */
    RT_ASSERT(console_rx_fifo);

    int_lvl = rt_hw_interrupt_disable();
    console = rt_console_get_device();
    if (console)
    {
        /* backup RX indicate */
        odev_rx_ind = console->rx_indicate;
        rt_device_set_rx_indicate(console, console_getchar_rx_ind);
    }

    rt_hw_interrupt_enable(int_lvl);
}

void at_cli_deinit(void)
{
    rt_base_t int_lvl;
    rt_device_t console;

    int_lvl = rt_hw_interrupt_disable();
    console = rt_console_get_device();
    if (console && odev_rx_ind)
    {
        /* restore RX indicate */
        rt_device_set_rx_indicate(console, odev_rx_ind);
    }
    rt_hw_interrupt_enable(int_lvl);

    rt_sem_detach(&console_rx_notice);
    rt_ringbuffer_destroy(console_rx_fifo);
}

#ifdef AT_USING_SERVER
static void server_cli_parser(void)
{
    extern at_server_t at_get_server(void);

    at_server_t server = at_get_server();
    rt_base_t int_lvl;
    static rt_device_t device_bak;
    static char (*getchar_bak)(void);
    static char endmark_back[AT_END_MARK_LEN];

    /* backup server device and getchar function */
    {
        int_lvl = rt_hw_interrupt_disable();

        device_bak = server->device;
        getchar_bak = server->get_char;

        memset(endmark_back, 0x00, AT_END_MARK_LEN);
        memcpy(endmark_back, server->end_mark, strlen(server->end_mark));

        /* setup server device as console device */
        server->device = rt_console_get_device();
        server->get_char = console_getchar;

        memset(server->end_mark, 0x00, AT_END_MARK_LEN);
        server->end_mark[0] = '\r';

        rt_hw_interrupt_enable(int_lvl);
    }

    if (server)
    {
        rt_kprintf("======== Welcome to using RT-Thread AT command server cli ========\n");
        rt_kprintf("Input your at command for test server. Press 'ESC' to exit.\n");
        server->parser_entry(server);
    }
    else
    {
        rt_kprintf("AT client not initialized\n");
    }

    /* restore server device and getchar function */
    {
        int_lvl = rt_hw_interrupt_disable();

        server->device = device_bak;
        server->get_char = getchar_bak;

        memset(server->end_mark, 0x00, AT_END_MARK_LEN);
        memcpy(server->end_mark, endmark_back, strlen(endmark_back));

        rt_hw_interrupt_enable(int_lvl);
    }
}
#endif /* AT_USING_SERVER */

#ifdef AT_USING_CLIENT
static char client_getchar(void)
{
    char ch;

    rt_sem_take(&client_rx_notice, RT_WAITING_FOREVER);
    rt_ringbuffer_getchar(client_rx_fifo, (rt_uint8_t *)&ch);

    return ch;
}

static void at_client_entry(void *param)
{
    char ch;

    while(1)
    {
        ch = client_getchar();
        rt_kprintf("%c", ch);
    }
}

static rt_err_t client_getchar_rx_ind(rt_device_t dev, rt_size_t size)
{
    uint8_t ch;
    rt_size_t i;

    for (i = 0; i < size; i++)
    {
        /* read a char */
        if (rt_device_read(dev, 0, &ch, 1))
        {
            rt_ringbuffer_put_force(client_rx_fifo, &ch, 1);
            rt_sem_release(&client_rx_notice);
        }
    }

    return RT_EOK;
}
static void client_cli_parser(void)
{
#define ESC_KEY                 0x1B
#define BACKSPACE_KEY           0x08
#define DELECT_KEY              0x7F

    extern at_client_t rt_at_get_client(void);
    at_client_t  client = rt_at_get_client();
    char ch;
    char cur_line[FINSH_CMD_SIZE] = { 0 };
    rt_size_t cur_line_len = 0;
    static rt_err_t (*client_odev_rx_ind)(rt_device_t dev, rt_size_t size) = RT_NULL;
    rt_base_t int_lvl;
    rt_thread_t at_client;

    if (client)
    {
        /* backup client device RX indicate */
        {
            int_lvl = rt_hw_interrupt_disable();
            client_odev_rx_ind = client->device->rx_indicate;
            rt_device_set_rx_indicate(client->device, client_getchar_rx_ind);
            rt_hw_interrupt_enable(int_lvl);
        }

        rt_sem_init(&client_rx_notice, "at_cli_client_notice", 0, RT_IPC_FLAG_FIFO);
        client_rx_fifo = rt_ringbuffer_create(AT_CLI_FIFO_SIZE);

        at_client = rt_thread_create("at_cli_client", at_client_entry, RT_NULL, 512, 8, 8);
        if (client_rx_fifo && at_client)
        {
            rt_kprintf("======== Welcome to using RT-Thread AT command client cli ========\n");
            rt_kprintf("Cli will forward your command to server port(%s). Press 'ESC' to exit.\n", client->device->parent.name);
            rt_thread_startup(at_client);
            /* process user input */
            while (ESC_KEY != (ch = console_getchar()))
            {
                if (ch == BACKSPACE_KEY || ch == DELECT_KEY)
                {
                    if (cur_line_len)
                    {
                        cur_line[--cur_line_len] = 0;
                        rt_kprintf("\b \b");
                    }
                    continue;
                }
                else if (ch == '\r' || ch == '\n')
                {
                    /* execute a AT request */
                    if (cur_line_len)
                    {
                        rt_kprintf("\n");
                        at_exec_cmd(RT_NULL, "%.*s", cur_line_len, cur_line);
                    }
                    cur_line_len = 0;
                }
                else
                {
                    rt_kprintf("%c", ch);
                    cur_line[cur_line_len++] = ch;
                }
            }

            /* restore client device RX indicate */
            {
                int_lvl = rt_hw_interrupt_disable();
                rt_device_set_rx_indicate(client->device, client_odev_rx_ind);
                rt_hw_interrupt_enable(int_lvl);
            }

            rt_thread_delete(at_client);
            rt_sem_detach(&client_rx_notice);
            rt_ringbuffer_destroy(client_rx_fifo);
        }
        else
        {
            rt_kprintf("No mem for AT cli client\n");
        }
    }
    else
    {
        rt_kprintf("AT client not initialized\n");
    }
}
#endif /* AT_USING_CLIENT */

static void at(int argc, char **argv)
{
    if (argc < 2)
    {
        rt_kprintf("Please input 'at <server|client>' \n");
        return;
    }

    at_cli_init();

    if (!strcmp(argv[1], "server"))
    {
#ifdef AT_USING_SERVER
        server_cli_parser();
#else
        rt_kprintf("Not support AT server, please check your configure!\n");
#endif
    }
    else if (!strcmp(argv[1], "client"))
    {
#ifdef AT_USING_CLIENT
        client_cli_parser();
#else
        rt_kprintf("Not support AT client, please check your configure!\n");
#endif
    }
    else
    {
        rt_kprintf("Please input 'at <server|client>' \n");
    }

    at_cli_deinit();
}
MSH_CMD_EXPORT(at, RT-Thread AT component cli: at <server|client>);

#endif /* AT_USING_CLI */
