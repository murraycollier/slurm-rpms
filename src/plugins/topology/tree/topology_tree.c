/*****************************************************************************\
 *  topology_tree.c - Build configuration information for hierarchical
 *	switch topology
 *****************************************************************************
 *  Copyright (C) 2009 Lawrence Livermore National Security.
 *  Copyright (C) 2023 NVIDIA CORPORATION.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Morris Jette <jette1@llnl.gov>
 *  CODE-OCEC-09-009. All rights reserved.
 *
 *  This file is part of Slurm, a resource management program.
 *  For details, see <https://slurm.schedmd.com/>.
 *  Please also read the included file: DISCLAIMER.
 *
 *  Slurm is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  In addition, as a special exception, the copyright holders give permission
 *  to link the code of portions of this program with the OpenSSL library under
 *  certain conditions as described in each individual source file, and
 *  distribute linked combinations including the two. You must obey the GNU
 *  General Public License in all respects for all of the code used other than
 *  OpenSSL. If you modify file(s) with this exception, you may extend this
 *  exception to your version of the file(s), but you are not obligated to do
 *  so. If you do not wish to do so, delete this exception statement from your
 *  version.  If you delete this exception statement from all source files in
 *  the program, then also delete it here.
 *
 *  Slurm is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with Slurm; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
\*****************************************************************************/

#include <math.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/types.h>

#include "src/common/slurm_xlator.h"

#include "slurm/slurm_errno.h"
#include "src/common/bitstring.h"
#include "src/common/log.h"
#include "src/common/node_conf.h"
#include "src/common/xstring.h"

#include "src/slurmctld/locks.h"
#include "src/slurmctld/slurmctld.h"

#include "eval_nodes_tree.h"

/* These are defined here so when we link with something other than
 * the slurmctld we will have these symbols defined.  They will get
 * overwritten when linking with the slurmctld.
 */
#if defined (__APPLE__)
extern node_record_t **node_record_table_ptr __attribute__((weak_import));
extern int node_record_count __attribute__((weak_import));
extern int active_node_record_count __attribute__((weak_import));
#else
node_record_t **node_record_table_ptr;
int node_record_count;
int active_node_record_count;
#endif

/*
 * These variables are required by the generic plugin interface.  If they
 * are not found in the plugin, the plugin loader will ignore it.
 *
 * plugin_name - a string giving a human-readable description of the
 * plugin.  There is no maximum length, but the symbol must refer to
 * a valid string.
 *
 * plugin_type - a string suggesting the type of the plugin or its
 * applicability to a particular form of data or method of data handling.
 * If the low-level plugin API is used, the contents of this string are
 * unimportant and may be anything.  Slurm uses the higher-level plugin
 * interface which requires this string to be of the form
 *
 *      <application>/<method>
 *
 * where <application> is a description of the intended application of
 * the plugin (e.g., "task" for task control) and <method> is a description
 * of how this plugin satisfies that application.  Slurm will only load
 * a task plugin if the plugin_type string has a prefix of "task/".
 *
 * plugin_version - an unsigned 32-bit integer containing the Slurm version
 * (major.minor.micro combined into a single number).
 */
const char plugin_name[]        = "topology tree plugin";
const char plugin_type[]        = "topology/tree";
const uint32_t plugin_id = TOPOLOGY_PLUGIN_TREE;
const uint32_t plugin_version   = SLURM_VERSION_NUMBER;
const bool supports_exclusive_topo = false;

typedef topo_info_t topoinfo_switch_t;

typedef struct topoinfo_tree {
	uint32_t record_count;		/* number of records */
	topoinfo_switch_t *topo_array;	/* the switch topology records */
} topoinfo_tree_t;

/*
 * init() is called when the plugin is loaded, before any other functions
 *	are called.  Put global initialization here.
 */
extern int init(void)
{
	verbose("%s loaded", plugin_name);
	return SLURM_SUCCESS;
}

/*
 * fini() is called when the plugin is removed. Clear any allocated
 *	storage here.
 */
