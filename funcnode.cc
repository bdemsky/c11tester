#include "funcnode.h"

FuncNode::FuncNode(ModelHistory * history) :
	history(history),
	predicate_tree_initialized(false),
	exit_count(0),
	func_inst_map(),
	inst_list(),
	entry_insts(),
	action_list_buffer(),
	predicate_tree_position()
{
	predicate_tree_entry = new Predicate(NULL, true);
	predicate_tree_entry->add_predicate_expr(NOPREDICATE, NULL, true);

	// memories that are reclaimed after each execution
	read_locations = new loc_set_t();
	val_loc_map = new HashTable<uint64_t, loc_set_t *, uint64_t, 0>();
	loc_may_equal_map = new HashTable<void *, loc_set_t *, uintptr_t, 0>();
	thrd_inst_act_map = new SnapVector<inst_act_map_t *>();

	//values_may_read_from = new value_set_t();
}

/* Reallocate snapshotted memories when new executions start */
void FuncNode::set_new_exec_flag()
{
	for (mllnode<FuncInst *> * it = inst_list.begin(); it != NULL; it = it->getNext()) {
		FuncInst * inst = it->getVal();
		inst->unset_location();
	}

	read_locations = new loc_set_t();
	val_loc_map = new HashTable<uint64_t, loc_set_t *, uint64_t, 0>();
	loc_may_equal_map = new HashTable<void *, loc_set_t *, uintptr_t, 0>();
	thrd_inst_act_map = new SnapVector<inst_act_map_t *>();

	//values_may_read_from = new value_set_t();
}

/* Check whether FuncInst with the same type, position, and location
 * as act has been added to func_inst_map or not. If not, add it.
 *
 * Note: currently, actions with the same position are filtered out by process_action,
 * so the collision list of FuncInst is not used. May remove it later. 
 */
void FuncNode::add_inst(ModelAction *act)
{
	ASSERT(act);
	const char * position = act->get_position();

	/* THREAD* actions, ATOMIC_LOCK, ATOMIC_TRYLOCK, and ATOMIC_UNLOCK
	 * actions are not tagged with their source line numbers
	 */
	if (position == NULL)
		return;

	if ( func_inst_map.contains(position) ) {
		FuncInst * inst = func_inst_map.get(position);

		ASSERT(inst->get_type() == act->get_type());

		// locations are set to NULL when new executions start
		if (inst->get_location() == NULL)
			inst->set_location(act->get_location());

		if (inst->get_location() != act->get_location())
			inst->not_single_location();

		return;
	}

	FuncInst * func_inst = new FuncInst(act, this);

	func_inst_map.put(position, func_inst);
	inst_list.push_back(func_inst);
}

/* Get the FuncInst with the same type, position, and location
 * as act
 *
 * @return FuncInst with the same type, position, and location as act */
FuncInst * FuncNode::get_inst(ModelAction *act)
{
	ASSERT(act);
	const char * position = act->get_position();

	/* THREAD* actions, ATOMIC_LOCK, ATOMIC_TRYLOCK, and ATOMIC_UNLOCK
	 * actions are not tagged with their source line numbers
	 */
	if (position == NULL)
		return NULL;

	FuncInst * inst = func_inst_map.get(position);
	if (inst == NULL)
		return NULL;

	action_type inst_type = inst->get_type();
	action_type act_type = act->get_type();

	// else if branch: an RMWRCAS action is converted to a RMW or READ action
	if (inst_type == act_type)
		return inst;
	else if (inst_type == ATOMIC_RMWRCAS &&
			(act_type == ATOMIC_RMW || act_type == ATOMIC_READ))
		return inst;

	return NULL;
}


void FuncNode::add_entry_inst(FuncInst * inst)
{
	if (inst == NULL)
		return;

	mllnode<FuncInst *> * it;
	for (it = entry_insts.begin(); it != NULL; it = it->getNext()) {
		if (inst == it->getVal())
			return;
	}

	entry_insts.push_back(inst);
}

