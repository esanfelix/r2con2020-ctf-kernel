#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/list.h>
#include <linux/sched/mm.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/uaccess.h>

#include "socks.h"

#include <linux/spinlock.h>

sock_dev_t sock_device;

#define MAX_SIZE 0x1000

static long socks_ioctl_init(sock_t *sock, unsigned long size) {
    sock_buf_t *buf = NULL;
    int err = 0;

    // Sanity check without locking the buffer, this time with arg before the addition
    if (size > MAX_SIZE) {
        return -EINVAL;
    }

    // First off: lock the buffer

    spin_lock(&sock->lock);

    if (sock->state != UNINITIALIZED) {
        err = -EINVAL;
        goto out_unlock;
    }

    buf = kzalloc(sizeof(sock_buf_t), GFP_KERNEL);

    if (IS_ERR_OR_NULL(buf)) {
        err = (buf ? PTR_ERR(buf) : -ENOMEM);
        goto out_unlock;
    }

    buf->buffer = kzalloc(size, GFP_KERNEL);


    if (IS_ERR_OR_NULL(buf->buffer)) {
        err = (buf->buffer ? PTR_ERR(buf->buffer) : -ENOMEM);
        kfree(buf);
        goto out_unlock;
    }

    // printk(KERN_ALERT "Allocated ptr %llx buffer %llx\n", buf, buf->buffer);


    sock->buf = buf;
    sock->buf->size = size;
    sock->buf->write_index = 0;
    sock->buf->read_index = 0;

    sock->state = INITIALIZED;

    pr_info("Initialized socket with buffer size %lx\n", sock->buf->size);    

out_unlock:
    spin_unlock(&sock->lock);
    return err;
}

static sock_t *socks_find_listening_device(char *name) {
    pr_info("Searching for socket %s\n", name);
    sock_t *s = NULL;

    list_for_each_entry ( s , &sock_device.listening, listening_list ) {
        if (!strcmp(s->name, name)) {
            return s;
        }
    } 

    return NULL;
}

static long socks_ioctl_listen(sock_t *sock, unsigned long arg) {
    struct sock_name_param * __user user_param = (struct sock_name_param * __user)arg;
    struct sock_name_param local_param, *param = &local_param;
    int err = 0;

    /* Fail if copy_from_user is bad */
    if (copy_from_user(param, user_param, sizeof(*param))) {
        return -EFAULT;
    }

    /* Make sure we null-terminate the string */
    param->name[sizeof(param->name) -1] = '\0';

    pr_info("Length of name: %d\n", strlen(param->name));


    spin_lock(&sock->lock);

    /*
     * Not allowed to listen unless we are in initialized
     * state.
     */
    if (sock->state != INITIALIZED) {
        err = -EINVAL;
        goto out_unlock;
    }

    spin_lock(&sock_device.lock);
    if (socks_find_listening_device(param->name)) {
        err = -EINVAL;
        pr_err("There's already a socket with that name");
        spin_unlock(&sock_device.lock);
        goto out_unlock;
    }

    // Alright, nobody else on that list. Add to the list and set to listening
    strcpy(sock->name, param->name);
    sock->state = LISTENING;
    list_add(&sock->listening_list, &sock_device.listening);
    pr_info("Socket is now listening at %s\n", param->name);
    spin_unlock(&sock_device.lock);

    // Now we need to wait until someone connects actually! Maybe we just poll for now.
    
out_unlock:
    spin_unlock(&sock->lock);
    return err;
}

