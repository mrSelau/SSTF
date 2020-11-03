/*
 * SSTF IO Scheduler
 *
 * For Kernel 4.13.9
 */

#include <linux/blkdev.h>
#include <linux/elevator.h>
#include <linux/bio.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/vmalloc.h>
#include <linux/printk.h>

/* SSTF data structure. */
struct sstf_data {
	struct list_head bigger;
	struct list_head smaller;

	sector_t last_sector;
};

static void sstf_merged_requests(struct request_queue *q, struct request *rq,
				 struct request *next)
{
	list_del_init(&next->queuelist);
}

/* Esta função despacha o próximo bloco a ser lido. */
static int sstf_dispatch(struct request_queue *q, int force){
	struct sstf_data *nd = q->elevator->elevator_data;
	char direction = 'R';
	struct request *rq, *rq1, *rq2;
	sector_t delta1, delta2;
	/* Aqui deve-se retirar uma requisição da fila e enviá-la para processamento.
	 * Use como exemplo o driver noop-iosched.c. Veja como a requisição é tratada.
	 *
	 * Antes de retornar da função, imprima o sector que foi atendido.
	 */
	if (list_empty(&nd->bigger) && list_empty(&nd->smaller)) {
		return 0;
	}
	// only the bigger queue is empty.
	else if (!list_empty(&nd->bigger) && list_empty(&nd->smaller)) {
		rq = list_entry_rq(nd->bigger.next);
		list_del_init(&rq->queuelist);
		nd->last_sector = rq_end_sector(rq);

		elv_dispatch_sort(q, rq);
		printk(KERN_EMERG "[SSTF] dsp %c %lu\n", direction, blk_rq_pos(rq));
		return 1;
	}
	else if (list_empty(&nd->bigger) && !list_empty(&nd->smaller)) {
		rq = list_entry_rq(nd->smaller.next);
		list_del_init(&rq->queuelist);
		nd->last_sector = rq_end_sector(rq);

		elv_dispatch_sort(q, rq);
		printk(KERN_EMERG "[SSTF] dsp %c %lu\n", direction, blk_rq_pos(rq));
		return 1;
	}
	// none of them are empty.
	else {
		// fetch the head of each queue.
		rq1 = list_entry_rq(nd->smaller.next);
		rq2 = list_entry_rq(nd->bigger.next);

		// find the nearest one
		delta1 = abs(nd->last_sector - rq_end_sector(rq1));
		delta2 = abs(nd->last_sector - rq_end_sector(rq2));

		if ( delta1 < delta2 ) {
			list_del_init(&rq1->queuelist);
			nd->last_sector = rq_end_sector(rq1);
			elv_dispatch_sort(q, rq1);
		}
		else {
			list_del_init(&rq2->queuelist);
			nd->last_sector = rq_end_sector(rq2);
			elv_dispatch_sort(q, rq2);
		}
		printk(KERN_EMERG "[SSTF] dsp %c %lu\n", direction, blk_rq_pos(rq));
		return 1;
	}

	return 0;
}

