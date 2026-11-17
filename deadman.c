#define pr_fmt(fmt) "DEADMAN: " fmt

#include <linux/module.h>	/* Specifically, a module */
#include <linux/version.h>
#include <linux/kernel.h>	/* We're doing kernel work */
#include <linux/proc_fs.h>	/* Necessary because we use the proc fs */
#include <linux/sched/signal.h> /* for_each_process */
#include <linux/sched.h>	/* We need to put ourselves to sleep and wake up later */
#include <linux/uaccess.h>
#include <linux/workqueue.h>	/* We scheduale tasks here */				   
#include <linux/init.h>		/* For __init and __exit */
#include <linux/rcupdate.h>	/* rcu_read_lock */
#include <linux/reboot.h>
#include <linux/mutex.h>
#include <linux/seq_file.h>

/* 
 * some work_queue related functions
 * are just available to GPL licensed Modules 
 */
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Tal Shalif");
MODULE_DESCRIPTION("Reboot the machine when a registered process dies or is stopped");
/*
  Based on examples from The 'Linux Kernel Module Programming Guide'
  at http://www.tldp.org/LDP/lkmpg/2.6/html/index.html
*/


#define PROCFS_MAX_SIZE		512
#define PROCFS_NAME 		"deadman"
#define MAX_PRECIOUS_PIDS	32

static int loopsleep = 30;	/* in seconds */
static int noboot = 0;
static int nowayout = 1;
static bool man_is_dead;
static int reboot_delay = 180;  /* delay reboot by enough time to exit a desktop session */

module_param(nowayout,int, 0444);
MODULE_PARM_DESC(nowayout, "Deadman cannot be stopped once started (default=true)");
module_param(loopsleep,int, 0444);
MODULE_PARM_DESC(loopsleep, "Deadman check is triggered every x seconds (default=30)");
module_param(noboot,int, 0444);
MODULE_PARM_DESC(noboot, "Deadman will not reboot the machine (default=false)");

module_param(reboot_delay,int, 0444);
MODULE_PARM_DESC(reboot_delay, "Deadman will delay reboot by that many seconds - enough time for example to exit a desktop session (default=180)");


/*
 * This structure hold information about the /proc file
 *
 */
static struct proc_dir_entry *deadman_proc_file;

#define MY_WORK_QUEUE_NAME "WQdeadman.c"


static DEFINE_MUTEX(deadman_mutex);

static int myprecious[MAX_PRECIOUS_PIDS];
static int num_precious;

static void intrpt_routine(struct work_struct *);

static int die;			/* set this to 1 for shutdown */
static int reboot_remaining;	/* seconds left before delayed reboot */

/* 
 * The work queue structure for this task, from workqueue.h 
 */
static struct workqueue_struct *my_workqueue;

static DECLARE_DELAYED_WORK(Task, intrpt_routine);


static void deadman_reboot(void)
{
  pr_crit("Initiating system reboot.\n");
#ifdef HAS_MACHINE_RESTART
  machine_restart(NULL);
#else
  emergency_restart();
#endif
  pr_crit("Reboot did not happen\n");
}

/*
 * Task state moved from ->state to ->__state in Linux 5.14.
 */
static inline unsigned int deadman_task_state(struct task_struct *p)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 14, 0)
  return READ_ONCE(p->__state);
#else
  return READ_ONCE(p->state);
#endif
}

/*
 * Returns true if the process with the given (init namespace) pid is gone,
 * stopped, a zombie or dead.  The task list is walked and the task inspected
 * under rcu_read_lock() so the task_struct cannot be freed underneath us.
 */
static bool deadman_pid_is_dead(int pid)
{
  struct task_struct *p;
  bool dead = true;

  rcu_read_lock();
  for_each_process(p) {
    if (p->pid == pid) {
      /* zombie/dead are reported in exit_state, stopped in state */
      dead = (READ_ONCE(p->exit_state) & (EXIT_ZOMBIE | EXIT_DEAD)) ||
             (deadman_task_state(p) & (__TASK_STOPPED | TASK_DEAD));
      break;
    }
  }
  rcu_read_unlock();

  if (dead)
    pr_crit("my precious pid %d is gone, a zombie, stopped or dead!\n", pid);
  else
    pr_info("found my precious %d\n", pid);

  return dead;
}

