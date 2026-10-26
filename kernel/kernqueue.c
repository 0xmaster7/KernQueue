// SPDX-License-Identifier: GPL-2.0-only
#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/poll.h>
#include <linux/workqueue.h>
#include "kernqueue.h"

struct kq_job {
    struct list_head node;
    struct kq_request request;
    struct kq_result result;
};
struct kq_context {
    struct mutex lock;
    struct list_head pending, done;
    wait_queue_head_t readable, writable;
    struct work_struct work;
    unsigned int outstanding, completed;
};
static struct workqueue_struct *kq_workers;

static void kq_process(struct work_struct *work)
{
    struct kq_context *ctx = container_of(work, struct kq_context, work);
    struct kq_job *job;
    unsigned int i;
    u64 hash;
    for (;;) {
        mutex_lock(&ctx->lock);
        if (list_empty(&ctx->pending)) {
            mutex_unlock(&ctx->lock);
            return;
        }
        job = list_first_entry(&ctx->pending, struct kq_job, node);
        list_del(&job->node);
        mutex_unlock(&ctx->lock);
        hash = 14695981039346656037ULL;
        for (i = 0; i < job->request.length; i++)
            hash = (hash ^ job->request.data[i]) * 1099511628211ULL;
        job->result.id = job->request.id;
        job->result.length = job->request.length;
        job->result.checksum = hash;
        mutex_lock(&ctx->lock);
        list_add_tail(&job->node, &ctx->done);
        ctx->completed++;
        mutex_unlock(&ctx->lock);
        wake_up_interruptible(&ctx->readable);
        cond_resched();
    }
}
static int kq_open(struct inode *inode, struct file *file)
{
    struct kq_context *ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
    if (!ctx)
        return -ENOMEM;
    mutex_init(&ctx->lock);
    INIT_LIST_HEAD(&ctx->pending);
    INIT_LIST_HEAD(&ctx->done);
    init_waitqueue_head(&ctx->readable);
    init_waitqueue_head(&ctx->writable);
    INIT_WORK(&ctx->work, kq_process);
    file->private_data = ctx;
    return nonseekable_open(inode, file);
}
static ssize_t kq_write(struct file *file, const char __user *buf,
                        size_t count, loff_t *pos)
{
    struct kq_context *ctx = file->private_data;
    struct kq_job *job;
    int ret;
    if (count != sizeof(struct kq_request))
        return -EINVAL;
    job = kzalloc(sizeof(*job), GFP_KERNEL);
    if (!job)
        return -ENOMEM;
    if (copy_from_user(&job->request, buf, count)) {
        ret = -EFAULT;
        goto fail;
    }
    if (!job->request.length || job->request.length > KQ_PAYLOAD ||
        job->request.reserved) {
        ret = -EINVAL;
        goto fail;
    }
    for (;;) {
        ret = mutex_lock_interruptible(&ctx->lock);
        if (ret)
            goto fail;
        if (ctx->outstanding < KQ_CAPACITY)
            break;
        mutex_unlock(&ctx->lock);
        if (file->f_flags & O_NONBLOCK) {
            ret = -EAGAIN;
            goto fail;
        }
        ret = wait_event_interruptible(ctx->writable,
                    READ_ONCE(ctx->outstanding) < KQ_CAPACITY);
        if (ret)
            goto fail;
    }
    list_add_tail(&job->node, &ctx->pending);
    ctx->outstanding++;
    /* Queue while locked so release/worker transitions have one ordering. */
    queue_work(kq_workers, &ctx->work);
    mutex_unlock(&ctx->lock);
    return count;
fail:
    kfree(job);
    return ret;
}
static ssize_t kq_read(struct file *file, char __user *buf,
                       size_t count, loff_t *pos)
{
    struct kq_context *ctx = file->private_data;
    struct kq_job *job;
    int ret;
    if (count < sizeof(struct kq_result))
        return -EINVAL;
    for (;;) {
        ret = mutex_lock_interruptible(&ctx->lock);
        if (ret)
            return ret;
        if (!list_empty(&ctx->done))
            break;
        mutex_unlock(&ctx->lock);
        if (file->f_flags & O_NONBLOCK)
            return -EAGAIN;
        ret = wait_event_interruptible(ctx->readable,
                                      READ_ONCE(ctx->completed) != 0);
        if (ret)
            return ret;
    }
    job = list_first_entry(&ctx->done, struct kq_job, node);
    /* Preserve the completion on a bad userspace pointer. Mutex may sleep. */
    if (copy_to_user(buf, &job->result, sizeof(job->result))) {
        mutex_unlock(&ctx->lock);
        return -EFAULT;
    }
    list_del(&job->node);
    ctx->completed--;
    ctx->outstanding--;
    mutex_unlock(&ctx->lock);
    kfree(job);
    wake_up_interruptible(&ctx->writable);
    return sizeof(struct kq_result);
}
static __poll_t kq_poll(struct file *file, poll_table *wait)
{
    struct kq_context *ctx = file->private_data;
    __poll_t mask = 0;
    poll_wait(file, &ctx->readable, wait);
    poll_wait(file, &ctx->writable, wait);
    mutex_lock(&ctx->lock);
    if (ctx->completed)
        mask |= EPOLLIN | EPOLLRDNORM;
    if (ctx->outstanding < KQ_CAPACITY)
        mask |= EPOLLOUT | EPOLLWRNORM;
    mutex_unlock(&ctx->lock);
    return mask;
}
static int kq_release(struct inode *inode, struct file *file)
{
    struct kq_context *ctx = file->private_data;
    struct kq_job *job, *next;
    cancel_work_sync(&ctx->work);
    list_for_each_entry_safe(job, next, &ctx->pending, node) {
        list_del(&job->node);
        kfree(job);
    }
    list_for_each_entry_safe(job, next, &ctx->done, node) {
        list_del(&job->node);
        kfree(job);
    }
    kfree(ctx);
    return 0;
}
static const struct file_operations kq_fops = {
    .owner = THIS_MODULE, .open = kq_open, .release = kq_release,
    .read = kq_read, .write = kq_write, .poll = kq_poll,
};
static struct miscdevice kq_device = {
    .minor = MISC_DYNAMIC_MINOR, .name = "kernqueue",
    .fops = &kq_fops, .mode = 0600,
};
static int __init kq_init(void)
{
    int ret;
    kq_workers = alloc_workqueue("kernqueue", WQ_UNBOUND, 0);
    if (!kq_workers)
        return -ENOMEM;
    ret = misc_register(&kq_device);
    if (ret)
        destroy_workqueue(kq_workers);
    return ret;
}
static void __exit kq_exit(void)
{
    misc_deregister(&kq_device);
    destroy_workqueue(kq_workers);
}
module_init(kq_init);
module_exit(kq_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Keshav Nanda");
MODULE_DESCRIPTION("Bounded asynchronous checksum job queues with per-open isolation");