/**
 * @brief Convert ModelAdtion list to FuncInst list 
 * @param act_list A list of ModelActions
 */
void FuncNode::update_tree(action_list_t * act_list)
{
	if (act_list == NULL || act_list->size() == 0)
		return;

	HashTable<void *, value_set_t *, uintptr_t, 4> * write_history = history->getWriteHistory();

	/* build inst_list from act_list for later processing */
	func_inst_list_t inst_list;
	action_list_t read_act_list;

	for (sllnode<ModelAction *> * it = act_list->begin(); it != NULL; it = it->getNext()) {
		ModelAction * act = it->getVal();
		FuncInst * func_inst = get_inst(act);

		if (func_inst == NULL)
			continue;

		inst_list.push_back(func_inst);

		if (func_inst->is_read()) {
			read_act_list.push_back(act);

			/* If func_inst may only read_from a single location, then:
			 *
			 * The first time an action reads from some location, import all the values that have
			 * been written to this location from ModelHistory and notify ModelHistory that this
			 * FuncNode may read from this location.
			 */
			void * loc = act->get_location();
			if (!read_locations->contains(loc) && func_inst->is_single_location()) {
				read_locations->add(loc);
				value_set_t * write_values = write_history->get(loc);
				add_to_val_loc_map(write_values, loc);
				history->add_to_loc_func_nodes_map(loc, this);
			}
		}
	}

//	model_print("function %s\n", func_name);
//	print_val_loc_map();

	update_inst_tree(&inst_list);
	update_predicate_tree(&read_act_list);

//	print_predicate_tree();
}

/** 
 * @brief Link FuncInsts in inst_list  - add one FuncInst to another's predecessors and successors
 * @param inst_list A list of FuncInsts
 */
void FuncNode::update_inst_tree(func_inst_list_t * inst_list)
{
	if (inst_list == NULL)
		return;
	else if (inst_list->size() == 0)
		return;

	/* start linking */
	sllnode<FuncInst *>* it = inst_list->begin();
	sllnode<FuncInst *>* prev;

	/* add the first instruction to the list of entry insts */
	FuncInst * entry_inst = it->getVal();
	add_entry_inst(entry_inst);

	it = it->getNext();
	while (it != NULL) {
		prev = it->getPrev();

		FuncInst * prev_inst = prev->getVal();
		FuncInst * curr_inst = it->getVal();

		prev_inst->add_succ(curr_inst);
		curr_inst->add_pred(prev_inst);

		it = it->getNext();
	}
}

