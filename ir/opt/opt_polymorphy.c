/*
 * Copyright (C) 1995-2008 University of Karlsruhe.  All right reserved.
 *
 * This file is part of libFirm.
 *
 * This file may be distributed and/or modified under the terms of the
 * GNU General Public License version 2 as published by the Free Software
 * Foundation and appearing in the file LICENSE.GPL included in the
 * packaging of this file.
 *
 * Licensees holding valid libFirm Professional Edition licenses may use
 * this file in accordance with the libFirm Commercial License.
 * Agreement provided with the Software.
 *
 * This file is provided AS IS with NO WARRANTY OF ANY KIND, INCLUDING THE
 * WARRANTY OF DESIGN, MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE.
 */

/**
 * @file
 * @brief   Optimize polymorphic Sel and Load nodes.
 * @author  Goetz Lindenmaier, Michael Beck
 * @version $Id$
 * @brief
 *  This file subsumes optimization code from cgana.
 */
#include "config.h"

#include "opt_polymorphy.h"
#include "iroptimize.h"
#include "irprog_t.h"
#include "entity_t.h"
#include "type_t.h"
#include "irop.h"
#include "irnode_t.h"
#include "ircons.h"

#include "iropt_dbg.h"
#include "irflag_t.h"

/**
 * Checks if a graph allocates new memory and returns the
 * type of the newly allocated entity.
 * Returns NULL if the graph did not represent an Allocation.
 *
 * The default implementation hecks for Alloc nodes only.
 */
static ir_type *default_firm_get_Alloc(ir_node *n)
{
	n = skip_Proj(n);
	if (is_Alloc(n)) {
		return get_Alloc_type(n);
	}
	return NULL;
}

/** The get_Alloc function */
static get_Alloc_func firm_get_Alloc = default_firm_get_Alloc;

/** Set a new get_Alloc_func and returns the old one. */
get_Alloc_func firm_set_Alloc_func(get_Alloc_func newf)
{
	get_Alloc_func old = firm_get_Alloc;
	firm_get_Alloc = newf;
	return old;
}

/** Return dynamic type of ptr.
 *
 * If we can deduct the dynamic type from the firm nodes
 * by a limited test we return the dynamic type.  Else
 * we return unknown_type.
 *
 * If we find a dynamic type this means that the pointer always points
 * to an object of this type during runtime.   We resolved polymorphy.
 */
static ir_type *get_dynamic_type(ir_node *ptr)
{
	ir_type *tp;

	/* skip Cast and Confirm nodes */
	for (;;) {
		unsigned code = get_irn_opcode(ptr);

		switch (code) {
		case iro_Cast:
			ptr = get_Cast_op(ptr);
			continue;
		case iro_Confirm:
			ptr = get_Confirm_value(ptr);
			continue;
		default:
			break;
		}
		break;
	}
	tp = (*firm_get_Alloc)(ptr);
	return tp ? tp : firm_unknown_type;
}

/**
 * Check, if an entity is final, i.e. is not anymore overridden.
 */
static int is_final_ent(ir_entity *ent)
{
	if (is_entity_final(ent)) {
		/* not possible to override this entity. */
		return 1;
	}
	if (get_opt_closed_world() && get_entity_n_overwrittenby(ent) == 0) {
		/* we have a closed world, so simply check how often it was
		overridden. */
		return 1;
	}
	return 0;
}

/*
 * Transform Sel[method] to SymC[method] if possible.
 */
ir_node *transform_node_Sel(ir_node *node)
{
	ir_node   *new_node, *ptr;
	ir_type   *dyn_tp;
	ir_entity *ent = get_Sel_entity(node);

	if (get_irp_phase_state() == phase_building) return node;

	if (!get_opt_dyn_meth_dispatch())
		return node;

	if (!is_Method_type(get_entity_type(ent)))
		return node;

	/* If the entity is a leave in the inheritance tree,
	   we can replace the Sel by a constant. */
	if (is_final_ent(ent)) {
		/* In dead code, we might call a leave entity that is a description.
		   Do not turn the Sel to a SymConst. */
		if (get_entity_peculiarity(ent) == peculiarity_description) {
			/* We could remove the Call depending on this Sel. */
			new_node = node;
		} else {
			new_node = copy_const_value(get_irn_dbg_info(node), get_atomic_ent_value(ent), get_nodes_block(node));
			DBG_OPT_POLY(node, new_node);
		}
		return new_node;
	}

	/* If we know the dynamic type, we can replace the Sel by a constant. */
	ptr = get_Sel_ptr(node);      /* The address we select from. */
	dyn_tp = get_dynamic_type(ptr);  /* The runtime type of ptr. */

	if (dyn_tp != firm_unknown_type) {
		ir_entity *called_ent;

		/* We know which method will be called, no dispatch necessary. */
		called_ent = resolve_ent_polymorphy(dyn_tp, ent);

		new_node = copy_const_value(get_irn_dbg_info(node), get_atomic_ent_value(called_ent), get_nodes_block(node));
		DBG_OPT_POLY(node, new_node);

		return new_node;
	}

	return node;
}

/* Transform  Load(Sel(Alloc)[constant static entity])
 *  to Const[constant static entity value].
 *
 *  This function returns a node replacing the Proj(Load)[Value].
 *  If this is actually called in transform_node, we must build
 *  a tuple, or replace the Projs of the load.
 *  Therefore we call this optimization in ldstopt().
 */
ir_node *transform_polymorph_Load(ir_node *load)
{
	ir_node *new_node = NULL;
	ir_node *field_ptr, *ptr;
	ir_entity *ent;
	ir_type *dyn_tp;

	if (!get_opt_dyn_meth_dispatch())
		return load;

	field_ptr = get_Load_ptr(load);

	if (! is_Sel(field_ptr)) return load;

	ent = get_Sel_entity(field_ptr);
	if ( !(get_entity_linkage(ent) & IR_LINKAGE_CONSTANT) )
		return load;

	/* If the entity is a leave in the inheritance tree,
	   we can replace the Sel by a constant. */
	if ((get_irp_phase_state() != phase_building) && is_final_ent(ent)) {
		new_node = get_atomic_ent_value(ent);
	} else {
		/* If we know the dynamic type, we can replace the Sel by a constant. */
		ptr = get_Sel_ptr(field_ptr);    /* The address we select from. */
		dyn_tp = get_dynamic_type(ptr);  /* The runtime type of ptr. */

		if (dyn_tp != firm_unknown_type) {
			ir_entity *loaded_ent;

			/* We know which method will be called, no dispatch necessary. */
			loaded_ent = resolve_ent_polymorphy(dyn_tp, ent);
			new_node = get_atomic_ent_value(loaded_ent);
		}
	}
	if (new_node != NULL) {
		new_node = can_replace_load_by_const(load, new_node);
		if (new_node != NULL) {
			DBG_OPT_POLY(field_ptr, new_node);

			return new_node;
		}
	}
	return load;
}