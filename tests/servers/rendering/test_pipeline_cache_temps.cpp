/**************************************************************************/
/*  test_pipeline_cache_temps.cpp                                         */
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

TEST_FORCE_LINK(test_pipeline_cache_temps)

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "servers/rendering/pipeline_cache_temps.h"
#include "tests/test_utils.h"

namespace TestPipelineCacheTemps {

static void make_file(const String &p_path) {
	Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::WRITE);
	REQUIRE(f.is_valid());
	f->store_32(1);
	f->close();
}

static void check_stale_temps(const String &p_base) {
	REQUIRE(DirAccess::make_dir_recursive_absolute(p_base) == OK);
	const String cache = p_base.path_join("pipelines.test.cache");
	const String old_temp = cache + ".tmp";
	const String process_temp = cache + ".1234.tmp";
	const String other_cache_temp = p_base.path_join("pipelines.other.cache.1234.tmp");
	const String other_file = cache + ".1234.bak";
	const String files[] = { cache, old_temp, process_temp, other_cache_temp, other_file };
	for (const String &path : files) {
		make_file(path);
	}
	const uint64_t modified = FileAccess::get_modified_time(process_temp);
	REQUIRE(modified > 0);

	remove_stale_pipeline_cache_temps(cache, modified + 1800);
	for (const String &path : files) {
		CHECK(FileAccess::exists(path));
	}

	remove_stale_pipeline_cache_temps(cache, modified + 7200);
	CHECK_FALSE(FileAccess::exists(old_temp));
	CHECK_FALSE(FileAccess::exists(process_temp));
	CHECK(FileAccess::exists(cache));
	CHECK(FileAccess::exists(other_cache_temp));
	CHECK(FileAccess::exists(other_file));

	for (const String &path : files) {
		if (FileAccess::exists(path)) {
			DirAccess::remove_absolute(path);
		}
	}
	DirAccess::remove_absolute(p_base);
}

TEST_CASE("[PipelineCache] Stale temp files go and fresh ones stay") {
	SUBCASE("Absolute path") {
		check_stale_temps(TestUtils::get_temp_path("pipeline_cache_temps"));
	}
	SUBCASE("User path, where the cache lives") {
		check_stale_temps("user://pipeline_cache_temps_test");
	}
}

} // namespace TestPipelineCacheTemps