extern int fini(void)
{
	return SLURM_SUCCESS;
}

extern int topology_p_add_rm_node(node_record_t *node_ptr, char *unit,
				  topology_ctx_t *tctx)
{
	tree_context_t *ctx = tctx->plugin_ctx;
	bool *added = NULL;
	int add_inx = -1;
	char *tmp_str = NULL, *tok = NULL, *saveptr = NULL;
	int rc = SLURM_SUCCESS;

	if (unit) {
		tmp_str = xstrdup(unit);
		tok = strtok_r(tmp_str, ":", &saveptr);
	}

	while (tok) {
		int inx = switch_record_get_switch_inx(tok, ctx);

		if ((inx < 0) && (add_inx < 0)) {
			error("Don't know where to add switch %s", tok);
			rc = SLURM_ERROR;
			goto fini;
		}
		if (inx < 0)
			inx = switch_record_add_switch(tctx, tok, add_inx);

		if (inx < 0) {
			error("Failed to add switch %s", tok);
			rc = SLURM_ERROR;
			goto fini;
		}
		tok = strtok_r(NULL, ":", &saveptr);
		add_inx = inx;
	}

	if ((add_inx >= 0) && (ctx->switch_table[add_inx].level != 0)) {
		error("%s isn't a leaf switch", ctx->switch_table[add_inx].name);
		rc = SLURM_ERROR;
		goto fini;
	}

	added = xcalloc(ctx->switch_count, sizeof(bool));
	for (int i = 0; i < ctx->switch_count; i++) {
		bool add, in_switch;
		int sw = i;

		if (ctx->switch_table[i].level != 0)
			continue;

		in_switch = bit_test(ctx->switch_table[i].node_bitmap,
				     node_ptr->index);
		add = (add_inx == i);

		if ((!in_switch && !add) || (in_switch && add))
			continue;

		while (sw != SWITCH_NO_PARENT) {
			if (added[sw])
				break;

			if (add && !in_switch) {
				debug2("%s: add %s to %s",
				       __func__, node_ptr->name,
				       ctx->switch_table[sw].name);
				bit_set(ctx->switch_table[sw].node_bitmap,
					node_ptr->index);
				added[sw] = true;
			} else if (!add && in_switch) {
				debug2("%s: remove %s from %s",
				       __func__, node_ptr->name,
				       ctx->switch_table[sw].name);
				bit_clear(ctx->switch_table[sw].node_bitmap,
					  node_ptr->index);
			}
			xfree(ctx->switch_table[sw].nodes);
			ctx->switch_table[sw].nodes =
				bitmap2node_name(ctx->switch_table[sw]
							 .node_bitmap);
			switch_record_update_block_config(tctx, sw);
			sw = ctx->switch_table[sw].parent;
		}
	}
fini:
	xfree(added);
	xfree(tmp_str);
	return rc;
}

/*
 * topo_build_config - build or rebuild system topology information
 *	after a system startup or reconfiguration.
 */
extern int topology_p_build_config(topology_ctx_t *tctx)
{
	if (node_record_count)
		return switch_record_validate(tctx);
	return SLURM_SUCCESS;
}

extern int topology_p_destroy_config(topology_ctx_t *tctx)
{
	tree_context_t *ctx = tctx->plugin_ctx;

	switch_record_table_destroy(ctx);
	xfree(tctx->plugin_ctx);

	return SLURM_SUCCESS;
}

extern int topology_p_eval_nodes(topology_eval_t *topo_eval)
{
	topo_eval->eval_nodes = eval_nodes_tree;
	topo_eval->trump_others = false;

	return common_topo_choose_nodes(topo_eval);
}

extern int topology_p_whole_topo(bitstr_t *node_mask, void *tctx)
{
	tree_context_t *ctx = tctx;
	for (int i = 0; i < ctx->switch_count; i++) {
		if (ctx->switch_table[i].level != 0)
			continue;
		if (bit_overlap_any(ctx->switch_table[i].node_bitmap,
				    node_mask)) {
			bit_or(node_mask, ctx->switch_table[i].node_bitmap);
		}
	}
	return SLURM_SUCCESS;
}

