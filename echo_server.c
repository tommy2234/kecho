#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/sched/signal.h>
#include <linux/tcp.h>
#include <linux/types.h>

#include "echo_server.h"

#define BUF_SIZE 4096

struct echo_service daemon = {.is_stopped = false};
/* A global workqueue */
extern struct workqueue_struct *kecho_wq;

static int get_request(struct socket *sock, unsigned char *buf, size_t size)
{
    struct msghdr msg;
    struct kvec vec;
    int length;

    /* kvec setting */
    vec.iov_len = size;
    vec.iov_base = buf;

    /* msghdr setting */
    msg.msg_name = 0;
    msg.msg_namelen = 0;
    msg.msg_control = NULL;
    msg.msg_controllen = 0;
    msg.msg_flags = 0;

    /*
     * TODO: during benchmarking, such printk() is useless and lead to worse
     * result. Add a specific build flag for these printk() would be good.
     */
    printk(MODULE_NAME ": start get response\n");
    /* get msg */
    length = kernel_recvmsg(sock, &msg, &vec, size, size, msg.msg_flags);
    printk(MODULE_NAME ": get request = %s\n", buf);

    return length;
}

static int send_request(struct socket *sock, unsigned char *buf, size_t size)
{
    int length;
    struct kvec vec;
    struct msghdr msg;

    msg.msg_name = NULL;
    msg.msg_namelen = 0;
    msg.msg_control = NULL;
    msg.msg_controllen = 0;
    msg.msg_flags = 0;

    vec.iov_base = buf;
    vec.iov_len = strlen(buf);

    printk(MODULE_NAME ": start send request.\n");

    length = kernel_sendmsg(sock, &msg, &vec, 1, size);

    printk(MODULE_NAME ": send request = %s\n", buf);

    return length;
}

/*
 * The function for each worker(work_struct), assigned to a worker
 * using INIT_WORK().
 * It continuously handles requests from the connected socket, till
 * the daemon is stopped.
 */
static void echo_server_worker(struct work_struct *work)
{
    struct kecho *worker = container_of(work, struct kecho, kecho_work);
    unsigned char *buf;

    buf = kzalloc(BUF_SIZE, GFP_KERNEL);
    if (!buf) {
        printk(KERN_ERR MODULE_NAME ": kmalloc error....\n");
        return;
    }

    while (!daemon.is_stopped) {
        int res = get_request(worker->sock, buf, BUF_SIZE - 1);
        if (res <= 0) {
            if (res) {
                printk(KERN_ERR MODULE_NAME ": get request error = %d\n", res);
            }
            break;
        }

        res = send_request(worker->sock, buf, res);
        if (res < 0) {
            printk(KERN_ERR MODULE_NAME ": send request error = %d\n", res);
            break;
        }

        memset(buf, 0, res);
    }

    kernel_sock_shutdown(worker->sock, SHUT_RDWR);
    kfree(buf);
}

/* create a worker(work_struct) to handle a connection using the given socket */
static struct work_struct *create_work(struct socket *sk)
{
    struct kecho *work;

    if (!(work = kmalloc(sizeof(struct kecho), GFP_KERNEL)))
        return NULL;

    work->sock = sk;

    INIT_WORK(&work->kecho_work, echo_server_worker);

    list_add(&work->list, &daemon.worker);

    return &work->kecho_work;
}

/* it would be better if we do this dynamically */
static void free_work(void)
{
    struct kecho *l, *tar;
    /* cppcheck-suppress uninitvar */

    list_for_each_entry_safe (tar, l, &daemon.worker, list) {
        kernel_sock_shutdown(tar->sock, SHUT_RDWR);
        flush_work(&tar->kecho_work);
        sock_release(tar->sock);
        kfree(tar);
    }
}

/* continuously create workers, each worker handles an accepted socket */
int echo_server_daemon(void *arg)
{
    struct echo_server_param *param = arg;
    struct socket *sock;
    struct work_struct *work;

    allow_signal(SIGKILL);
    allow_signal(SIGTERM);

    INIT_LIST_HEAD(&daemon.worker);

    while (!kthread_should_stop()) {
        /* using blocking I/O */
        int error = kernel_accept(param->listen_sock, &sock, 0);
        if (error < 0) {
            if (signal_pending(current))
                break;
            printk(KERN_ERR MODULE_NAME ": socket accept error = %d\n", error);
            continue;
        }

        if (unlikely(!(work = create_work(sock)))) {
            printk(KERN_ERR MODULE_NAME
                   ": create work error, connection closed\n");
            kernel_sock_shutdown(sock, SHUT_RDWR);
            sock_release(sock);
            continue;
        }

        /* start the server worker, queue the work on the workqueue */
        queue_work(kecho_wq, work);
    }

    printk(MODULE_NAME ": daemon shutdown in progress...\n");

    daemon.is_stopped = true;
    free_work();

    return 0;
}
