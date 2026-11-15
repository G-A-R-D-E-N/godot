/**************************************************************************/
/*  test_resource_loader_lifetime.cpp                                     */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_resource_loader_lifetime)

#include "core/io/resource.h"
#include "core/io/resource_loader.h"
#include "core/object/callable_mp.h"
#include "core/object/message_queue.h"
#include "core/object/object.h"
#include "core/os/memory.h"
#include "core/os/os.h"
#include "core/os/semaphore.h"
#include "core/string/string_name.h"
#include "core/variant/variant.h"

namespace TestResourceLoaderLifetime {

class GatedLoader : public ResourceFormatLoader {
public:
	Semaphore entered;
	Semaphore release;
	WorkerThreadPool::TaskID task_id = WorkerThreadPool::INVALID_TASK_ID;

	void release_worker() {
		release.post();
	}

	bool recognize_path(const String &p_path, const String &p_type) const override {
		return p_path.ends_with(".loader_lifetime");
	}

	Ref<Resource> load(const String &p_path, const String &p_original_path, Error *r_error,
			bool p_use_sub_threads, float *r_progress, CacheMode p_cache_mode) override {
		task_id = WorkerThreadPool::get_singleton()->get_caller_task_id();
		entered.post();
		release.wait();
		if (r_error) {
			*r_error = OK;
		}
		return memnew(Resource);
	}
};

class NestedLoader : public GatedLoader {
public:
	Ref<ResourceLoader::LoadToken> child_token;
	WorkerThreadPool::TaskID parent_task_id = WorkerThreadPool::INVALID_TASK_ID;
	String child_path = "user://child.loader_lifetime";
	bool parent_started = false;
	bool changed = false;
	CallQueue *statistics_queue = nullptr;
	bool statistics_returned = false;
	Error statistics_error = OK;
	int queue_callbacks = 0;

	void on_resource_changed() {
		changed = true;
	}

	void on_queue_callback(Ref<Resource> p_resource, String p_text) {
		queue_callbacks++;
		changed = p_resource.is_valid() && p_text == "retained";
	}

	void on_queue_statistics() {
		do {
			statistics_error = statistics_queue->push_notification(get_instance_id(), 0);
		} while (statistics_error == OK);
		statistics_returned = true;
	}

	Ref<Resource> load(const String &p_path, const String &p_original_path, Error *r_error,
			bool p_use_sub_threads, float *r_progress, CacheMode p_cache_mode) override {
		if (r_error) {
			*r_error = OK;
		}
		if (!parent_started) {
			parent_started = true;
			parent_task_id = WorkerThreadPool::get_singleton()->get_caller_task_id();
			child_token = ResourceLoader::_load_start(child_path, "Resource",
					ResourceLoader::LOAD_THREAD_SPAWN_SINGLE, ResourceLoader::CACHE_MODE_IGNORE);
			return memnew(Resource);
		}
		Ref<Resource> resource = memnew(Resource);
		resource->connect_changed(callable_mp(this, &NestedLoader::on_resource_changed));
		task_id = WorkerThreadPool::get_singleton()->get_caller_task_id();
		entered.post();
		release.wait();
		return resource;
	}
};

class AwaitingLoader : public GatedLoader {
public:
	Ref<ResourceLoader::LoadToken> child_token;
	Ref<Resource> child_resource;
	WorkerThreadPool::TaskID parent_task_id = WorkerThreadPool::INVALID_TASK_ID;
	bool changed = false;

	void on_resource_changed() {
		changed = true;
	}

	bool emitted_while_loading = false;
	bool release_child = false;

