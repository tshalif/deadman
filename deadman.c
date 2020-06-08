#include <linux/module.h>	/* Specifically, a module */
#include <linux/kernel.h>	/* We're doing kernel work */
#include <linux/proc_fs.h>	/* Necessary because we use the proc fs */
#include <linux/sched/signal.h> /* for_each_process */
#include <linux/sched.h>	/* We need to put ourselves to sleep and wake up later */
#include <linux/uaccess.h>
// #include <asm/uaccess.h>	/* for copy_from_user */
#include <linux/workqueue.h>	/* We scheduale tasks here */				   
#include <linux/init.h>		/* For __init and __exit */
#include <linux/interrupt.h>	/* For irqreturn_t */
#include <linux/reboot.h>
#include <linux/semaphore.h>


/* 
 * some work_queue related functions
 * are just available to GPL licensed Modules 
 */
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Tal Shalif");
/*
  Based on examples from The 'Linux Kernel Module Programming Guide'
  at http://www.tldp.org/LDP/lkmpg/2.6/html/index.html
*/


#define PROCFS_MAX_SIZE		512
#define PROCFS_NAME 		"deadman"

static int loopsleep = 30;	/* in seconds */
static int noboot = 0;
static int nowayout = 1;
static bool man_is_dead = false;
static int reboot_delay = 180;  /* delay reboot by enough time to exit a desktop session */

module_param(nowayout,int, 0444);
MODULE_PARM_DESC(nowayout, "Deadman cannot be stopped once started (default=true)");
module_param(loopsleep,int, 0444);
MODULE_PARM_DESC(loopsleep, "Deadman check is triggered every x seconds (default=60)");
module_param(noboot,int, 0444);
MODULE_PARM_DESC(noboot, "Deadman will not reboot the machine (default=false)");

module_param(reboot_delay,int, 0444);
MODULE_PARM_DESC(reboot_delay, "Deadman will delay reboot by that many seconds - enough time for example to exit a desktop session (default=180)");


/**
 * This structure hold information about the /proc file
 *
 */
static struct proc_dir_entry *deadman_proc_file;

#define MY_WORK_QUEUE_NAME "WQdeadman.c"


static DEFINE_SEMAPHORE(deadman_mutex);

static int myprecious = -1;

static void intrpt_routine(struct work_struct *);

static int die = 0;		/* set this to 1 for shutdown */

/* 
 * The work queue structure for this task, from workqueue.h 
 */
static struct workqueue_struct *my_workqueue;

//static struct work_struct Task;
static DECLARE_DELAYED_WORK(Task, intrpt_routine);


static struct task_struct * deadman_find_pid(int pid);


static void deadman_reboot(void)
{
  printk(KERN_CRIT "DEADMAN: Initiating system reboot.\n");
#ifdef HAS_MACHINE_RESTART
  machine_restart(NULL);
#else
  emergency_restart();
#endif
  printk("DEADMAN: Reboot didn't ?????\n");
}


/* 
 * This function will be called on every timer interrupt. Notice the void*
 * pointer - task functions can be used for more than one purpose, each time
 * getting a different parameter.
 */
static void intrpt_routine(struct work_struct *irrelevant)
{

  int pid = myprecious;
  struct task_struct *res = 0;

  res = deadman_find_pid(pid);

  if (!res) {
    printk(KERN_INFO "my precious pid %d not found\n", pid);
    man_is_dead = true;
  } else {
    switch (res->state) {
    case -1:
    case TASK_STOPPED:
    case EXIT_ZOMBIE:
    case EXIT_DEAD:
    case TASK_DEAD:
      printk(KERN_CRIT "DEADMAN: my precious pid %d is a zomby, stopped or dead!\n", res->pid);

      man_is_dead = true;

      break;
    default:
      printk(KERN_INFO "DEADMAN: found my precious %d\n", pid);
      break;
    }

  }


  if (man_is_dead) {
    if (noboot) {
      printk(KERN_CRIT "DEADMAN: Triggered - Reboot ignored.\n");
    }
    else {
      if (reboot_delay <= 0) {
        deadman_reboot();
      } else {

        {
          int reboot_time = reboot_delay <= 0 ? loopsleep : reboot_delay;
          printk(KERN_CRIT "DEADMAN: delayed reboot - will reboot in about %d seconds.\n", reboot_time);
        }

        reboot_delay -= loopsleep;

      }
    }
  }

  /* 
   * If cleanup wants us to die
   */
  if (die == 0) {
    queue_delayed_work(my_workqueue, &Task, msecs_to_jiffies(loopsleep * 1000));
  }
}