/*
 * Get bitmap of nodes in switch
 *
 * IN name of block
 * RET bitmap of nodes from ctx->switch_table (do not free)
 */
extern bitstr_t *topology_p_get_bitmap(char *name, void *tctx)
{
	tree_context_t *ctx = tctx;

	for (int i = 0; i < ctx->switch_count; i++) {
		if (!xstrcmp(ctx->switch_table[i].name, name)) {
			return ctx->switch_table[i].node_bitmap;
		}
	}

	return NULL;
}

/*
 * When TopologyParam=SwitchAsNodeRank is set, this plugin assigns a unique
 * node_rank for all nodes belonging to the same leaf switch.
 */
extern bool topology_p_generate_node_ranking(topology_ctx_t *tctx)
{
	/* By default, node_rank is 0, so start at 1 */
	int switch_rank = 1;
	tree_context_t *ctx;

	if (!xstrcasestr(slurm_conf.topology_param, "SwitchAsNodeRank"))
		return false;

	/* Build a temporary topology to be able to find the leaf switches. */
	switch_record_validate(tctx);

	ctx = tctx->plugin_ctx;

	if (ctx->switch_count == 0) {
		topology_p_destroy_config(tctx);
		return false;
	}

	for (int sw = 0; sw < ctx->switch_count; sw++) {
		/* skip if not a leaf switch */
		if (ctx->switch_table[sw].level != 0)
			continue;

		for (int n = 0; n < node_record_count; n++) {
			if (!bit_test(ctx->switch_table[sw].node_bitmap, n))
				continue;
			node_record_table_ptr[n]->node_rank = switch_rank;
			debug("node=%s rank=%d",
			      node_record_table_ptr[n]->name, switch_rank);
		}

		switch_rank++;
	}

	/* Discard the temporary topology since it is using node bitmaps */
	topology_p_destroy_config(tctx);

	return true;
}

/*
 * topo_get_node_addr - build node address and the associated pattern
 *      based on the topology information
 *
 * example of output :
 *      address : s0.s4.s8.tux1
 *      pattern : switch.switch.switch.node
 */
extern int topology_p_get_node_addr(char *node_name, char **paddr,
				    char **ppattern, void *tctx)
{
	node_record_t *node_ptr;
	hostlist_t *sl = NULL;
	tree_context_t *ctx = tctx;

	int s_max_level = 0;
	int i, j;

	/* no switches found, return */
	if (ctx->switch_count == 0) {
		*paddr = xstrdup(node_name);
		*ppattern = xstrdup("node");
		return SLURM_SUCCESS;
	}

	node_ptr = find_node_record(node_name);
	/* node not found in configuration */
	if ( node_ptr == NULL )
		return SLURM_ERROR;

	/* look for switches max level */
	for (i = 0; i < ctx->switch_count; i++) {
		if (ctx->switch_table[i].level > s_max_level)
			s_max_level = ctx->switch_table[i].level;
	}

	/* initialize output parameters */
	*paddr = xstrdup("");
	*ppattern = xstrdup("");

	/* build node topology address and the associated pattern */
	for (j = s_max_level; j >= 0; j--) {
		for (i = 0; i < ctx->switch_count; i++) {
			if (ctx->switch_table[i].level != j)
				continue;
			if (!bit_test(ctx->switch_table[i].node_bitmap,
				      node_ptr->index))
				continue;
			if (sl == NULL) {
				sl = hostlist_create(ctx->switch_table[i].name);
			} else {
				hostlist_push_host(sl,
						   ctx->switch_table[i].name);
			}
		}
		if (sl) {
			char *buf = hostlist_ranged_string_xmalloc(sl);
			xstrcat(*paddr,buf);
			xfree(buf);
			hostlist_destroy(sl);
			sl = NULL;
		}
		xstrcat(*paddr, ".");
		xstrcat(*ppattern, "switch.");
	}

	/* append node name */
	xstrcat(*paddr, node_name);
	xstrcat(*ppattern, "node");

	return SLURM_SUCCESS;
}

