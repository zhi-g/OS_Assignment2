/*
 * Dummy scheduling class, mapped to range of 5 levels of SCHED_NORMAL policy
 */

#include "sched.h"

/*
 * Timeslice and age threshold are represented in jiffies. Default timeslice
 * is 100ms. Both parameters can be tuned from /proc/sys/kernel.
 */

#define DUMMY_TIMESLICE		(100 * HZ / 1000)
#define DUMMY_AGE_THRESHOLD	(3 * DUMMY_TIMESLICE)




unsigned int sysctl_sched_dummy_timeslice = DUMMY_TIMESLICE;


static inline unsigned int get_timeslice()
{
	return sysctl_sched_dummy_timeslice;
}

unsigned int sysctl_sched_dummy_age_threshold = DUMMY_AGE_THRESHOLD;
static inline unsigned int get_age_threshold()
{
	return sysctl_sched_dummy_age_threshold;
}

/*
 * Init
 */

void init_dummy_rq(struct dummy_rq *dummy_rq, struct rq *rq)
{
	struct dummy_prio_array *array;
	int i;

	array = &dummy_rq->array;
	for (i = 0; i < NBR_DUMMY_PRIO; i++) {
		INIT_LIST_HEAD(array->queues + i);
	}
}

/*
 * Helper functions
 */

static inline int get_list_prio(struct task_struct *p){
	return NBR_DUMMY_PRIO - (DUMMY_PRIO_UPPER_BOUND - (int)(p->prio))-1;
}

static inline struct task_struct *dummy_task_of(struct sched_dummy_entity *dummy_se)
{
	return container_of(dummy_se, struct task_struct, dummy_se);
}

static inline void _enqueue_task_dummy(struct rq *rq, struct task_struct *p)
{
	struct sched_dummy_entity *dummy_se = &p->dummy_se;
	struct dummy_prio_array *array = &rq->dummy.array;
	struct list_head *queue = array->queues + get_list_prio(p);
	list_add_tail(&dummy_se->run_list, queue);
}

static inline void _dequeue_task_dummy(struct task_struct *p, struct rq *rq)
{	
	struct sched_dummy_entity *dummy_se = &p->dummy_se;
	list_del_init(&dummy_se->run_list);
}

/*
 * Scheduling class functions to implement
 */

static void enqueue_task_dummy(struct rq *rq, struct task_struct *p, int flags)
{
	printk(KERN_CRIT "enqueue: %d, priority: %d\n",p->pid,p->prio);
	_enqueue_task_dummy(rq, p);	
	if (p->dummy_se.time_slice >= get_timeslice()) {
		p->dummy_se.time_slice = 0;
	}
	inc_nr_running(rq);
}

static void dequeue_task_dummy(struct rq *rq, struct task_struct *p, int flags)
{
	printk(KERN_CRIT "dequeue: %d, priority: %d\n",p->pid,p->prio);
	_dequeue_task_dummy(p, rq);	
	dec_nr_running(rq);
}

static void yield_task_dummy(struct rq *rq)
{
	struct task_struct *p = rq->curr;
	dequeue_task_dummy(rq, p, 0);
	if (p->dummy_se.aging >= get_age_threshold()) {
		p->dummy_se.aging = 0;
		p->prio = p->static_prio;
	}
	enqueue_task_dummy(rq, p, 0);
}

static void check_preempt_curr_dummy(struct rq *rq, struct task_struct *p, int flags)
{
	struct task_struct *curr = rq->curr;
	if(get_list_prio(p) < get_list_prio(curr)) {
		resched_task(curr);
	}
}

static struct task_struct *pick_next_task_dummy(struct rq *rq)
{
	struct dummy_rq *dummy_rq = &rq->dummy;
	struct sched_dummy_entity *next;
	int i;
	for(i=0; i<NBR_DUMMY_PRIO; ++i){
		if (!list_empty(dummy_rq->array.queues + i)) {
			next = list_first_entry(dummy_rq->array.queues + i, struct sched_dummy_entity, run_list);
			return dummy_task_of(next);
		} else {}
	}
	return NULL;
}

static void put_prev_task_dummy(struct rq *rq, struct task_struct *prev)
{
	
}

static void set_curr_task_dummy(struct rq *rq)
{
	//struct task_struct *p = rq->curr;
	//p->se.exec_start = rq_clock_task(rq);
}

static void task_tick_dummy(struct rq *rq, struct task_struct *curr, int queued)
{
	/*
	 * Requeue to the end of queue if we (and all of our ancestors) are the
	 * only element on the queue
	 */
	int i;

	curr->dummy_se.time_slice++;

	if (curr->dummy_se.time_slice >= get_timeslice()) {
		curr->prio = curr->static_prio;
		curr->dummy_se.aging = 0;
		dequeue_task_dummy(rq, curr, queued);
		enqueue_task_dummy(rq, curr, queued);
		resched_task(curr);
	}
	
	for (i = 1; i < NBR_DUMMY_PRIO; i++) {
		/*struct list_head *temp;
		INIT_LIST_HEAD(temp);*/
		struct sched_dummy_entity *dummy;
		struct sched_dummy_entity *dummy_temp;
		list_for_each_entry_safe(dummy, dummy_temp, rq->dummy.array.queues + i, run_list) {
			dummy->aging++;
			if(dummy->aging >= get_age_threshold() && dummy_task_of(dummy)->prio > DUMMY_PRIO_UPPER_BOUND - 5 + 1) {
				printk(KERN_CRIT "process %d aged\n",dummy_task_of(dummy)->pid);
				dummy->aging = 0;
				dummy_task_of(dummy)->prio = dummy_task_of(dummy)->prio-1;
				dequeue_task_dummy(rq, dummy_task_of(dummy), queued);
 				enqueue_task_dummy(rq, dummy_task_of(dummy), queued);
 				resched_task(dummy_task_of(dummy));
				check_preempt_curr_dummy(rq, dummy_task_of(dummy), queued);
			}
		}
	}
}

static void switched_from_dummy(struct rq *rq, struct task_struct *p)
{
	
}

static void switched_to_dummy(struct rq *rq, struct task_struct *p)
{
	
}

static void prio_changed_dummy(struct rq *rq, struct task_struct *p, int oldprio)
{
	if (!p->on_rq) {
		return;
	}

	if (rq->curr == p) {
		if (p->prio > oldprio)
			resched_task(rq->curr);
	} else {
		check_preempt_curr(rq, p, 0);
	}
}

static unsigned int get_rr_interval_dummy(struct rq *rq, struct task_struct *p)
{
	return get_timeslice();
}



/*
 * Scheduling class
 */

const struct sched_class dummy_sched_class = {
	.next			= &idle_sched_class,
	.enqueue_task		= enqueue_task_dummy,
	.dequeue_task		= dequeue_task_dummy,
	.yield_task		= yield_task_dummy,

	.check_preempt_curr 	= check_preempt_curr_dummy,

	.pick_next_task		= pick_next_task_dummy,
	.put_prev_task		= put_prev_task_dummy,

	.set_curr_task		= set_curr_task_dummy,
	.task_tick		= task_tick_dummy,

	.switched_from		= switched_from_dummy,
	.switched_to		= switched_to_dummy,
	.prio_changed		= prio_changed_dummy,

	.get_rr_interval	= get_rr_interval_dummy,
};