void FuncNode::update_predicate_tree(action_list_t * act_list)
{
	if (act_list == NULL || act_list->size() == 0)
		return;

	/* map a FuncInst to the its predicate */
	HashTable<FuncInst *, Predicate *, uintptr_t, 0> inst_pred_map(128);

	// number FuncInsts to detect loops
	HashTable<FuncInst *, uint32_t, uintptr_t, 0> inst_id_map(128);
	uint32_t inst_counter = 0;

	HashTable<void *, ModelAction *, uintptr_t, 0> loc_act_map(128);
	HashTable<FuncInst *, ModelAction *, uintptr_t, 0> inst_act_map(128);

	sllnode<ModelAction *> *it = act_list->begin();
	Predicate * curr_pred = predicate_tree_entry;
	while (it != NULL) {
		ModelAction * next_act = it->getVal();
		FuncInst * next_inst = get_inst(next_act);

		SnapVector<Predicate *> unset_predicates = SnapVector<Predicate *>();
		bool branch_found = follow_branch(&curr_pred, next_inst, next_act, &inst_act_map, &unset_predicates);

		// no predicate expressions
		if (!branch_found && unset_predicates.size() != 0) {
			ASSERT(unset_predicates.size() == 1);
			Predicate * one_branch = unset_predicates[0];

			bool amended = amend_predicate_expr(&curr_pred, next_inst, next_act);
			if (amended)
				continue;
			else {
				curr_pred = one_branch;
				branch_found = true;
			}
		}

		// detect loops
		if (!branch_found && inst_id_map.contains(next_inst)) {
			FuncInst * curr_inst = curr_pred->get_func_inst();
			uint32_t curr_id = inst_id_map.get(curr_inst);
			uint32_t next_id = inst_id_map.get(next_inst);

			if (curr_id >= next_id) {
				Predicate * old_pred = inst_pred_map.get(next_inst);
				Predicate * back_pred = old_pred->get_parent();

				curr_pred->add_backedge(back_pred);
				curr_pred = back_pred;

				continue;
			}
		}

		// generate new branches
		if (!branch_found) {
			SnapVector<struct half_pred_expr *> half_pred_expressions;
			void * loc = next_act->get_location();

			if ( loc_act_map.contains(loc) ) {
				ModelAction * last_act = loc_act_map.get(loc);
				FuncInst * last_inst = get_inst(last_act);
				struct half_pred_expr * expression = new half_pred_expr(EQUALITY, last_inst);
				half_pred_expressions.push_back(expression);
			} else if ( next_inst->is_single_location() ){
				loc_set_t * loc_may_equal = loc_may_equal_map->get(loc);

				if (loc_may_equal != NULL) {
					loc_set_iter * loc_it = loc_may_equal->iterator();
					while (loc_it->hasNext()) {
						void * neighbor = loc_it->next();
						if (loc_act_map.contains(neighbor)) {
							ModelAction * last_act = loc_act_map.get(neighbor);
							FuncInst * last_inst = get_inst(last_act);

							struct half_pred_expr * expression = new half_pred_expr(EQUALITY, last_inst);
							half_pred_expressions.push_back(expression);
						}
					}
				} 
			} else {
				// next_inst is not single location
				uint64_t read_val = next_act->get_reads_from_value();

				// only generate NULLITY predicate when it is actually NULL.
				if ( (void*)read_val == NULL) {
					struct half_pred_expr * expression = new half_pred_expr(NULLITY, NULL);
					half_pred_expressions.push_back(expression);
				}
			}

			if (half_pred_expressions.size() == 0) {
				// no predicate needs to be generated
				Predicate * new_pred = new Predicate(next_inst);
				curr_pred->add_child(new_pred);
				new_pred->set_parent(curr_pred);

				if (curr_pred->is_entry_predicate())
					new_pred->add_predicate_expr(NOPREDICATE, NULL, true);

				curr_pred = new_pred;
			} else {
				generate_predicate(&curr_pred, next_inst, &half_pred_expressions);
				bool branch_found = follow_branch(&curr_pred, next_inst, next_act, &inst_act_map, NULL);
				ASSERT(branch_found);
			}
		}

		inst_pred_map.put(next_inst, curr_pred);
		if (!inst_id_map.contains(next_inst))
			inst_id_map.put(next_inst, inst_counter++);

		loc_act_map.put(next_act->get_location(), next_act);
		inst_act_map.put(next_inst, next_act);
		it = it->getNext();
	}
}

/* Given curr_pred and next_inst, find the branch following curr_pred that
 * contains next_inst and the correct predicate. 
 * @return true if branch found, false otherwise.
 */