static long socks_ioctl_connect(sock_t *sock, unsigned long arg) {
    struct sock_name_param * __user user_param = (struct sock_name_param * __user)arg;
    struct sock_name_param local_param, *param = &local_param;
    sock_t *peer = NULL;
    int err = 0;

    /* Fail if copy_from_user is bad */
    if (copy_from_user(param, user_param, sizeof(*param))) {
        return -EFAULT;
    }

    /* Make sure we null-terminate the string */
    param->name[63] = '\0';

    pr_info("Length of name: %d\n", strlen(param->name));

    spin_lock(&sock->lock);

    /*
     * Not allowed to connect unless we are in initialized
     * state.
     */
    if (sock->state != INITIALIZED) {
        err = -EINVAL;
        goto out_unlock;
    }

    spin_lock(&sock_device.lock);
    if ( (peer = socks_find_listening_device(param->name)) == NULL) {
        pr_err("No socket with that name found");
        err = -EINVAL;
        spin_unlock(&sock_device.lock);
        goto out_unlock;
    }

    // Lock the peer so no-one else can touch it
    spin_lock(&peer->lock);

    // Remove peer from listening list
    list_del_init(&peer->listening_list);

    // Unlock the list as we're done with it now
    spin_unlock(&sock_device.lock);

    // Connect them!
    sock->state = CONNECTED;
    sock->peer = peer;
    peer->peer = sock;
    peer->state = CONNECTED;

    pr_info("Successfully connected to %s\n", param->name);

    // Unlock the peer
    spin_unlock(&peer->lock);

    // Unlock ourselves and go
out_unlock:
    spin_unlock(&sock->lock);
    return err;
}

/*
 * Assume it's called with the owning sock locked so nobody
 * can touch it.
 */
static size_t sock_buf_count(sock_buf_t *buf) {
    /* write_index > read_index: then write_index-read_index */
    if (buf->write_index >= buf->read_index) {
        return buf->write_index - buf->read_index;
    }

    /* If write_index is below read_index, we have available
     * from [read_index, size) + [0, write_index).
     */

    return (buf->size - buf->read_index) + buf->write_index;
}

static size_t sock_buf_left(sock_buf_t *buf) {
    return buf->size - sock_buf_count(buf);
}

static long socks_push(sock_buf_t *buf, void * __user buffer, size_t size) {

    /*
     * If data doesn't fit, fail.
     */

    if (sock_buf_left(buf) < size) {
        return -ENOMEM;
    }


    // We can write up to read_index if it's bigger than write_index, or up to end of buffer otherwise
    size_t max_write_index = (buf->read_index > buf->write_index) ? buf->read_index : buf->size;
    size_t copy1_size = min(size, max_write_index - buf->write_index);
    size_t prev_write_index = buf->write_index;

    if (copy_from_user(buf->buffer + buf->write_index, buffer, copy1_size)) {
        // Failed to copy, ignore the data and return ENOMEM
        return -ENOMEM;
    }

    // Update our index
    buf->write_index = (buf->write_index + copy1_size) % buf->size;

    // More to copy, this time to beginning of buffer
    if (size > copy1_size) {
        size_t copy_left = size - copy1_size;
        if ( (sock_buf_left(buf) < copy_left) || 
            copy_from_user(buf->buffer + buf->write_index, buffer, copy_left)) {
            // Failed to copy, roll back and return ENOMEM
            buf->write_index = prev_write_index;
            return -ENOMEM;            
        }

        // Update write index
        buf->write_index = (buf->write_index + copy_left) % buf->size;
    }

    return size;
}

static long socks_pull(sock_buf_t *buf, void * __user buffer, size_t size) {
    size_t count = sock_buf_count(buf);
    if ( count == 0) {
        // Empty buffer => nothing to do
        return -EWOULDBLOCK;
    }

    // We are going to read as much as we can always
    size_t to_read = min(count, size);

    // It either fits in one go or we need to read from the beginning of the buffer
    size_t copy1_size = min(to_read, buf->size - buf->read_index);
    size_t prev_read_index = buf->read_index;

    if (copy_to_user(buffer, buf->buffer + buf->read_index, copy1_size)) {
        // Failed to copy, ignore the data and return ENOMEM
        return -ENOMEM;
    } 


    // Update read index
    buf->read_index = (buf->read_index + copy1_size) % buf->size;

    // Do we still have something to read?
    if (to_read > copy1_size) {
        // In this case read_index must have rolled over. WARN_ON just in case
        WARN_ON(buf->read_index != 0);

        // printk(KERN_ALERT "[*] Doing second copy from beginning of buffer %llx\n", buf->buffer);
        size_t left = to_read - copy1_size;
        if (copy_to_user(buffer + copy1_size, buf->buffer + buf->read_index, left)) {
            // Failed to copy, ignore the data and return ENOMEM
            buf->read_index = prev_read_index;
            return -ENOMEM;
        }

        buf->read_index = (buf->read_index + copy1_size) % buf->size;

    }

    return (long)to_read;
}

