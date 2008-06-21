/* Copyright (c) 2008 The Board of Trustees of The Leland Stanford
 * Junior University
 * 
 * We are making the OpenFlow specification and associated documentation
 * (Software) available for public use and benefit with the expectation
 * that others will use, modify and enhance the Software and contribute
 * those enhancements back to the community. However, since we would
 * like to make the Software available for broadest use, with as few
 * restrictions as possible permission is hereby granted, free of
 * charge, to any person obtaining a copy of this Software to deal in
 * the Software under the copyrights without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 * 
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 * 
 * The name and trademarks of copyright holder(s) may NOT be used in
 * advertising or publicity pertaining to the Software or any
 * derivatives without specific, written prior permission.
 */

#include <linux/module.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/delay.h>
#include <linux/if_arp.h>

#include "chain.h"
#include "table.h"
#include "flow.h"
#include "datapath.h"

/* For NetFPGA */
#include "reg_defines.h"
#include "nf2_openflow.h"
#include "hwtable-nf2.h"


/* Max number of flow entries supported by the hardware */
#define DUMMY_MAX_FLOW   8192

static void table_nf2_sfw_destroy(struct sw_flow_nf2 *sfw)
{
	/* xxx Remove the entry from hardware.  If you need to do any other
	 * xxx clean-up associated with the entry, do it here.
	 */

	kfree(sfw);
}

static void table_nf2_rcu_callback(struct rcu_head *rcu)
{
	struct sw_flow *flow = container_of(rcu, struct sw_flow, rcu);

	flow_free(flow);
}

static void table_nf2_flow_deferred_free(struct sw_flow *flow)
{
	call_rcu(&flow->rcu, table_nf2_rcu_callback);
}

static struct sw_flow *table_nf2_lookup(struct sw_table *swt,
					  const struct sw_flow_key *key)
{
	struct sw_table_nf2 *td = (struct sw_table_nf2 *) swt;
	struct sw_flow *flow;
	list_for_each_entry (flow, &td->flows, node) {
		if (flow_matches(&flow->key, key)) {
			return flow; 
		}
	}
	return NULL;
}

static int table_nf2_insert(struct sw_table *swt, struct sw_flow *flow)
{
    struct sw_table_nf2 *tb = (struct sw_table_nf2 *) swt;
    unsigned long int flags;
    struct sw_flow *f; 

    printk("Adding: ");
    printk("inport:%04x", ntohs(flow->key.in_port));
    printk(":vlan:%04x", ntohs(flow->key.dl_vlan));
    printk(" ip[%#x", flow->key.nw_src);
    printk("->%#x", flow->key.nw_dst);
    printk("] proto:%u", flow->key.nw_proto);
    printk(" tport[%d", ntohs(flow->key.tp_src));
    printk("->%d]\n", ntohs(flow->key.tp_dst));

	/* xxx Do whatever needs to be done to insert an entry in hardware. 
	 * xxx If the entry can't be inserted, return 0.  This stub code
	 * xxx doesn't do anything yet, so we're going to return 0...you
	 * xxx shouldn't.
	 */
    
	if (nf2_are_actions_supported(flow)) {
		printk("---Actions are supported---\n");
		if (nf2_build_and_write_flow(flow)) {
			printk("---build and write flow failed---\n");
			// failed
			return 0;
		}
	} else {
		// unsupported actions or no netdevice
		return 0;
	}

    /* Replace flows that match exactly. */

	spin_lock_irqsave(&tb->lock, flags);
    list_for_each_entry_rcu (f, &tb->flows, node) {
        if (f->key.wildcards == flow->key.wildcards
                && flow_matches(&f->key, &flow->key)
                && flow_del(f)) {
            list_replace_rcu(&f->node, &flow->node);
            list_replace_rcu(&f->iter_node, &flow->iter_node);
            spin_unlock_irqrestore(&tb->lock, flags);
            table_nf2_flow_deferred_free(f);
            return 1;
        }
    }

    atomic_inc(&tb->n_flows);

    list_add_rcu(&flow->node, &tb->flows);
    list_add_rcu(&flow->iter_node, &tb->iter_flows);
    spin_unlock_irqrestore(&tb->lock, flags);

    return 1;
}


static int do_delete(struct sw_table *swt, struct sw_flow *flow)
{
	if (flow_del(flow)) {
		list_del_rcu(&flow->node);
		list_del_rcu(&flow->iter_node);
		table_nf2_flow_deferred_free(flow);

    	if (flow && flow->private) {    
    		nf2_delete_private(flow->private);
	        flow->private = NULL;
    	}
		
		return 1;
	}
	return 0;
}

static int table_nf2_delete(struct sw_table *swt,
			      const struct sw_flow_key *key, uint16_t priority, int strict)
{
	struct sw_table_nf2 *td = (struct sw_table_nf2 *) swt;
	struct sw_flow *flow;
	unsigned int count = 0;

	list_for_each_entry_rcu (flow, &td->flows, node) {
		if (flow_del_matches(&flow->key, key, strict)
		    && (!strict || (flow->priority == priority)))
			count += do_delete(swt, flow);
	}
	if (count)
		atomic_sub(count, &td->n_flows);
	return count;
}