bool FuncNode::follow_branch(Predicate ** curr_pred, FuncInst * next_inst, ModelAction * next_act,
	HashTable<FuncInst *, ModelAction *, uintptr_t, 0> * inst_act_map,
	SnapVector<Predicate *> * unset_predicates)
{
	/* check if a branch with func_inst and corresponding predicate exists */
	bool branch_found = false;
	ModelVector<Predicate *> * branches = (*curr_pred)->get_children();
	for (uint i = 0; i < branches->size(); i++) {
		Predicate * branch = (*branches)[i];
		if (branch->get_func_inst() != next_inst)
			continue;

		/* check against predicate expressions */
		bool predicate_correct = true;
		PredExprSet * pred_expressions = branch->get_pred_expressions();
		PredExprSetIter * pred_expr_it = pred_expressions->iterator();

		if (pred_expressions->getSize() == 0) {
			predicate_correct = false;
			unset_predicates->push_back(branch);
		}

		while (pred_expr_it->hasNext()) {
			pred_expr * pred_expression = pred_expr_it->next();
			uint64_t last_read, next_read;
			bool equality;

			switch(pred_expression->token) {
				case NOPREDICATE:
					predicate_correct = true;
					break;
				case EQUALITY:
					FuncInst * to_be_compared;
					ModelAction * last_act;

					to_be_compared = pred_expression->func_inst;
					last_act = inst_act_map->get(to_be_compared);

					last_read = last_act->get_reads_from_value();
					next_read = next_act->get_reads_from_value();
					equality = (last_read == next_read);
					if (equality != pred_expression->value)
						predicate_correct = false;

					break;
				case NULLITY:
					next_read = next_act->get_reads_from_value();
					equality = ((void*)next_read == NULL);
					if (equality != pred_expression->value)
						predicate_correct = false;
					break;
				default:
					predicate_correct = false;
					model_print("unkown predicate token\n");
					break;
			}
		}

		if (predicate_correct) {
			*curr_pred = branch;
			branch_found = true;
			break;
		}
	}

	return branch_found;
}

/* Able to generate complex predicates when there are multiple predciate expressions */
void FuncNode::generate_predicate(Predicate ** curr_pred, FuncInst * next_inst,
	SnapVector<struct half_pred_expr *> * half_pred_expressions)
{
	ASSERT(half_pred_expressions->size() != 0);
	SnapVector<Predicate *> predicates;

	struct half_pred_expr * half_expr = (*half_pred_expressions)[0];
	predicates.push_back(new Predicate(next_inst));
	predicates.push_back(new Predicate(next_inst));

	predicates[0]->add_predicate_expr(half_expr->token, half_expr->func_inst, true);
	predicates[1]->add_predicate_expr(half_expr->token, half_expr->func_inst, false);

	for (uint i = 1; i < half_pred_expressions->size(); i++) {
		half_expr = (*half_pred_expressions)[i];

		uint old_size = predicates.size();
		for (uint j = 0; j < old_size; j++) {
			Predicate * pred = predicates[j];
			Predicate * new_pred = new Predicate(next_inst);
			new_pred->copy_predicate_expr(pred);

			pred->add_predicate_expr(half_expr->token, half_expr->func_inst, true);
			new_pred->add_predicate_expr(half_expr->token, half_expr->func_inst, false);

			predicates.push_back(new_pred);
		}
	}

	for (uint i = 0; i < predicates.size(); i++) {
		Predicate * pred= predicates[i];
		(*curr_pred)->add_child(pred);
		pred->set_parent(*curr_pred);
	}
}

/* Amend predicates that contain no predicate expressions. Currenlty only amend with NULLITY predicates */
bool FuncNode::amend_predicate_expr(Predicate ** curr_pred, FuncInst * next_inst, ModelAction * next_act)
{
	// there should only be only child
	Predicate * unset_pred = (*curr_pred)->get_children()->back();
	uint64_t read_val = next_act->get_reads_from_value();

	// only generate NULLITY predicate when it is actually NULL.
	if ( !next_inst->is_single_location() && (void*)read_val == NULL ) {
		Predicate * new_pred = new Predicate(next_inst);

		(*curr_pred)->add_child(new_pred);
		new_pred->set_parent(*curr_pred);

		unset_pred->add_predicate_expr(NULLITY, NULL, false);
		new_pred->add_predicate_expr(NULLITY, NULL, true);

		return true;
	}

	return false;
}

void FuncNode::add_to_val_loc_map(uint64_t val, void * loc)
{
	loc_set_t * locations = val_loc_map->get(val);

	if (locations == NULL) {
		locations = new loc_set_t();
		val_loc_map->put(val, locations);
	}

	update_loc_may_equal_map(loc, locations);
	locations->add(loc);
	// values_may_read_from->add(val);
}