static long socks_ioctl_send(sock_t *sock, unsigned long arg) {
    struct sock_buffer_param * __user user_param = (struct sock_buffer_param * __user)arg;
    struct sock_buffer_param local_param, *param = &local_param;
    int err = 0;

    /* Fail if copy_from_user is bad */
    if (copy_from_user(param, user_param, sizeof(*param))) {
        return -EFAULT;
    }

    spin_lock(&sock->lock);

    if (sock->state != CONNECTED) {
        /* Must be connected to send! */
        err = -EINVAL;
        goto out_unlock_self;
    }

    spin_unlock(&sock->lock);

    // Connected: lock peer and push to buffer
    spin_lock(&sock->peer->lock);
    err = socks_push(sock->peer->buf, param->buffer, param->size);
    spin_unlock(&sock->peer->lock);

    return err;

out_unlock_self:
    spin_unlock(&sock->lock);
    return err;
}

static long socks_ioctl_recv(sock_t *sock, unsigned long arg) {
    struct sock_buffer_param * __user user_param = (struct sock_buffer_param * __user)arg;
    struct sock_buffer_param local_param, *param = &local_param;
    int err = 0;

    /* Fail if copy_from_user is bad */
    if (copy_from_user(param, user_param, sizeof(*param))) {
        return -EFAULT;
    }

    spin_lock(&sock->lock);

    if (sock->state != CONNECTED) {
        /* Must be connected to receive! */
        err = -EINVAL;
        goto out_unlock_self;
    }

    /* Try to read from the buffer now */
    err = socks_pull(sock->buf, param->buffer, param->size);

out_unlock_self:
    spin_unlock(&sock->lock);
    return err;
}

static long socks_ioctl_resize(sock_t *sock, unsigned long arg) {
    void *new_buffer = NULL;
    uint64_t new_size = arg;
    uint64_t copy_size = 0;
    uint64_t old_size = 0;
    void *old_buffer = NULL;
    mm_segment_t old_fs;

    if (sock->state < INITIALIZED) {
        /* Bad state */
        return -1;
    }

    /* Can't grow over the max! */
    if (new_size > MAX_SIZE) {
        return -1;
    }

    if (sock->buf == NULL) {
        /* Shouldn't happen if we're initialized, but just in case */
        return -1;
    }

    /* Nothing to do if same size */
    if (sock->buf->size == new_size)
        return 0;

    /* If it's becoming smaller, ensure it fits! */
    if (new_size < sock->buf->size) {

        if (sock_buf_count(sock->buf) >= new_size) {
            // printk(KERN_ALERT "shrinking but it won't fit!\n");

            return -1;
        }
    }


    int err = 0;
    /* Make a new buffer */

    new_buffer = kzalloc(new_size, GFP_KERNEL);

    if (IS_ERR_OR_NULL(new_buffer)) {
        err = (new_buffer ? PTR_ERR(new_buffer) : -ENOMEM);
        return -1;
    }

    /* Get the old buffer */
    old_buffer = sock->buf->buffer;
    old_size = sock->buf->size;

    /*
     * Pull data to front of new buffer. Since we are going to use
     * socks_pull, we need to use KERNEL_DS.
     */
    old_fs = get_fs();
    set_fs(KERNEL_DS);
    copy_size = sock_buf_count(sock->buf);
    socks_pull(sock->buf, new_buffer, copy_size);
    
    /* Replace old buffer by new buffer */
    sock->buf->write_index = copy_size;
    sock->buf->read_index = 0;
    sock->buf->size = new_size;
    sock->buf->buffer = new_buffer;
    set_fs(old_fs);


    
    /*
     * And now free the old buffer.
     * Let's fill it with 0xcc first for debugging purposes.
     */
    memset(old_buffer, 0xcc, old_size);
    kfree(old_buffer);

    return 0;
}

