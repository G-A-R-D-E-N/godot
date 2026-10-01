/**************************************************************************/
/*  test_scenes.cpp                                                       */
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

TEST_FORCE_LINK(test_scenes)

#include "core/io/file_access.h"
#include "core/io/resource.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/object/message_queue.h"
#include "core/object/object.h"
#include "core/object/property_info.h"
#include "core/os/memory.h"
#include "core/os/os.h"
#include "core/os/semaphore.h"
#include "core/string/string_name.h"
#include "core/variant/variant.h"
#include "scene/main/node.h"
#include "scene/resources/packed_scene.h"
#include "tests/test_utils.h"

namespace TestScenes {

class _TestNodeForTestingNestedScenes : public Node {
	GDCLASS(_TestNodeForTestingNestedScenes, Node);

	Ref<PackedScene> _inner_scene;

public:
	void set_inner_scene(const Ref<PackedScene> p_scene) {
		_inner_scene = p_scene;
	}

	Ref<PackedScene> get_inner_scene() const {
		return _inner_scene;
	}

	static void _bind_methods() {
		ClassDB::bind_method(D_METHOD("set_inner_scene", "p_scene"), &_TestNodeForTestingNestedScenes::set_inner_scene);
		ClassDB::bind_method(D_METHOD("get_inner_scene"), &_TestNodeForTestingNestedScenes::get_inner_scene);

		ADD_PROPERTY(
				PropertyInfo(Variant::OBJECT, "inner_scene", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE),
				"set_inner_scene", "get_inner_scene");
	}
};

void prepare_scene(Ref<PackedScene> scene) {
	Ref<Resource> child_resource = memnew(Resource);
	child_resource->set_name("I'm a child resource");
	scene->set_meta("other_resource", child_resource);

	scene->set_name("Hello world");
	scene->set_meta("ExampleMetadata", Vector2i(40, 80));
	scene->set_meta("string", "The\nstring\nwith\nunnecessary\nline\n\t\\\nbreaks");
}

void validate_scene(Ref<PackedScene> loaded_scene) {
	CHECK_MESSAGE(
			loaded_scene->get_name() == "Hello world",
			"The loaded resource name should be equal to the expected value.");
	CHECK_MESSAGE(
			loaded_scene->get_meta("ExampleMetadata") == Vector2i(40, 80),
			"The loaded resource metadata should be equal to the expected value.");
	CHECK_MESSAGE(
			loaded_scene->get_meta("string") == "The\nstring\nwith\nunnecessary\nline\n\t\\\nbreaks",
			"The loaded resource metadata should be equal to the expected value.");

	const Ref<Resource> &meta_resource = loaded_scene->get_meta("other_resource");
	CHECK_MESSAGE(
			meta_resource->get_name() == "I'm a child resource",
			"The loaded child resource name should be equal to the expected value.");
}

void validate_and_instantiate_scene(Ref<PackedScene> loaded_scene) {
	bool is_null = loaded_scene.is_null();
	CHECK_MESSAGE(!is_null, "Could not open scene file");

	if (is_null) {
		return;
	}

	validate_scene(loaded_scene);

	Node *new_node = static_cast<Node *>(loaded_scene->instantiate());
	CHECK_MESSAGE(
			new_node->get_name() == "named_node",
			"The instantiated node has invalid name.");

	const Ref<PackedScene> &inner_loaded_scene = loaded_scene->get_meta("inner_scene");
	validate_scene(inner_loaded_scene);

	Node *inner_new_node = static_cast<Node *>(inner_loaded_scene->instantiate());
	CHECK_MESSAGE(
			inner_new_node->get_name() == "inner_named_node",
			"The instantiated node has invalid name.");

	memdelete(new_node);
	memdelete(inner_new_node);
}

TEST_CASE("[Scenes] Scene Saving and loading") {
	Node *inner_node = memnew(Node);
	inner_node->set_name("inner_named_node");

	Ref<PackedScene> inner_scene = memnew(PackedScene);
	inner_scene->pack(inner_node);
	prepare_scene(inner_scene);

	Node *node = memnew(Node);
	node->set_name("named_node");

	Ref<PackedScene> scene = memnew(PackedScene);
	scene->pack(node);
	prepare_scene(scene);
	scene->set_meta("inner_scene", inner_scene);

	Ref<Resource> child_resource = memnew(Resource);
	child_resource->set_name("I'm a child resource");
	scene->set_meta("other_resource", child_resource);

	const String save_path_binary = TestUtils::get_temp_path("scene.res");
	const String save_path_text = TestUtils::get_temp_path("scene.tscn");

	ResourceSaver::save(scene, save_path_binary);
	ResourceSaver::save(scene, save_path_text);

	const Ref<PackedScene> &loaded_resource_binary = ResourceLoader::load(save_path_binary);
	validate_and_instantiate_scene(loaded_resource_binary);

	const Ref<PackedScene> &loaded_resource_text = ResourceLoader::load(save_path_text);
	validate_and_instantiate_scene(loaded_resource_text);

	memdelete(node);
	memdelete(inner_node);
}

void validate_nested_scene(Ref<PackedScene> loaded_scene) {
	bool is_null = loaded_scene.is_null();
	CHECK_MESSAGE(!is_null, "Could not open scene file");

	if (is_null) {
		return;
	}

	_TestNodeForTestingNestedScenes *node_for_testing = static_cast<_TestNodeForTestingNestedScenes *>(loaded_scene->instantiate());
	Ref<PackedScene> inner_scene = node_for_testing->get_inner_scene();
	Node *inner_node = inner_scene->instantiate();

	CHECK_MESSAGE(
			inner_node->get_name() == "named_node",
			"The instantiated node has invalid name.");

	memdelete(node_for_testing);
	memdelete(inner_node);
}

// This is to test a specific bug that can happen.
TEST_CASE("[Scenes] Scene Saving and loading as property") {
	ClassDB::register_class<_TestNodeForTestingNestedScenes>();

	Node *inner_node = memnew(Node);
	const Ref<PackedScene> inner_scene = memnew(PackedScene);
	inner_node->set_name("named_node");
	inner_scene->pack(inner_node);

	_TestNodeForTestingNestedScenes *nested_scene_node = memnew(_TestNodeForTestingNestedScenes);
	nested_scene_node->set_inner_scene(inner_scene);

	Ref<PackedScene> scene = memnew(PackedScene);
	scene->pack(nested_scene_node);

	const String save_path_binary = TestUtils::get_temp_path("nested_scene_test.res");
	const String save_path_text = TestUtils::get_temp_path("nested_scene_test.tscn");

	ResourceSaver::save(scene, save_path_binary);
	ResourceSaver::save(scene, save_path_text);

	memdelete(inner_node);
	memdelete(nested_scene_node);

	const Ref<PackedScene> &loaded_resource_binary = ResourceLoader::load(save_path_binary);
	validate_nested_scene(loaded_resource_binary);

	const Ref<PackedScene> &loaded_resource_text = ResourceLoader::load(save_path_text);
	validate_nested_scene(loaded_resource_text);
}

void validate_simple_scene(Ref<PackedScene> loaded_scene) {
	bool is_null = loaded_scene.is_null();
	CHECK(!is_null);

	if (is_null) {
		return;
	}

	Node *instanced_node = static_cast<Node *>(loaded_scene->instantiate());
	CHECK_MESSAGE(
			instanced_node->get_name() == "SimpleNode",
			"The instantiated node has invalid name..");

	memdelete(instanced_node);
}

TEST_CASE("[Scenes] Simple scene saving and loading") {
	Node *node = memnew(Node);
	node->set_name("SimpleNode");

	Ref<PackedScene> scene = memnew(PackedScene);
	scene->pack(node);

	const String save_path_binary = TestUtils::get_temp_path("simple_scene_test.res");
	const String save_path_text = TestUtils::get_temp_path("simple_scene_test.tscn");

	ResourceSaver::save(scene, save_path_binary);
	ResourceSaver::save(scene, save_path_text);

	memdelete(node);

	const Ref<PackedScene> &loaded_resource_binary = ResourceLoader::load(save_path_binary);
	validate_simple_scene(loaded_resource_binary);

	const Ref<PackedScene> &loaded_resource_text = ResourceLoader::load(save_path_text);
	validate_simple_scene(loaded_resource_text);

	// Simple scenes should not have a [resource] tag, so let's validate it.
	Ref<FileAccess> f = FileAccess::open(save_path_text, FileAccess::READ);
	bool is_null = f.is_null();
	CHECK(!is_null);

	if (is_null) {
		return;
	}

	while (!f->eof_reached()) {
		String line;
		line = f->get_line();
		CHECK_MESSAGE(
				line != "[resource]",
				"The scene file cannot have the [resource] tag");
	}
	f->close();
}
} // namespace TestScenes

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

	void on_resource_changed() {
		changed = true;
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
	while (!pool->is_task_completed(loader->task_id)) {
		OS::get_singleton()->delay_usec(100);
	}
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
		while (!loader->entered.try_wait()) {
			OS::get_singleton()->delay_usec(100);
		}
	}
	auto *pool = WorkerThreadPool::get_singleton();
	while (!pool->is_task_completed(loader->parent_task_id)) {
		OS::get_singleton()->delay_usec(100);
	}
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
		while (!pool->is_task_completed(loader->task_id)) {
			OS::get_singleton()->delay_usec(100);
		}
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

} // namespace TestResourceLoaderLifetime
