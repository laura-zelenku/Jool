#ifndef _JOOL_MOD_NAMESPACE_H
#define _JOOL_MOD_NAMESPACE_H

#include "nat64/mod/common/config.h"

/**
 * A Jool translator "instance". The point is that each network namespace has
 * a separate instance (if Jool has been loaded there).
 *
 * The instance holds all the databases and configuration the translating code
 * should use to handle a packet in the respective namespace.
 */
struct xlator {
	struct net *ns;

	struct global_configuration *global;
	struct pool6 *pool6;
	union {
		struct {
			struct eam_table *eamt;
			struct addr4_pool *blacklist;
			struct addr4_pool *pool6791;

			/** MAP-T */

			/** End-user IPv6 prefix table */
			struct enduser_prefix6_table *mapt_enduprefix6_table;
			/** Mapping Rule table */
			struct mapping_rule_table *mapt_mr_table;
			/** Forwarding-Mapping-Rule(FMR) table */
			struct mapping_rule_table *mapt_fmr_table;

			struct eup6_mr_relation_table *relation_table;

		} siit;
		struct {
			struct fragdb *frag;
			struct pool4 *pool4;
			struct bib *bib;
			struct sessiondb *session;
			struct joold_queue *joold;
		} nat64;
	};

	struct config_candidate *newcfg;
};

int xlator_init(void);
void xlator_destroy(void);

int xlator_add(struct xlator *result);
int xlator_rm(void);
int xlator_replace(struct xlator *instance);

int xlator_find(struct net *ns, struct xlator *result);
int xlator_find_current(struct xlator *result);
void xlator_put(struct xlator *instance);

typedef int (*xlator_foreach_cb)(struct xlator *, void *);
int xlator_foreach(xlator_foreach_cb cb, void *args);

void xlator_copy_config(struct xlator *instance, struct full_config *copy);

#endif /* _JOOL_MOD_NAMESPACE_H */