void FuncNode::add_to_val_loc_map(value_set_t * values, void * loc)
{
	if (values == NULL)
		return;

	value_set_iter * it = values->iterator();
	while (it->hasNext()) {
		uint64_t val = it->next();
		add_to_val_loc_map(val, loc);
	}
}

void FuncNode::update_loc_may_equal_map(void * new_loc, loc_set_t * old_locations)
{
	if ( old_locations->contains(new_loc) )
		return;

	loc_set_t * neighbors = loc_may_equal_map->get(new_loc);

	if (neighbors == NULL) {
		neighbors = new loc_set_t();
		loc_may_equal_map->put(new_loc, neighbors);
	}

	loc_set_iter * loc_it = old_locations->iterator();
	while (loc_it->hasNext()) {
		// new_loc: { old_locations, ... }
		void * member = loc_it->next();
		neighbors->add(member);

		// for each i in old_locations, i : { new_loc, ... }
		loc_set_t * _neighbors = loc_may_equal_map->get(member);
		if (_neighbors == NULL) {
			_neighbors = new loc_set_t();
			loc_may_equal_map->put(member, _neighbors);
		}
		_neighbors->add(new_loc);
	}
}

void FuncNode::init_predicate_tree_position(thread_id_t tid)
{
	int thread_id = id_to_int(tid);
	if (predicate_tree_position.size() <= (uint) thread_id)
		predicate_tree_position.resize(thread_id + 1);

	predicate_tree_position[thread_id] = predicate_tree_entry;
}

void FuncNode::set_predicate_tree_position(thread_id_t tid, Predicate * pred)
{
	int thread_id = id_to_int(tid);
	predicate_tree_position[thread_id] = pred;
}

Predicate * FuncNode::get_predicate_tree_position(thread_id_t tid)
{
	int thread_id = id_to_int(tid);
	return predicate_tree_position[thread_id];
}

void FuncNode::init_inst_act_map(thread_id_t tid)
{
	int thread_id = id_to_int(tid);
	uint old_size = thrd_inst_act_map->size();

	if (thrd_inst_act_map->size() <= (uint) thread_id) {
		uint new_size = thread_id + 1;
		thrd_inst_act_map->resize(new_size);

		for (uint i = old_size; i < new_size; i++)
			(*thrd_inst_act_map)[i] = new inst_act_map_t(128);
	}
}

void FuncNode::reset_inst_act_map(thread_id_t tid)
{
	int thread_id = id_to_int(tid);
	inst_act_map_t * map = (*thrd_inst_act_map)[thread_id];
	map->reset();
}

void FuncNode::update_inst_act_map(thread_id_t tid, ModelAction * read_act)
{
	int thread_id = id_to_int(tid);
	inst_act_map_t * map = (*thrd_inst_act_map)[thread_id];
	FuncInst * read_inst = get_inst(read_act);
	map->put(read_inst, read_act);
}

inst_act_map_t * FuncNode::get_inst_act_map(thread_id_t tid)
{
	int thread_id = id_to_int(tid);
	return (*thrd_inst_act_map)[thread_id];
}

void FuncNode::print_predicate_tree()
{
	model_print("digraph function_%s {\n", func_name);
	predicate_tree_entry->print_pred_subtree();
	model_print("}\n");	// end of graph
}

void FuncNode::print_val_loc_map()
{
/*
	value_set_iter * val_it = values_may_read_from->iterator();
	while (val_it->hasNext()) {
		uint64_t value = val_it->next();
		model_print("val %llx: ", value);

		loc_set_t * locations = val_loc_map->get(value);
		loc_set_iter * loc_it = locations->iterator();
		while (loc_it->hasNext()) {
			void * location = loc_it->next();
			model_print("%p ", location);
		}
		model_print("\n");
	}
*/
}
