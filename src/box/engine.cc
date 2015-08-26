/*
 * Copyright 2010-2015, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include "tuple.h"
#include "txn.h"
#include "port.h"
#include "request.h"
#include "engine.h"
#include "space.h"
#include "exception.h"
#include "schema.h"
#include "salad/rlist.h"
#include "scoped_guard.h"
#include <stdlib.h>
#include <string.h>
#include <latch.h>
#include <errinj.h>

RLIST_HEAD(engines);

extern bool snapshot_in_progress;
extern struct latch schema_lock;

Engine::Engine(const char *engine_name)
	:name(engine_name),
	 link(RLIST_HEAD_INITIALIZER(link))
{}

void Engine::init()
{}

void Engine::beginStatement(struct txn *)
{}

void Engine::prepare(struct txn *)
{}

void Engine::commit(struct txn *)
{}

void Engine::rollback(struct txn *)
{}

void Engine::rollbackStatement(struct txn_stmt *)
{}

void Engine::initSystemSpace(struct space * /* space */)
{
	panic("not implemented");
}

void
Engine::addPrimaryKey(struct space * /* space */)
{
}

void
Engine::dropPrimaryKey(struct space * /* space */)
{
}

bool Engine::needToBuildSecondaryKey(struct space * /* space */)
{
	return true;
}

Handler::Handler(Engine *f)
	:engine(f)
{
}

void
Handler::executeReplace(struct txn*, struct space*,
                        struct request*, struct port*)
{
	tnt_raise(ClientError, ER_UNSUPPORTED, engine->name, "replace()");
}

void
Handler::executeDelete(struct txn*, struct space*, struct request*,
                       struct port*)
{
	tnt_raise(ClientError, ER_UNSUPPORTED, engine->name, "delete()");
}

void
Handler::executeUpdate(struct txn*, struct space*, struct request*,
                       struct port*)
{
	tnt_raise(ClientError, ER_UNSUPPORTED, engine->name, "update()");
}

void
Handler::executeUpsert(struct txn*, struct space*, struct request*,
                       struct port*)
{
	tnt_raise(ClientError, ER_UNSUPPORTED, engine->name, "upsert()");
}

void
Handler::executeSelect(struct txn* /* txn */, struct space *space,
                       struct request *request,
                       struct port *port)
{
	/*
	tnt_raise(ClientError, ER_UNSUPPORTED, engine->name, "select()");
	*/
	Index *index = index_find(space, request->index_id);

	ERROR_INJECT_EXCEPTION(ERRINJ_TESTING);

	uint32_t found = 0;
	uint32_t offset = request->offset;
	uint32_t limit = request->limit;
	if (request->iterator >= iterator_type_MAX)
		tnt_raise(IllegalParams, "Invalid iterator type");
	enum iterator_type type = (enum iterator_type) request->iterator;

	const char *key = request->key;
	uint32_t part_count = key ? mp_decode_array(&key) : 0;
	key_validate(index->key_def, type, key, part_count);

	struct iterator *it = index->allocIterator();
	auto it_guard = make_scoped_guard([=] {
		it->free(it);
	});
	index->initIterator(it, type, key, part_count);

	struct tuple *tuple;
	while ((tuple = it->next(it)) != NULL) {
		TupleGuard tuple_gc(tuple);
		if (offset > 0) {
			offset--;
			continue;
		}
		if (limit == found++)
			break;
		port_add_tuple(port, tuple);
	}
	if (! in_txn()) {
		 /* no txn is created, so simply collect garbage here */
		fiber_gc();
	}
}

/** Register engine instance. */
void engine_register(Engine *engine)
{
	static int n_engines;
	rlist_add_tail_entry(&engines, engine, link);
	engine->id = n_engines++;
}

/** Find engine by name. */
Engine *
engine_find(const char *name)
{
	Engine *e;
	engine_foreach(e) {
		if (strcmp(e->name, name) == 0)
			return e;
	}
	tnt_raise(LoggedError, ER_NO_SUCH_ENGINE, name);
}

/** Shutdown all engine factories. */
void engine_shutdown()
{
	Engine *e, *tmp;
	rlist_foreach_entry_safe(e, &engines, link, tmp) {
		delete e;
	}
}

void
engine_recover_to_checkpoint(int64_t checkpoint_id)
{
	/* recover engine snapshot */
	Engine *engine;
	engine_foreach(engine) {
		engine->recoverToCheckpoint(checkpoint_id);
	}
}

void
engine_begin_join()
{
	/* recover engine snapshot */
	Engine *engine;
	engine_foreach(engine) {
		engine->beginJoin();
	}
}

void
engine_end_recovery()
{
	/*
	 * For all new spaces created after recovery is complete,
	 * when the primary key is added, enable all keys.
	 */
	Engine *engine;
	engine_foreach(engine)
		engine->endRecovery();
}

int
engine_checkpoint(int64_t checkpoint_id)
{
	if (snapshot_in_progress)
		return EINPROGRESS;

	snapshot_in_progress = true;
	latch_lock(&schema_lock);

	/* create engine snapshot */
	Engine *engine;
	engine_foreach(engine) {
		if (engine->beginCheckpoint(checkpoint_id))
			goto error;
	}

	/* wait for engine snapshot completion */
	engine_foreach(engine) {
		if (engine->waitCheckpoint())
			goto error;
	}

	/* remove previous snapshot reference */
	engine_foreach(engine) {
		engine->commitCheckpoint();
	}
	latch_unlock(&schema_lock);
	snapshot_in_progress = false;
	return 0;
error:
	int save_errno = errno;
	/* rollback snapshot creation */
	engine_foreach(engine)
		engine->abortCheckpoint();
	latch_unlock(&schema_lock);
	snapshot_in_progress = false;
	return save_errno;
}

void
engine_join(Relay *relay)
{
	Engine *engine;
	engine_foreach(engine) {
		engine->join(relay);
	}
}