/*
 * _subtree_split_hostlist() split a hostlist into topology aware subhostlists
 *
 * IN/OUT nodes_bitmap - bitmap of all hosts that need to be sent
 * IN parent - location in ctx->switch_table
 * IN/OUT msg_count - running count of how many messages we need to send
 * IN/OUT sp_hl - array of subhostlists
 * IN/OUT count - position in sp_hl array
 */
static int _subtree_split_hostlist(bitstr_t *nodes_bitmap, int parent,
				   int *msg_count, hostlist_t ***sp_hl,
				   int *count, tree_context_t *ctx)
{
	int lst_count = 0, sw_count;
	bitstr_t *fwd_bitmap = NULL;		/* nodes in forward list */

	for (int i = 0; i < ctx->switch_table[parent].num_switches; i++) {
		int k = ctx->switch_table[parent].switch_index[i];

		if (!fwd_bitmap)
			fwd_bitmap = bit_copy(ctx->switch_table[k].node_bitmap);
		else
			bit_copybits(fwd_bitmap,
				     ctx->switch_table[k].node_bitmap);
		bit_and(fwd_bitmap, nodes_bitmap);
		sw_count = bit_set_count(fwd_bitmap);
		if (sw_count == 0) {
			continue; /* no nodes on this switch in message list */
		}
		(*sp_hl)[*count] = bitmap2hostlist(fwd_bitmap);
		/* Now remove nodes from this switch from message list */
		bit_and_not(nodes_bitmap, fwd_bitmap);
		if (slurm_conf.debug_flags & DEBUG_FLAG_ROUTE) {
			char *buf;
			buf = hostlist_ranged_string_xmalloc((*sp_hl)[*count]);
			debug("ROUTE: ... sublist[%d] switch=%s :: %s",
			      i, ctx->switch_table[i].name, buf);
			xfree(buf);
		}
		(*count)++;
		lst_count += sw_count;
		if (lst_count == *msg_count)
			break; /* all nodes in message are in a child list */
	}
	*msg_count -= lst_count;

	FREE_NULL_BITMAP(fwd_bitmap);
	return lst_count;
}

