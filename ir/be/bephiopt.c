/**
 * @author Daniel Grund
 * @date 04.01.2005
 */

#include <stdlib.h>
#include <stdio.h>

#include "pset.h"
#include "obst.h"
#include "irgraph.h"
#include "irnode.h"
#include "irgwalk.h"
#include "irouts.h"
#include "irdom.h"

#include "bera_t.h"
#include "bephiopt.h"
#include "phiclass_t.h"
#include "bephicoal_t.h"
#include "phistat.h"

#define DEBUG_LVL SET_LEVEL_1
#define DO_PHI_STATISTICS
#define CHECK_RESULTS
#define COUNT_COPY_SAVINGS

#undef DUMP_IRG_PHI_STAT
#define DUMP_DIR_PHI_STAT
#define DUMP_ALL_PHI_STAT

#define PHI_STAT_FILE "all.phistat"
#define ENV_PHI_STAT "PHI_STAT"

static firm_dbg_module_t *dbgphi = NULL;

static void phi_node_walker(ir_node *node, void *env) {
	if (is_Phi(node) && mode_is_datab(get_irn_mode(node)))
		pset_insert_ptr((pset *)env, node);
}


static void node_collector(ir_node *node, void *env) {
	if (!is_Block(node) && is_allocatable_irn(node))
		obstack_ptr_grow(env, node);
}


#ifdef CHECK_RESULTS
static void check_result(ir_graph *irg) {
	struct obstack ob;
	ir_node **nodes, *n1, *n2;
	int i, o;

	obstack_init(&ob);
	irg_walk_graph(irg, node_collector, NULL, &ob);
	nodes = (ir_node **) obstack_finish(&ob);
	for (i = 0, n1 = nodes[i]; n1; n1 = nodes[++i])
		for (o = i+1, n2 = nodes[o]; n2; n2 = nodes[++o])
			if (phi_ops_interfere(n1, n2) && get_irn_color(n1) == get_irn_color(n2)) {
				DBG((dbgphi, 1, "Ouch! %n -- %n\n", n1, n2));
				assert(0 && "Interfering values have the same color!");
			}

	obstack_free(&ob, NULL);
}
#endif


#ifdef COUNT_COPY_SAVINGS
/* TODO: how to count copies in case of phi swapping */
static int count_copies(pset *all_phi_nodes) {
	int i, max, phi_color, res = 0;
	ir_node *phi;

	for (phi = pset_first(all_phi_nodes); phi; phi = pset_next(all_phi_nodes)) {
		phi_color = get_irn_color(phi);
		for (i = 0, max = get_irn_arity(phi); i < max; ++i)
			if (phi_color != get_irn_color(get_irn_n(phi, i)))
				res++;
	}
	return res;
}
#endif


void be_phi_opt(ir_graph* irg) {
	pset *all_phi_nodes, *all_phi_classes;
#ifdef COUNT_COPY_SAVINGS
	int before, after;
#endif

	DBG((dbgphi, 1, "\n\n=======================> IRG: %s\n\n", get_entity_name(get_irg_entity(irg))));


	/* get all phi nodes */
	DBG((dbgphi, 1, "-----------------------> Collecting phi nodes <-----------------------\n"));
	all_phi_nodes = pset_new_ptr(64);
	irg_walk_graph(irg, phi_node_walker, NULL, all_phi_nodes);


	/* get all phi congruence classes */
	DBG((dbgphi, 1, "-----------------------> Collecting phi classes <---------------------\n"));
	all_phi_classes = phi_class_compute_by_phis(all_phi_nodes);


	/* do some statistics */
#ifdef DO_PHI_STATISTICS
	DBG((dbgphi, 1, "-----------------------> Collecting phi stats <-----------------------\n"));
	phi_stat_reset();
	phi_stat_collect(irg, all_phi_nodes, all_phi_classes);
#  ifdef DUMP_IRG_PHI_STAT
	{
		char buf[1024];
		snprintf(buf, sizeof(buf), "%s.phistat", get_entity_name(get_irg_entity(irg)));
		/*phi_stat_dump(buf);*/
		phi_stat_dump_pretty(buf);
	}
#  endif
#  ifdef DUMP_DIR_PHI_STAT
	phi_stat_update(PHI_STAT_FILE);
#  endif
#  ifdef DUMP_ALL_PHI_STAT
	phi_stat_update(getenv(ENV_PHI_STAT));
#  endif
#endif


	/* try to coalesce the colors of each phi class */
	DBG((dbgphi, 1, "-----------------------> Coalescing <---------------------------------\n"));
	compute_outs(irg);
	compute_doms(irg);
	check_result(irg);
#ifdef COUNT_COPY_SAVINGS
	before = count_copies(all_phi_nodes);
#endif
	be_phi_coalesce(all_phi_classes);
#ifdef CHECK_RESULTS
	check_result(irg);
#endif
#ifdef COUNT_COPY_SAVINGS
	after = count_copies(all_phi_nodes);
	DBG((dbgphi, 1, "Irg: %s. Copies form %d to %d\n", get_entity_name(get_irg_entity(irg)), before, after));
#endif
	free_dom_and_peace(irg);
}


void be_phi_opt_init(void) {
	dbgphi = firm_dbg_register("ir.be.phiopt");
	firm_dbg_set_mask(dbgphi, DEBUG_LVL);

	phi_class_init();
	be_phi_coal_init();
}