/*
 * Periodic check, runs from the work queue every loopsleep seconds.
 */
static void intrpt_routine(struct work_struct *irrelevant)
{
  int pids[MAX_PRECIOUS_PIDS];
  int i, n;

  mutex_lock(&deadman_mutex);
  n = num_precious;
  for (i = 0; i < n; i++)
    pids[i] = myprecious[i];
  mutex_unlock(&deadman_mutex);

  for (i = 0; i < n; i++) {
    if (deadman_pid_is_dead(pids[i]))
      man_is_dead = true;
  }

  if (man_is_dead) {
    if (noboot) {
      pr_crit("Triggered - Reboot ignored.\n");
    } else if (reboot_remaining <= 0) {
      deadman_reboot();
    } else {
      pr_crit("delayed reboot - will reboot in about %d seconds.\n",
              reboot_remaining);
      reboot_remaining -= loopsleep;
    }
  }

  /*
   * If cleanup wants us to die, do not re-queue
   */
  if (!READ_ONCE(die))
    queue_delayed_work(my_workqueue, &Task, msecs_to_jiffies(loopsleep * MSEC_PER_SEC));
}


static int deadman_proc_show_info(struct seq_file *seq, void *v)
{
    int i;

    if (mutex_lock_interruptible(&deadman_mutex))
        return -ERESTARTSYS;
    seq_printf(seq, "nowayout=%d, noboot=%d, loopsleep=%d, reboot_delay=%d, myprecious=",
               nowayout, noboot, loopsleep, reboot_delay);
    if (num_precious == 0) {
        seq_puts(seq, "-1\n");
    } else {
        for (i = 0; i < num_precious; i++)
            seq_printf(seq, "%s%d", i ? "," : "", myprecious[i]);
        seq_putc(seq, '\n');
    }
    mutex_unlock(&deadman_mutex);
    return 0;
}

static int deadman_proc_open(struct inode *inode, struct file *file)
{
    return single_open(file, deadman_proc_show_info, NULL);
}

static bool deadman_has_pid_locked(int pid)
{
  int i;

  for (i = 0; i < num_precious; i++) {
    if (myprecious[i] == pid)
      return true;
  }
  return false;
}

/*
 * Register one or more pids (whitespace- or comma-separated) to watch.
 * Can be called multiple times to append additional pids up to
 * MAX_PRECIOUS_PIDS.  Returns 0 on success or a negative errno.
 */
static int read_pids(char *str, bool *first_registration)
{
  int parsed[MAX_PRECIOUS_PIDS];
  int nparsed = 0;
  int new_count = 0;
  char *cur = str;
  char *tok;
  int i, j, pid, ret;

  while ((tok = strsep(&cur, " \t\n\r,")) != NULL) {
    if (*tok == '\0')
      continue;

    ret = kstrtoint(tok, 10, &pid);
    if (ret) {
      pr_warn("input not an integer ('%s')\n", tok);
      return ret;
    }
    if (pid <= 0) {
      pr_warn("invalid pid %d\n", pid);
      return -EINVAL;
    }

    /* Deduplicate within the same write buffer */
    for (j = 0; j < nparsed; j++) {
      if (parsed[j] == pid)
        goto next_tok;
    }

    if (nparsed >= MAX_PRECIOUS_PIDS) {
      pr_warn("too many pids (max %d)\n", MAX_PRECIOUS_PIDS);
      return -ENOSPC;
    }
    parsed[nparsed++] = pid;
next_tok:
    ;
  }

  if (nparsed == 0) {
    pr_warn("no pid provided\n");
    return -EINVAL;
  }

  if (mutex_lock_interruptible(&deadman_mutex))
    return -ERESTARTSYS;

  for (i = 0; i < nparsed; i++) {
    if (!deadman_has_pid_locked(parsed[i]))
      new_count++;
  }

  if (num_precious + new_count > MAX_PRECIOUS_PIDS) {
    pr_warn("cannot register %d new pid(s); %d/%d slots already in use\n",
            new_count, num_precious, MAX_PRECIOUS_PIDS);
    mutex_unlock(&deadman_mutex);
    return -ENOSPC;
  }

  *first_registration = (num_precious == 0 && new_count > 0);
  if (*first_registration)
    reboot_remaining = reboot_delay;

  for (i = 0; i < nparsed; i++) {
    if (deadman_has_pid_locked(parsed[i])) {
      pr_info("pid %d is already registered\n", parsed[i]);
    } else {
      myprecious[num_precious++] = parsed[i];
      pr_info("registered precious pid %d (%d total)\n",
              parsed[i], num_precious);
    }
  }

  mutex_unlock(&deadman_mutex);
  return 0;
}