extern int topology_p_split_hostlist(hostlist_t *hl, hostlist_t ***sp_hl,
				     int *count, uint16_t tree_width,
				     void *tctx)
{
	int i, j, k, msg_count, switch_count, switch_nodes_cnt, depth = 0,
		upper_switch_level = 0;
	int s_first, s_last;
	char *buf;
	bitstr_t *nodes_bitmap = NULL;		/* nodes in message list */
	bitstr_t *switch_bitmap = NULL;		/* switches  */
	slurmctld_lock_t node_read_lock = { .node = READ_LOCK };
	static pthread_mutex_t init_lock = PTHREAD_MUTEX_INITIALIZER;
	tree_context_t *ctx = tctx;

	if (!common_topo_route_tree()) {
		return common_topo_split_hostlist_treewidth(
			hl, sp_hl, count, tree_width);
	}

	slurm_mutex_lock(&init_lock);
	if (ctx->switch_count == 0) {
		if (running_in_slurmctld())
			fatal_abort("%s: Somehow we have 0 for ctx->switch_count and we are here in the slurmctld.  This should never happen.", __func__);
		/* configs have not already been processed */
		init_node_conf();
		build_all_nodeline_info(false, 0);
		rehash_node();

		if (topology_g_build_config() != SLURM_SUCCESS) {
			fatal("ROUTE: Failed to build topology config");
		}
	}
	slurm_mutex_unlock(&init_lock);

	/* Only acquire the slurmctld lock if running as the slurmctld. */
	if (running_in_slurmctld())
		lock_slurmctld(node_read_lock);

	/* create bitmap of nodes to send message too */
	if (hostlist2bitmap(hl, false, &nodes_bitmap) != SLURM_SUCCESS) {
		buf = hostlist_ranged_string_xmalloc(hl);
		fatal("ROUTE: Failed to make bitmap from hostlist=%s.", buf);
	}

	/* Find lowest level switches containing all the nodes in the list */
	switch_bitmap = bit_alloc(ctx->switch_count);
	for (j = 0; j < ctx->switch_count; j++) {
		if ((ctx->switch_table[j].level == 0) &&
		    (switch_nodes_cnt =
			     bit_overlap(ctx->switch_table[j].node_bitmap,
					 nodes_bitmap))) {
			/*
			 * Examine the standard forward tree depth for the leaf
			 * switches, and consider the final depth as the max
			 * value for all them
			 */
			int switch_nodes_tree_depth =
				ceil(log2(switch_nodes_cnt * (tree_width - 1) +
					  1) / log2(tree_width));
			depth = MAX(depth, switch_nodes_tree_depth);
			bit_set(switch_bitmap, j);
		}
	}

	switch_count = bit_set_count(switch_bitmap);

	for (i = 1; i <= ctx->switch_levels; i++) {
		/* All nodes in message list are in one switch */
		if (switch_count < 2)
			break;
		for (j = 0; j < ctx->switch_count; j++) {
			if (switch_count < 2)
				break;
			int level = ctx->switch_table[j].level;
			if (level == i) {
				int first_child = -1, child_cnt = 0, num_desc;
				num_desc =
					ctx->switch_table[j].num_desc_switches;
				for (k = 0; k < num_desc; k++) {
					int index =
						ctx->switch_table[j]
							.switch_desc_index[k];
					if (bit_test(switch_bitmap, index)) {
						child_cnt++;
						if (child_cnt > 1) {
							bit_clear(switch_bitmap,
								  index);
						} else {
							first_child = index;
						}
					}
				}
				if (child_cnt > 1) {
					/*
					 * Track the uppermost level for all the
					 * intermediate switches
					 */
					upper_switch_level = MAX(
							     upper_switch_level,
							     level);
					bit_clear(switch_bitmap, first_child);
					bit_set(switch_bitmap, j);
					switch_count -= (child_cnt - 1);
				}
			}
		}
	}

	/*
	 * The final depth for this hostlist is: the sum of the max depth caused
	 * by the intermediate switches, plus the max depth of those standard
	 * forward trees hanging of the leaf switches
	 */
	depth += upper_switch_level;

	s_first = bit_ffs(switch_bitmap);
	if (s_first != -1)
		s_last = bit_fls(switch_bitmap);
	else
		s_last = -2;

	if (switch_count == 1 && ctx->switch_table[s_first].level == 0 &&
	    bit_super_set(nodes_bitmap,
			  ctx->switch_table[s_first].node_bitmap)) {
		/* This is a leaf switch. Construct list based on TreeWidth */
		if (running_in_slurmctld())
			unlock_slurmctld(node_read_lock);
		FREE_NULL_BITMAP(nodes_bitmap);
		FREE_NULL_BITMAP(switch_bitmap);
		/*
		 * We are here returning the depth directly, so we don't really
		 * need our previous calculation.
		 */
		return common_topo_split_hostlist_treewidth(hl, sp_hl, count,
							    tree_width);
	}
	*sp_hl = xcalloc(ctx->switch_count, sizeof(hostlist_t *));
	msg_count = hostlist_count(hl);
	*count = 0;
	for (j = s_first; j <= s_last; j++) {
		xassert(msg_count);

		if (!bit_test(switch_bitmap, j))
			continue;
		_subtree_split_hostlist(nodes_bitmap, j, &msg_count, sp_hl,
					count, ctx);
	}
	xassert(msg_count == bit_set_count(nodes_bitmap));
	if (msg_count) {
		size_t new_size = xsize(*sp_hl);
		node_record_t *node_ptr;

		if (slurm_conf.debug_flags & DEBUG_FLAG_ROUTE) {
			buf = bitmap2node_name(nodes_bitmap);
			debug("ROUTE: didn't find switch containing nodes=%s",
			      buf);
			xfree(buf);
		}
		new_size += msg_count * sizeof(hostlist_t *);
		xrealloc(*sp_hl, new_size);

		for (j = 0; (node_ptr = next_node_bitmap(nodes_bitmap, &j));
		     j++) {
			(*sp_hl)[*count] = hostlist_create(NULL);
			hostlist_push_host((*sp_hl)[*count], node_ptr->name);
			(*count)++;
		}
	}

	if (running_in_slurmctld())
		unlock_slurmctld(node_read_lock);
	FREE_NULL_BITMAP(nodes_bitmap);
	FREE_NULL_BITMAP(switch_bitmap);

	return depth;
}