/** 
 * This function is called then the /proc file is read
 *
 */
static ssize_t 
procfile_read(struct file *file, char *buffer,
	      size_t buffer_length, loff_t *offset)
{
  int ret;
	
  if (*offset) {
    /* we have finished to read, return 0 */
    ret  = 0;
  } else {
    ret = snprintf(buffer, buffer_length, "nowayout=%d, noboot=%d, loopsleep=%d, reboot_delay=%d, myprecious=%d\n", nowayout, noboot, loopsleep, reboot_delay, myprecious);
    *offset += ret;
  }
  return ret;
}

static int read_pid(const char *str) 
{
  char *simple_strtol_term = 0;
  int retval = 0;

  down(&deadman_mutex);

  if (-1 == myprecious) {
    myprecious = simple_strtol(str, &simple_strtol_term, 10);

    if (simple_strtol_term == str || (*simple_strtol_term != '\0' && *simple_strtol_term != '\n')) { /* integer parse error */
      printk("DEADMAN: input not an integer ('%s')\n", str);
      
      myprecious = -1;
    }

    retval = -1 != myprecious;
  }

  up(&deadman_mutex);

  return retval;
}
/**
 * This function is called with the /proc file is written
 *
 */
static ssize_t procfile_write(struct file *file, const char *buffer, size_t count,
		   loff_t *data)
{
  char read_buff[PROCFS_MAX_SIZE + 1];

  if (count > PROCFS_MAX_SIZE ) {
    printk(KERN_WARNING "deadman write size %ld exceeds max\n", count);
    return -EFAULT;
  }
  
  /* write data to the buffer */
  if ( copy_from_user(read_buff, buffer, count) ) {
    return -EFAULT;
  }
		
  read_buff[count] = '\0';

  if (read_pid(read_buff)) {
    queue_delayed_work(my_workqueue, &Task, msecs_to_jiffies(loopsleep * 1000));
  }


  return count;
}


static struct task_struct * deadman_find_pid(int pid)
{
  struct task_struct *p, *res = 0;

  // extern rwlock_t tasklist_lock __attribute__((weak));


  // printk(KERN_INFO "deadman_find_pid: rasklist_lock >>\n");

  // read_lock(&tasklist_lock);

  for_each_process(p)
  {
    if (p->pid == pid) {
      res = p;
      break;
    }
  }    

  // read_unlock(&tasklist_lock);
	
  // printk(KERN_INFO "deadman_find_pid: rasklist_lock <<\n");

  return res;
}


static const struct file_operations proc_file_fops = {
  .owner = THIS_MODULE,
  .read  = procfile_read,
  .write = procfile_write,
};
  
/**
 *This function is called when the module is loaded
 *
 */
int __init init_module()
{

  /* create the /proc file */
  deadman_proc_file = proc_create(PROCFS_NAME, 0644, NULL, &proc_file_fops);
	
  if (deadman_proc_file == NULL) {
    printk(KERN_ALERT "Error: Could not initialize /proc/%s\n",
           PROCFS_NAME);
    return -ENOMEM;
  }


  /* 
   * Put the task in the work_timer task queue, so it will be executed at
   * next timer interrupt
   */
  my_workqueue = create_workqueue(MY_WORK_QUEUE_NAME);
	
  printk(KERN_INFO "/proc/%s created\n", PROCFS_NAME);	
  return 0;	/* everything is ok */
}

/**
 *This function is called when the module is unloaded
 *
 */
void __exit cleanup_module()
{

  if (nowayout && !noboot) {
    printk(KERN_WARNING "Deadman cannot be stopped!\nI take the machine down with me..");
    deadman_reboot();
  }
    
  remove_proc_entry(PROCFS_NAME, NULL);


  die = 1;		/* keep intrp_routine from queueing itself */
  cancel_delayed_work(&Task);	/* no "new ones" */
  flush_workqueue(my_workqueue);	/* wait till all "old ones" finished */
  destroy_workqueue(my_workqueue);

  /* 
   * Sleep until intrpt_routine is called one last time. This is 
   * necessary, because otherwise we'll deallocate the memory holding 
   * intrpt_routine and Task while work_timer still references them.
   * Notice that here we don't allow signals to interrupt us.
   *
   * Since WaitQ is now not NULL, this automatically tells the interrupt
   * routine it's time to die.
   */

  printk(KERN_INFO "/proc/%s removed\n", PROCFS_NAME);
}