static void sstf_add_request(struct request_queue *q, struct request *rq){
	
	struct sstf_data *nd = q->elevator->elevator_data;
	char direction = 'R';
	struct request *big, *small, *req;
	sector_t pos, pos1, pos2;

	/* Aqui deve-se adicionar uma requisição na fila do driver.
	 * Use como exemplo o driver noop-iosched.c
	 *
	 * Antes de retornar da função, imprima o sector que foi adicionado na lista.
	 */
	// first case : both the queue are empty
	if (list_empty(&nd->bigger) && list_empty(&nd->smaller)) {
		// if both are empty, insert to the bigger queue.
		list_add_tail(&rq->queuelist, &nd->bigger);
	}
	else if (!list_empty(&nd->bigger) && list_empty(&nd->smaller)) {

		// get the first element of 'bigger' queue.
		big = list_entry_rq(nd->bigger.next);
		pos = blk_rq_pos(rq);
		pos1 = blk_rq_pos(big);

		// if 'rq' is bigger than the first element of 'bigger' queue
		if ( pos > pos1) {
			// find a right position to insert
			list_for_each_entry(req, &nd->bigger, queuelist) {
				if (blk_rq_pos(req) > pos)
					break;
			}
			list_add_tail(&rq->queuelist, &req->queuelist);
		}
		else {
			// else insert to the 'smaller' queue
			list_add_tail(&rq->queuelist, &nd->smaller);
		}
	}
	// third case : the bigger queue is empty
	else if (list_empty(&nd->bigger) && !list_empty(&nd->smaller)) {

		small = list_entry_rq(nd->smaller.next);

		pos = blk_rq_pos(rq);
		pos2 = blk_rq_pos(small);

		if (pos < pos2) {
			list_for_each_entry(req, &nd->smaller, queuelist) {
				if (blk_rq_pos(req) < pos)
					break;
			}
			list_add_tail(&rq->queuelist, &req->queuelist);
		}
		else {
			list_add_tail(&rq->queuelist, &nd->bigger);
		}
	}
	// fourth case : none of them are empty.
	else {
		big = list_entry_rq(nd->bigger.next);
		small = list_entry_rq(nd->smaller.next);

		pos1 = blk_rq_pos(big);
		pos2 = blk_rq_pos(small);
		pos = blk_rq_pos(rq);

		// sector num is bigger than the 'bigger' queue
		if ( pos > pos1 ) {

			// find a right position to insert
			list_for_each_entry(req, &nd->smaller, queuelist) {
				if (blk_rq_pos(req) > pos)
					break;
			}
			list_add_tail(&rq->queuelist, &req->queuelist);
		}
		else {
			list_for_each_entry(req, &nd->smaller, queuelist) {
				if (blk_rq_pos(req) < pos)
					break;
			}
			list_add_tail(&rq->queuelist, &req->queuelist);
		}
	}
	printk(KERN_EMERG "[SSTF] add %c %lu\n", direction, blk_rq_pos(rq));
}

static int sstf_init_queue(struct request_queue *q, struct elevator_type *e){
	struct sstf_data *nd;
	struct elevator_queue *eq;
	/* Implementação da inicialização da fila (queue).
	 *
	 * Use como exemplo a inicialização da fila no driver noop-iosched.c
	 *
	 */

	eq = elevator_alloc(q, e);
	if (!eq)
		return -ENOMEM;

	nd = kmalloc_node(sizeof(*nd), GFP_KERNEL, q->node);
	if (!nd) {
		printk(KERN_ALERT "Failed to allocate memory.\n");
		return NULL;
	}
	eq->elevator_data = nd;

	INIT_LIST_HEAD(&nd->bigger);
	INIT_LIST_HEAD(&nd->smaller);

	spin_lock_irq(q->queue_lock);
	q->elevator = eq;
	spin_unlock_irq(q->queue_lock);

	return 0;
}

static void sstf_exit_queue(struct elevator_queue *e)
{
	struct sstf_data *nd = e->elevator_data;

	/* Implementação da finalização da fila (queue).
	 *
	 * Use como exemplo o driver noop-iosched.c
	 *
	 */
	kfree(nd);
}

static struct request *
sstf_former_request(struct request_queue *q, struct request *rq)
{
	struct sstf_data *nd = q->elevator->elevator_data;

	if (rq->queuelist.prev == &nd->bigger || rq->queuelist.prev == &nd->smaller)
		return NULL;
	return list_entry_rq(rq->queuelist.prev);
}

static struct request *
sstf_latter_request(struct request_queue *q, struct request *rq)
{
	struct sstf_data *nd = q->elevator->elevator_data;

	if (rq->queuelist.next == &nd->bigger || rq->queuelist.next == &nd->smaller)
		return NULL;
	return list_entry_rq(rq->queuelist.next);
}

/* Infrastrutura dos drivers de IO Scheduling. */
static struct elevator_type elevator_sstf = {
	.ops.sq = {
		.elevator_merge_req_fn		= sstf_merged_requests,
		.elevator_dispatch_fn		= sstf_dispatch,
		.elevator_add_req_fn		= sstf_add_request,
		.elevator_former_req_fn		= sstf_former_request,
		.elevator_latter_req_fn		= sstf_latter_request,
		.elevator_init_fn		= sstf_init_queue,
		.elevator_exit_fn		= sstf_exit_queue,
	},
	.elevator_name = "sstf",
	.elevator_owner = THIS_MODULE,
};

/* Inicialização do driver. */
static int __init sstf_init(void)
{
	return elv_register(&elevator_sstf);
}

/* Finalização do driver. */
static void __exit sstf_exit(void)
{
	elv_unregister(&elevator_sstf);
}

module_init(sstf_init);
module_exit(sstf_exit);

MODULE_AUTHOR("Sergio Johann Filho");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("SSTF IO scheduler");