extern int topology_p_topology_free(void *topoinfo_ptr)
{
	int i = 0;
	topoinfo_tree_t *topoinfo = topoinfo_ptr;
	if (topoinfo) {
		if (topoinfo->topo_array) {
			for (i = 0; i < topoinfo->record_count; i++) {
				xfree(topoinfo->topo_array[i].name);
				xfree(topoinfo->topo_array[i].nodes);
				xfree(topoinfo->topo_array[i].switches);
			}
			xfree(topoinfo->topo_array);
		}
		xfree(topoinfo);
	}
	return SLURM_SUCCESS;
}

extern int topology_p_get(topology_data_t type, void *data, void *tctx)
{
	int rc = SLURM_SUCCESS;
	tree_context_t *ctx = tctx;

	switch (type) {
	case TOPO_DATA_TOPOLOGY_PTR:
	{
		dynamic_plugin_data_t **topoinfo_pptr = data;
		topoinfo_tree_t *topoinfo_ptr =
			xmalloc(sizeof(topoinfo_tree_t));

		*topoinfo_pptr = xmalloc(sizeof(dynamic_plugin_data_t));
		(*topoinfo_pptr)->data = topoinfo_ptr;
		(*topoinfo_pptr)->plugin_id = plugin_id;

		topoinfo_ptr->record_count = ctx->switch_count;
		topoinfo_ptr->topo_array = xcalloc(topoinfo_ptr->record_count,
						   sizeof(topoinfo_switch_t));

		for (int i = 0; i < topoinfo_ptr->record_count; i++) {
			topoinfo_ptr->topo_array[i].level =
				ctx->switch_table[i].level;
			topoinfo_ptr->topo_array[i].link_speed =
				ctx->switch_table[i].link_speed;
			topoinfo_ptr->topo_array[i].name =
				xstrdup(ctx->switch_table[i].name);
			topoinfo_ptr->topo_array[i].nodes =
				xstrdup(ctx->switch_table[i].nodes);
			topoinfo_ptr->topo_array[i].switches =
				xstrdup(ctx->switch_table[i].switches);
		}
		break;
	}
	case TOPO_DATA_REC_CNT:
	{
		int *rec_cnt = data;
		*rec_cnt = ctx->switch_count;
		break;
	}
	case TOPO_DATA_EXCLUSIVE_TOPO:
	{
		int *exclusive_topo = data;
		*exclusive_topo = 0;
		break;
	}
	default:
		error("Unsupported option %d", type);
		rc = SLURM_ERROR;
		break;
	}

	return rc;
}