/*
 * This function is called with the /proc file is written
 *
 */
static ssize_t procfile_write(struct file *file, const char __user *buffer,
                              size_t count, loff_t *data)
{
  char read_buff[PROCFS_MAX_SIZE + 1];
  bool first_registration = false;
  int ret;

  if (count > PROCFS_MAX_SIZE) {
    pr_warn("write size %zu exceeds max\n", count);
    return -EINVAL;
  }

  if (copy_from_user(read_buff, buffer, count))
    return -EFAULT;

  read_buff[count] = '\0';

  ret = read_pids(read_buff, &first_registration);
  if (ret)
    return ret;

  if (first_registration)
    queue_delayed_work(my_workqueue, &Task, msecs_to_jiffies(loopsleep * MSEC_PER_SEC));

  return count;
}


static const struct proc_ops proc_ops = {
  .proc_open = deadman_proc_open,
  .proc_read  = seq_read,
  .proc_write = procfile_write,
  .proc_lseek = seq_lseek,
  .proc_release = single_release
};
  
/*
 *This function is called when the module is loaded
 *
 */
static int __init deadman_init(void)
{
  if (loopsleep <= 0) {
    pr_err("loopsleep must be positive (got %d)\n", loopsleep);
    return -EINVAL;
  }

  my_workqueue = create_singlethread_workqueue(MY_WORK_QUEUE_NAME);
  if (!my_workqueue)
    return -ENOMEM;

  /* create the /proc file */
  deadman_proc_file = proc_create(PROCFS_NAME, 0644, NULL, &proc_ops);

  if (deadman_proc_file == NULL) {
    pr_alert("Could not initialize /proc/%s\n", PROCFS_NAME);
    destroy_workqueue(my_workqueue);
    return -ENOMEM;
  }

  pr_info("/proc/%s created\n", PROCFS_NAME);
  return 0;	/* everything is ok */
}

/*
 *This function is called when the module is unloaded
 *
 */
static void __exit deadman_exit(void)
{
  bool registered;

  mutex_lock(&deadman_mutex);
  registered = num_precious > 0;
  mutex_unlock(&deadman_mutex);

  /* Only a registered deadman has anything to guard. */
  if (registered && nowayout && !noboot) {
    pr_warn("Deadman cannot be stopped! I take the machine down with me..\n");
    deadman_reboot();
  }

  remove_proc_entry(PROCFS_NAME, NULL);

  WRITE_ONCE(die, 1);		/* keep intrpt_routine from queueing itself */
  cancel_delayed_work_sync(&Task);	/* no "new ones", wait for a running one */
  flush_workqueue(my_workqueue);	/* wait till all "old ones" finished */
  destroy_workqueue(my_workqueue);

  pr_info("/proc/%s removed\n", PROCFS_NAME);
}

module_init(deadman_init);
module_exit(deadman_exit);