static int table_nf2_timeout(struct datapath *dp, struct sw_table *swt)
{
	struct sw_table_nf2 *td = (struct sw_table_nf2 *) swt;
	struct sw_flow *flow;
	struct sw_flow_nf2 *sfw, *n;
	int del_count = 0;
	uint64_t packet_count = 0;
	int i = 0;
	struct net_device* dev;
	
	dev = nf2_get_net_device();

	list_for_each_entry_rcu (flow, &td->flows, node) {
		/* xxx Retrieve the packet count associated with this entry
		 * xxx and store it in "packet_count".
		 */
		
		sfw = flow->private;
		if (sfw) {
			packet_count = nf2_get_packet_count(dev, sfw);			
		}

		if ((packet_count > flow->packet_count)
                    && (flow->max_idle != OFP_FLOW_PERMANENT)) {
			flow->packet_count = packet_count;
			flow->timeout = jiffies + HZ * flow->max_idle;
		}

		if (flow_timeout(flow)) {
			if (dp->flags & OFPC_SEND_FLOW_EXP) {
				/* xxx Get byte count */
                if (sfw) {
                	flow->byte_count += 
                		nf2_get_byte_count(dev, sfw);
                }
				
				dp_send_flow_expired(dp, flow);
			}
			printk("CALLING do_delete: %X %X\n", swt, flow);
			del_count += do_delete(swt, flow);
		}
		if ((i % 50) == 0) {
			msleep_interruptible(1);
		}
		i++;
	}
	
	nf2_free_net_device(dev);

	if (del_count)
		atomic_sub(del_count, &td->n_flows);
	return del_count;
}


static void table_nf2_destroy(struct sw_table *swt)
{
	struct sw_table_nf2 *td = (struct sw_table_nf2 *)swt;
    struct sw_flow_nf2* sfw = NULL;


	/* xxx This table is being destroyed, so free any data that you
	 * xxx don't want to leak.
	 */


	if (td) {
		while (!list_empty(&td->flows)) {
			struct sw_flow *flow = list_entry(td->flows.next,
							  struct sw_flow, node);
			list_del(&flow->node);
            if (flow->private) {
            	sfw = (struct sw_flow_nf2*)flow->private;
            	if (sfw->type == NF2_TABLE_EXACT) {
					add_free_exact(sfw);
            	} else if (sfw->type == NF2_TABLE_WILDCARD) {
					add_free_wildcard(sfw);
            	}
				flow->private = NULL;
            }
			flow_free(flow);
		}
		kfree(td);
	}
    destroy_exact_free_list();
    destroy_wildcard_free_list();    
}

static int table_nf2_iterate(struct sw_table *swt,
			       const struct sw_flow_key *key,
			       struct sw_table_position *position,
			       int (*callback)(struct sw_flow *, void *),
			       void *private)
{
	struct sw_table_nf2 *tl = (struct sw_table_nf2 *) swt;
	struct sw_flow *flow;
	unsigned long start;

	start = ~position->private[0];
	list_for_each_entry_rcu (flow, &tl->iter_flows, iter_node) {
		if (flow->serial <= start && flow_matches(key, &flow->key)) {
			int error = callback(flow, private);
			if (error) {
				position->private[0] = ~flow->serial;
				return error;
			}
		}
	}
	return 0;
}

static void table_nf2_stats(struct sw_table *swt,
			      struct sw_table_stats *stats)
{
	struct sw_table_nf2 *td = (struct sw_table_nf2 *) swt;
	stats->name = "nf2";
	stats->n_flows = atomic_read(&td->n_flows);
	stats->max_flows = td->max_flows;
}


static struct sw_table *table_nf2_create(void)
{
	struct sw_table_nf2 *td;
	struct sw_table *swt;
	struct net_device *dev;

	// initialize the card
	dev = nf2_get_net_device();
	nf2_reset_card(dev);
	nf2_free_net_device(dev);

	td = kzalloc(sizeof *td, GFP_KERNEL);
	if (td == NULL)
		return NULL;

	swt = &td->swt;
	swt->lookup = table_nf2_lookup;
	swt->insert = table_nf2_insert;
	swt->delete = table_nf2_delete;
	swt->timeout = table_nf2_timeout;
	swt->destroy = table_nf2_destroy;
	swt->iterate = table_nf2_iterate;
	swt->stats = table_nf2_stats;

	td->max_flows = OPENFLOW_NF2_EXACT_TABLE_SIZE + 
	OPENFLOW_WILDCARD_TABLE_SIZE-8; 
		
	atomic_set(&td->n_flows, 0);
	INIT_LIST_HEAD(&td->flows);
	INIT_LIST_HEAD(&td->iter_flows);
	spin_lock_init(&td->lock);
	td->next_serial = 0;

    spin_lock_init(&wildcard_free_lock);
    init_wildcard_free_list();
	nf2_write_static_wildcard();
	printk("initialized wildcard free list\n");

    spin_lock_init(&exact_free_lock);
    init_exact_free_list();
	printk("initialized exact free list\n");

	return swt;
}

static int __init nf2_init(void)
{
	return chain_set_hw_hook(table_nf2_create, THIS_MODULE);
}
module_init(nf2_init);

static void nf2_cleanup(void) 
{
	chain_clear_hw_hook();
}
module_exit(nf2_cleanup);

MODULE_DESCRIPTION("NetFPGA OpenFlow Hardware Table Driver");
MODULE_AUTHOR("Copyright (c) 2008 The Board of Trustees of The Leland Stanford Junior University");
MODULE_LICENSE("GPL");