static long socks_ioctl (struct file *file, unsigned int code, unsigned long arg) {
    sock_t *sock = NULL;
    
    sock = (sock_t *)file->private_data;

    switch(code) {
        case IOCTL_SOCKS_INIT:
            return socks_ioctl_init(sock, arg);

        case IOCTL_SOCKS_LISTEN:
            return socks_ioctl_listen(sock, arg);

        case IOCTL_SOCKS_CONNECT:
            return socks_ioctl_connect(sock, arg);

        case IOCTL_SOCKS_SEND:
            return socks_ioctl_send(sock, arg);

        case IOCTL_SOCKS_RECV:
            return socks_ioctl_recv(sock, arg);

        case IOCTL_SOCKS_RESIZE:
            return socks_ioctl_resize(sock, arg);

        default:
            return -EINVAL;
    }

    return 0;
}
static int socks_open(struct inode *inode, struct file *file)
{

    sock_t *sock = kzalloc(sizeof(*sock), GFP_KERNEL);
    
    if (!sock)
        return -ENOMEM;

    /* Place our sock into private_data so we can find it later */
    file->private_data = sock;

    /* And make sure the listening_list list_head makes sense */
    INIT_LIST_HEAD(&sock->listening_list);

    pr_info("New socks successfully created!\n", sock);

    return 0;
}

static int socks_close(struct inode *inodep, struct file *filp)
{
    sock_t *sock =  (sock_t *)filp->private_data;

    spin_lock(&sock->lock);

    if (sock->state == CONNECTED ) {
        /*
         * If we were connected, let's make sure we disconnect 
         * from the other end now.
         */

        sock_t *peer = sock->peer;
        sock->peer = NULL;

        spin_lock(&peer->lock);
        peer->peer = NULL;
        /* Back to initialized for this peer */
        peer->state = INITIALIZED;
        
        /* Ignore any stale data */
        peer->buf->write_index = 0;
        peer->buf->read_index = 0;

        spin_unlock(&peer->lock);

    } else if (sock->state == LISTENING) {
        /*
         * If we were listening, remove from the listening list.
         */ 
        spin_lock(&sock_device.lock);
        list_del_init(&sock->listening_list);
        spin_unlock(&sock_device.lock);
    }

    if (sock->state != UNINITIALIZED) {

        /*
         * Since we were initialized, we must have a socket.
         * Free it.
         */
        kfree(sock->buf->buffer);
        kfree(sock->buf);
    }

    spin_unlock(&sock->lock);

    /* Finally done, free our sock */
    kfree(sock);

    return 0;
}

static const struct file_operations socks_fops = {
    .owner			= THIS_MODULE,
    .open			= socks_open,
    .release		= socks_close,
    .llseek 		= no_llseek,
    .unlocked_ioctl = socks_ioctl,
};

struct miscdevice socks_device = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "r2socks2",
    .fops = &socks_fops,
};

static int __init misc_init(void)
{
    int error;

    INIT_LIST_HEAD(&sock_device.listening);
    spin_lock_init(&sock_device.lock);

    error = misc_register(&socks_device);
    if (error) {
        pr_err("can't misc_register :(\n");
        return error;
    }

    pr_info("I'm in\n");
    return 0;
}

static void __exit misc_exit(void)
{
    misc_deregister(&socks_device);
}

module_init(misc_init)
module_exit(misc_exit)

MODULE_DESCRIPTION("Module providing IPC through socks!");
MODULE_AUTHOR("r2");
MODULE_LICENSE("GPL");