	Ref<Resource> load(const String &p_path, const String &p_original_path, Error *r_error,
			bool p_use_sub_threads, float *r_progress, CacheMode p_cache_mode) override {
		if (r_error) {
			*r_error = OK;
		}
		if (p_path.contains("grand")) {
			task_id = WorkerThreadPool::get_singleton()->get_caller_task_id();
			Ref<ResourceLoader::LoadToken> middle = ResourceLoader::_load_start("user://parent.loader_lifetime", "Resource",
					ResourceLoader::LOAD_THREAD_SPAWN_SINGLE, ResourceLoader::CACHE_MODE_IGNORE);
			uint64_t deadline = OS::get_singleton()->get_ticks_msec() + 10000;
			while ((parent_task_id == WorkerThreadPool::INVALID_TASK_ID || !WorkerThreadPool::get_singleton()->is_task_completed(parent_task_id)) && OS::get_singleton()->get_ticks_msec() < deadline) {
				OS::get_singleton()->delay_usec(100);
			}
			entered.post();
			release.wait();
			middle.unref();
			child_resource->emit_changed();
			emitted_while_loading = true;
			return memnew(Resource);
		}
		if (p_path.contains("parent")) {
			parent_task_id = WorkerThreadPool::get_singleton()->get_caller_task_id();
			child_token = ResourceLoader::_load_start("user://child.loader_lifetime", "Resource",
					ResourceLoader::LOAD_THREAD_SPAWN_SINGLE, ResourceLoader::CACHE_MODE_IGNORE);
			Error child_error;
			child_resource = ResourceLoader::_load_complete(*child_token.ptr(), &child_error);
			if (release_child) {
				child_token.unref();
			}
			return memnew(Resource);
		}
		Ref<Resource> resource = memnew(Resource);
		resource->connect_changed(callable_mp(this, &AwaitingLoader::on_resource_changed));
		return resource;
	}
};

static void wait_completed(WorkerThreadPool *p_pool, WorkerThreadPool::TaskID p_id) {
	uint64_t deadline = OS::get_singleton()->get_ticks_msec() + 20000;
	while (!p_pool->is_task_completed(p_id) && OS::get_singleton()->get_ticks_msec() < deadline) {
		OS::get_singleton()->delay_usec(100);
	}
	CHECK_MESSAGE(p_pool->is_task_completed(p_id), "Worker task did not complete before the deadline");
}

static bool wait_entered(Semaphore &p_sem) {
	uint64_t deadline = OS::get_singleton()->get_ticks_msec() + 20000;
	while (OS::get_singleton()->get_ticks_msec() < deadline) {
		if (p_sem.try_wait()) {
			return true;
		}
		OS::get_singleton()->delay_usec(100);
	}
	return false;
}

TEST_CASE("[ResourceLoader] Last worker reference retires its token") {
	Ref<GatedLoader> loader;
	loader.instantiate();
	ResourceLoader::add_resource_format_loader(loader, true);
	Ref<ResourceLoader::LoadToken> token = ResourceLoader::_load_start(
			"user://worker.loader_lifetime", "Resource", ResourceLoader::LOAD_THREAD_SPAWN_SINGLE,
			ResourceLoader::CACHE_MODE_IGNORE);
	CHECK(token.is_valid());
	if (token.is_null()) {
		ResourceLoader::remove_resource_format_loader(loader);
		return;
	}
	uint64_t deadline = OS::get_singleton()->get_ticks_msec() + 10000;
	bool entered = false;
	while (!(entered = loader->entered.try_wait()) && OS::get_singleton()->get_ticks_msec() < deadline) {
		OS::get_singleton()->delay_usec(100);
	}
	CHECK_MESSAGE(entered, "Loader worker did not enter before the deadline");
	if (!entered) {
		loader->release.post();
		Error error;
		ResourceLoader::_load_complete(**token, &error);
		token.unref();
		ResourceLoader::remove_resource_format_loader(loader);
		return;
	}
	ObjectID id = token->get_instance_id();
	token.unref();
	loader->release.post();
	auto *pool = WorkerThreadPool::get_singleton();
	wait_completed(pool, loader->task_id);
	MessageQueue::get_main_singleton()->flush();
	Object *remaining = ObjectDB::get_instance(id);
	CHECK_MESSAGE(remaining == nullptr, "Completed loader worker left its zero-reference token alive");
	ERR_PRINT_OFF;
	CHECK_FALSE(pool->is_task_completed(loader->task_id));
	ERR_PRINT_ON;
	if (remaining) {
		memdelete(remaining);
	}
	ResourceLoader::remove_resource_format_loader(loader);
}

TEST_CASE("[ResourceLoader] Nested connections survive task retirement") {
	Ref<NestedLoader> loader;
	loader.instantiate();
	bool child_first = false;
	SUBCASE("Registered child, parent retires first") {}
	SUBCASE("Unregistered child, parent retires first") {
		loader->child_path = "user://parent.loader_lifetime";
	}
	SUBCASE("Registered child, child retires first") {
		child_first = true;
	}
	SUBCASE("Unregistered child, child retires first") {
		loader->child_path = "user://parent.loader_lifetime";
		child_first = true;
	}
	ResourceLoader::add_resource_format_loader(loader, true);
	Ref<ResourceLoader::LoadToken> parent = ResourceLoader::_load_start("user://parent.loader_lifetime", "Resource",
			ResourceLoader::LOAD_THREAD_SPAWN_SINGLE, ResourceLoader::CACHE_MODE_IGNORE);
	CHECK(parent.is_valid());
	if (parent.is_null()) {
		ResourceLoader::remove_resource_format_loader(loader);
		return;
	}
	uint64_t deadline = OS::get_singleton()->get_ticks_msec() + 10000;
	bool entered = false;
	while (!(entered = loader->entered.try_wait()) && OS::get_singleton()->get_ticks_msec() < deadline) {
		OS::get_singleton()->delay_usec(100);
	}
	CHECK_MESSAGE(entered, "Nested child did not enter before the deadline");
	if (!entered) {
		loader->release.post();
	}
	auto *pool = WorkerThreadPool::get_singleton();
	wait_completed(pool, loader->parent_task_id);
	ObjectID parent_id = parent->get_instance_id();
	if (!child_first) {
		parent.unref();
		MessageQueue::get_main_singleton()->flush();
		CHECK_MESSAGE(ObjectDB::get_instance(parent_id) == nullptr, "Abandoned parent token was not retired");
	}
	CHECK(loader->child_token.is_valid());
	if (loader->child_token.is_valid()) {
		ObjectID child_id = loader->child_token->get_instance_id();
		loader->release.post();
		wait_completed(pool, loader->task_id);
		Error error;
		Ref<Resource> resource = ResourceLoader::_load_complete(*loader->child_token.ptr(), &error);
		CHECK(error == OK);
		CHECK(resource.is_valid());
		if (child_first) {
			Ref<Resource> parent_resource = ResourceLoader::_load_complete(*parent.ptr(), &error);
			CHECK(error == OK);
			CHECK(parent_resource.is_valid());
		}
		if (resource.is_valid()) {
			resource->emit_changed();
			CHECK_MESSAGE(loader->changed, "Child changed connection was lost with its abandoned parent");
		}
		loader->child_token.unref();
		MessageQueue::get_main_singleton()->flush();
		Object *remaining_child = ObjectDB::get_instance(child_id);
		CHECK(remaining_child == nullptr);
		ERR_PRINT_OFF;
		CHECK_FALSE(pool->is_task_completed(loader->task_id));
		ERR_PRINT_ON;
		if (remaining_child) {
			memdelete(remaining_child);
		}
	}
	parent.unref();
	MessageQueue::get_main_singleton()->flush();
	Object *remaining_parent = ObjectDB::get_instance(parent_id);
	CHECK(remaining_parent == nullptr);
	ERR_PRINT_OFF;
	CHECK_FALSE(pool->is_task_completed(loader->parent_task_id));
	ERR_PRINT_ON;
	if (remaining_parent) {
		memdelete(remaining_parent);
	}
	ResourceLoader::remove_resource_format_loader(loader);
}

TEST_CASE("[ResourceLoader] Propagated child connections survive parent retirement") {
	Ref<AwaitingLoader> loader;
	loader.instantiate();
	bool claim_parent = false;
	SUBCASE("Abandoned parent") {}
	SUBCASE("Claimed parent") {
		claim_parent = true;
	}
	ResourceLoader::add_resource_format_loader(loader, true);
	Ref<ResourceLoader::LoadToken> parent = ResourceLoader::_load_start("user://parent.loader_lifetime", "Resource",
			ResourceLoader::LOAD_THREAD_SPAWN_SINGLE, ResourceLoader::CACHE_MODE_IGNORE);
	CHECK(parent.is_valid());
	auto *pool = WorkerThreadPool::get_singleton();
	uint64_t deadline = OS::get_singleton()->get_ticks_msec() + 10000;
	while (loader->parent_task_id == WorkerThreadPool::INVALID_TASK_ID && OS::get_singleton()->get_ticks_msec() < deadline) {
		OS::get_singleton()->delay_usec(100);
	}
	bool finished = loader->parent_task_id != WorkerThreadPool::INVALID_TASK_ID;
	while (finished && !pool->is_task_completed(loader->parent_task_id) && OS::get_singleton()->get_ticks_msec() < deadline) {
		OS::get_singleton()->delay_usec(100);
	}
	finished = finished && pool->is_task_completed(loader->parent_task_id);
	CHECK_MESSAGE(finished, "Parent load did not finish before the deadline");
	if (finished && loader->child_resource.is_valid()) {
		ObjectID parent_id = parent->get_instance_id();
		if (claim_parent) {
			Error error;
			CHECK(ResourceLoader::_load_complete(*parent.ptr(), &error).is_valid());
		}
		parent.unref();
		MessageQueue::get_main_singleton()->flush();
		CHECK(ObjectDB::get_instance(parent_id) == nullptr);
		loader->child_resource->emit_changed();
		CHECK_MESSAGE(loader->changed, "Child changed connection was lost with its retired parent");
	} else {
		CHECK_MESSAGE(false, "Parent load did not return the awaited child resource");
	}
	parent.unref();
	loader->child_resource.unref();
	loader->child_token.unref();
	MessageQueue::get_main_singleton()->flush();
	ResourceLoader::clear_thread_load_tasks();
	ResourceLoader::remove_resource_format_loader(loader);
}

TEST_CASE("[ResourceLoader] Retired child connections follow a live parent") {
	if (WorkerThreadPool::get_singleton()->get_thread_count() * 3 / 10 < 2) {
		MESSAGE("Needs two low-priority worker slots; the grandparent load holds one while the parent runs");
		return;
	}
	Ref<AwaitingLoader> loader;
	loader.instantiate();
	ResourceLoader::add_resource_format_loader(loader, true);
	Ref<ResourceLoader::LoadToken> grandparent = ResourceLoader::_load_start("user://grand.loader_lifetime", "Resource",
			ResourceLoader::LOAD_THREAD_SPAWN_SINGLE, ResourceLoader::CACHE_MODE_IGNORE);
	CHECK(grandparent.is_valid());
	uint64_t deadline = OS::get_singleton()->get_ticks_msec() + 10000;
	bool entered = false;
	while (!(entered = loader->entered.try_wait()) && OS::get_singleton()->get_ticks_msec() < deadline) {
		OS::get_singleton()->delay_usec(100);
	}
	CHECK_MESSAGE(entered, "Grandparent did not reach its gate before the deadline");
	MessageQueue::get_main_singleton()->flush();
	loader->release.post();
	auto *pool = WorkerThreadPool::get_singleton();
	while (entered && !pool->is_task_completed(loader->task_id) && OS::get_singleton()->get_ticks_msec() < deadline) {
		OS::get_singleton()->delay_usec(100);
	}
	CHECK(loader->emitted_while_loading);
	CHECK_MESSAGE(loader->changed, "Child changed connection was lost when its parent retired during the grandparent load");
	Error error;
	CHECK(ResourceLoader::_load_complete(*grandparent.ptr(), &error).is_valid());
	grandparent.unref();
	loader->child_resource.unref();
	loader->child_token.unref();
	MessageQueue::get_main_singleton()->flush();
	ResourceLoader::clear_thread_load_tasks();
	ResourceLoader::remove_resource_format_loader(loader);
}

TEST_CASE("[ResourceLoader] Child dependencies are handed to the parent once") {
	Ref<AwaitingLoader> loader;
	loader.instantiate();
	loader->release_child = true;
	ResourceLoader::add_resource_format_loader(loader, true);
	Ref<ResourceLoader::LoadToken> parent = ResourceLoader::_load_start("user://parent.loader_lifetime", "Resource",
			ResourceLoader::LOAD_THREAD_SPAWN_SINGLE, ResourceLoader::CACHE_MODE_IGNORE);
	CHECK(parent.is_valid());
	auto *pool = WorkerThreadPool::get_singleton();
	uint64_t deadline = OS::get_singleton()->get_ticks_msec() + 10000;
	while ((loader->parent_task_id == WorkerThreadPool::INVALID_TASK_ID || !pool->is_task_completed(loader->parent_task_id)) && OS::get_singleton()->get_ticks_msec() < deadline) {
		OS::get_singleton()->delay_usec(100);
	}
	CHECK(loader->child_resource.is_valid());
	ResourceLoader::clear_thread_load_tasks();
	if (loader->child_resource.is_valid()) {
		CHECK_MESSAGE(loader->child_resource->get_reference_count() == 2, "Child source is referenced by the parent more than once");
	}
	parent.unref();
	loader->child_resource.unref();
	MessageQueue::get_main_singleton()->flush();
	ResourceLoader::clear_thread_load_tasks();
	ResourceLoader::remove_resource_format_loader(loader);
}

TEST_CASE("[ResourceLoader] Shutdown drains an active native load") {
	Ref<GatedLoader> loader;
	loader.instantiate();
	bool for_user = false;
	SUBCASE("Abandoned native request") {}
	SUBCASE("Unclaimed user request") {
		for_user = true;
	}
	ResourceLoader::add_resource_format_loader(loader, true);
	Ref<ResourceLoader::LoadToken> token = ResourceLoader::_load_start("user://shutdown.loader_lifetime", "Resource",
			ResourceLoader::LOAD_THREAD_SPAWN_SINGLE, ResourceLoader::CACHE_MODE_IGNORE, for_user);
	CHECK(token.is_valid());
	if (token.is_null()) {
		ResourceLoader::remove_resource_format_loader(loader);
		return;
	}
	uint64_t deadline = OS::get_singleton()->get_ticks_msec() + 10000;
	bool entered = false;
	while (!(entered = loader->entered.try_wait()) && OS::get_singleton()->get_ticks_msec() < deadline) {
		OS::get_singleton()->delay_usec(100);
	}
	CHECK_MESSAGE(entered, "Shutdown worker did not enter before the deadline");
	ObjectID id = token->get_instance_id();
	if (!entered) {
		loader->release.post();
		Error error;
		ResourceLoader::_load_complete(*token.ptr(), &error);
	}
	token.unref();
	Error queued = MessageQueue::get_main_singleton()->push_callable(callable_mp(loader.ptr(), &GatedLoader::release_worker));
	CHECK(queued == OK);
	if (queued != OK) {
		loader->release.post();
	}
	ResourceLoader::clear_thread_load_tasks();
	CHECK(ObjectDB::get_instance(id) == nullptr);
	ERR_PRINT_OFF;
	CHECK_FALSE(WorkerThreadPool::get_singleton()->is_task_completed(loader->task_id));
	ERR_PRINT_ON;
	ResourceLoader::remove_resource_format_loader(loader);
}

TEST_CASE("[ResourceLoader] Queue statistics preserve callbacks and arguments") {
	CallQueue queue(nullptr, 1);
	Ref<NestedLoader> loader;
	loader.instantiate();
	Ref<Resource> resource;
	resource.instantiate();
	ObjectID id = resource->get_instance_id();
	CHECK(queue.push_callable(callable_mp(loader.ptr(), &NestedLoader::on_queue_callback), resource, String("retained")) == OK);
	resource.unref();
	queue.statistics();
	CHECK(ObjectDB::get_instance(id) != nullptr);
	queue.statistics();
	CHECK(queue.has_messages());
	CHECK_FALSE(loader->changed);
	loader->statistics_queue = &queue;
	CHECK(queue.push_callable(callable_mp(loader.ptr(), &NestedLoader::on_queue_statistics)) == OK);
	Error queued;
	do {
		queued = queue.push_notification(loader->get_instance_id(), 0);
	} while (queued == OK);
	CHECK(queued == ERR_OUT_OF_MEMORY);
	CHECK(queue.push_notification(loader->get_instance_id(), 0) == ERR_OUT_OF_MEMORY);
	CHECK(ObjectDB::get_instance(id) != nullptr);
	CHECK(queue.flush() == OK);
	CHECK(loader->changed);
	CHECK(loader->statistics_returned);
	CHECK(loader->statistics_error == ERR_OUT_OF_MEMORY);
	CHECK(loader->queue_callbacks == 1);
	CHECK_FALSE(queue.has_messages());
	CHECK(ObjectDB::get_instance(id) == nullptr);
}

TEST_CASE("[ResourceLoader] Full deferred queue preserves native task ownership") {
	bool shutdown = false;
	SUBCASE("A later callback drains the completed task") {}
	SUBCASE("Shutdown drains without the rejected callback") {
		shutdown = true;
	}
	auto *queue = MessageQueue::get_main_singleton();
	queue->flush();
	Ref<GatedLoader> loader;
	loader.instantiate();
	ResourceLoader::add_resource_format_loader(loader, true);
	Ref<ResourceLoader::LoadToken> token = ResourceLoader::_load_start("user://queue_full.loader_lifetime", "Resource",
			ResourceLoader::LOAD_THREAD_SPAWN_SINGLE, ResourceLoader::CACHE_MODE_IGNORE);
	CHECK(token.is_valid());
	if (token.is_null()) {
		ResourceLoader::remove_resource_format_loader(loader);
		return;
	}
	uint64_t deadline = OS::get_singleton()->get_ticks_msec() + 10000;
	bool entered = false;
	while (!(entered = loader->entered.try_wait()) && OS::get_singleton()->get_ticks_msec() < deadline) {
		OS::get_singleton()->delay_usec(100);
	}
	CHECK(entered);
	if (!entered) {
		loader->release.post();
		Error error;
		ResourceLoader::_load_complete(*token.ptr(), &error);
		token.unref();
		ResourceLoader::clear_thread_load_tasks();
		ResourceLoader::remove_resource_format_loader(loader);
		return;
	}
	Error queued;
	do {
		queued = queue->push_notification(loader->get_instance_id(), 0);
	} while (queued == OK);
	CHECK(queued == ERR_OUT_OF_MEMORY);
	ObjectID id = token->get_instance_id();
	WorkerThreadPool::TaskID rejected_task_id = loader->task_id;
	token.unref();
	loader->release.post();
	auto *pool = WorkerThreadPool::get_singleton();
	wait_completed(pool, loader->task_id);
	CHECK(ObjectDB::get_instance(id) != nullptr);
	queue->clear();
	if (shutdown) {
		ResourceLoader::clear_thread_load_tasks();
	} else {
		loader->release.post();
		Ref<ResourceLoader::LoadToken> recovery = ResourceLoader::_load_start("user://queue_callback.loader_lifetime", "Resource",
				ResourceLoader::LOAD_THREAD_SPAWN_SINGLE, ResourceLoader::CACHE_MODE_IGNORE);
		CHECK(recovery.is_valid());
		if (recovery.is_valid()) {
			CHECK_MESSAGE(wait_entered(loader->entered), "Recovery load did not enter before the deadline");
			wait_completed(pool, loader->task_id);
			recovery.unref();
			queue->flush();
		}
	}
	CHECK(ObjectDB::get_instance(id) == nullptr);
	ERR_PRINT_OFF;
	CHECK_FALSE(pool->is_task_completed(rejected_task_id));
	ERR_PRINT_ON;
	ResourceLoader::clear_thread_load_tasks();
	ResourceLoader::remove_resource_format_loader(loader);
}

} // namespace TestResourceLoaderLifetime