extern int topology_p_topology_pack(void *topoinfo_ptr, buf_t *buffer,
				    uint16_t protocol_version)
{
	int i;
	topoinfo_tree_t *topoinfo = topoinfo_ptr;

	pack32(topoinfo->record_count, buffer);
	for (i = 0; i < topoinfo->record_count; i++) {
		pack16(topoinfo->topo_array[i].level, buffer);
		pack32(topoinfo->topo_array[i].link_speed, buffer);
		packstr(topoinfo->topo_array[i].name, buffer);
		packstr(topoinfo->topo_array[i].nodes, buffer);
		packstr(topoinfo->topo_array[i].switches, buffer);
	}
	return SLURM_SUCCESS;
}
void _print_topo_record(topoinfo_switch_t * topo_ptr, char **out)
{
	char *env, *line = NULL, *pos = NULL;

	/****** Line 1 ******/
	xstrfmtcatat(line, &pos, "SwitchName=%s Level=%u LinkSpeed=%u",
		     topo_ptr->name, topo_ptr->level, topo_ptr->link_speed);

	if (topo_ptr->nodes)
		xstrfmtcatat(line, &pos, " Nodes=%s", topo_ptr->nodes);

	if (topo_ptr->switches)
		xstrfmtcatat(line, &pos, " Switches=%s", topo_ptr->switches);

	if ((env = getenv("SLURM_TOPO_LEN")))
		xstrfmtcat(*out, "%.*s\n", atoi(env), line);
	else
		xstrfmtcat(*out, "%s\n", line);

	xfree(line);

}

extern int topology_p_topology_print(void *topoinfo_ptr, char *nodes_list,
				     char *unit, char **out)
{
	int i, match, match_cnt = 0;;
	topoinfo_tree_t *topoinfo = topoinfo_ptr;

	*out = NULL;

	if ((!nodes_list || (nodes_list[0] == '\0')) &&
	    (!unit || (unit[0] == '\0'))) {
		if (topoinfo->record_count == 0) {
			error("No topology information available");
			return SLURM_SUCCESS;
		}

		for (i = 0; i < topoinfo->record_count; i++)
			_print_topo_record(&topoinfo->topo_array[i], out);

		return SLURM_SUCCESS;
	}

	/* Search for matching switch name and node name*/
	for (i = 0; i < topoinfo->record_count; i++) {
		hostset_t *hs;

		if (unit && xstrcmp(topoinfo->topo_array[i].name, unit))
			continue;

		if (nodes_list) {
			if ((topoinfo->topo_array[i].nodes == NULL) ||
			    (topoinfo->topo_array[i].nodes[0] == '\0'))
				continue;

			hs = hostset_create(topoinfo->topo_array[i].nodes);
			if (hs == NULL)
				fatal("hostset_create: memory allocation failure");
			match = hostset_within(hs, nodes_list);
			hostset_destroy(hs);
			if (!match)
				continue;
		}
		match_cnt++;
		_print_topo_record(&topoinfo->topo_array[i], out);
	}

	if (match_cnt == 0) {
		error("Topology information contains no switch%s%s%s%s",
		      unit ? " named " : "",
		      unit ? unit : "",
		      nodes_list ? " with nodes " : "",
		      nodes_list ? nodes_list : "");
	}
	return SLURM_SUCCESS;
}

extern int topology_p_topology_unpack(void **topoinfo_pptr, buf_t *buffer,
				      uint16_t protocol_version)
{
	int i = 0;
	topoinfo_tree_t *topoinfo_ptr =
		xmalloc(sizeof(topoinfo_tree_t));

	*topoinfo_pptr = topoinfo_ptr;
	safe_unpack32(&topoinfo_ptr->record_count, buffer);
	safe_xcalloc(topoinfo_ptr->topo_array, topoinfo_ptr->record_count,
		     sizeof(topoinfo_switch_t));
	for (i = 0; i < topoinfo_ptr->record_count; i++) {
		safe_unpack16(&topoinfo_ptr->topo_array[i].level, buffer);
		safe_unpack32(&topoinfo_ptr->topo_array[i].link_speed, buffer);
		safe_unpackstr(&topoinfo_ptr->topo_array[i].name, buffer);
		safe_unpackstr(&topoinfo_ptr->topo_array[i].nodes, buffer);
		safe_unpackstr(&topoinfo_ptr->topo_array[i].switches, buffer);
	}

	return SLURM_SUCCESS;

unpack_error:
	topology_p_topology_free(topoinfo_ptr);
	*topoinfo_pptr = NULL;
	return SLURM_ERROR;
}

extern uint32_t topology_p_get_fragmentation(bitstr_t *node_mask, void *tcxt)
{
	return 0;
}
