#ifndef ACTIONLIST_H
#define ACTIONLIST_H

#include "classlist.h"
#include "stl-model.h"

#define ISACT 1
#define ALLBITS 4
#define ALLNODESIZE (1 << ALLBITS)
#define ALLMASK ((1 << ALLBITS)-1)
#define MODELCLOCKBITS 32

class allnode;
void decrementCount(allnode *);

class allnode {
public:
	allnode();
	~allnode();
	SNAPSHOTALLOC;

private:
	allnode * parent;
	allnode * children[ALLNODESIZE];
	int count;
	sllnode<ModelAction *> * findPrev(modelclock_t index);
	friend class actionlist;
	friend void decrementCount(allnode *);
};

class actionlist {
public:
	actionlist();
	~actionlist();
	void addAction(ModelAction * act);
	void removeAction(ModelAction * act);
	void clear();
	bool isEmpty();
	uint size() {return _size;}
	sllnode<ModelAction *> * begin() {return head;}
	sllnode<ModelAction *> * end() {return tail;}

	SNAPSHOTALLOC;

private:
	allnode root;
	sllnode<ModelAction *> * head;
	sllnode<ModelAction* > * tail;

	uint _size;
};
#endif
