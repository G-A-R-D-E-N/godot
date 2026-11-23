/**************************************************************************/
/*  test_shader_rd_group_failure.cpp                                      */
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

TEST_FORCE_LINK(test_shader_rd_group_failure)

#ifdef RD_ENABLED

#include "servers/rendering/renderer_rd/shader_rd.h"

namespace TestShaderRDGroupFailure {

typedef ShaderRD::GroupVariantState VariantState;
typedef ShaderRD::GroupEndState EndState;
typedef ShaderRD::GroupEndPlan EndPlan;

static EndState make_state(bool p_version_valid, bool p_group_lazy, bool p_cache_user_dir_valid, bool p_loaded_from_cache) {
	EndState state;
	state.version_valid = p_version_valid;
	state.group_lazy = p_group_lazy;
	state.cache_user_dir_valid = p_cache_user_dir_valid;
	state.loaded_from_cache = p_loaded_from_cache;
	return state;
}

static EndState state_after(const EndPlan &p_plan, bool p_group_lazy) {
	EndState state = make_state(p_plan.version_valid, p_group_lazy, true, false);
	state.creation_group_failed = p_plan.creation_group_failed;
	return state;
}

static void check_free(const EndPlan &p_plan, const LocalVector<uint32_t> &p_expected) {
	CHECK(p_plan.free_indices.size() == p_expected.size());
	if (p_plan.free_indices.size() != p_expected.size()) {
		return;
	}
	for (uint32_t i = 0; i < p_expected.size(); i++) {
		CHECK(p_plan.free_indices[i] == p_expected[i]);
	}
}

TEST_CASE("[ShaderRD] A group whose variants all compiled is kept and cached") {
	const LocalVector<VariantState> variants = { { true, true, 64 }, { true, true, 32 } };
	const EndPlan plan = ShaderRD::plan_group_end(variants, make_state(false, false, true, false));
	CHECK(plan.group_ok);
	CHECK(plan.version_valid);
	CHECK_FALSE(plan.creation_group_failed);
	CHECK(plan.save_to_cache);
	check_free(plan, {});
}

TEST_CASE("[ShaderRD] A group loaded from the cache is not written back") {
	const LocalVector<VariantState> variants = { { true, true, 64 }, { true, true, 32 } };
	const EndPlan plan = ShaderRD::plan_group_end(variants, make_state(false, false, true, true));
	CHECK(plan.group_ok);
	CHECK(plan.version_valid);
	CHECK_FALSE(plan.save_to_cache);
	check_free(plan, {});
}

TEST_CASE("[ShaderRD] A compiled group is not saved when the user cache folder is unusable") {
	const LocalVector<VariantState> variants = { { true, true, 64 } };
	const EndPlan plan = ShaderRD::plan_group_end(variants, make_state(false, false, false, false));
	CHECK(plan.group_ok);
	CHECK(plan.version_valid);
	CHECK_FALSE(plan.save_to_cache);
}

TEST_CASE("[ShaderRD] A lazy group that compiled on a valid version keeps the version valid") {
	const LocalVector<VariantState> variants = { { true, true, 64 } };
	const EndPlan plan = ShaderRD::plan_group_end(variants, make_state(true, true, true, false));
	CHECK(plan.group_ok);
	CHECK(plan.version_valid);
	CHECK_FALSE(plan.creation_group_failed);
	CHECK(plan.save_to_cache);
	check_free(plan, {});
}

TEST_CASE("[ShaderRD] A rejected lazy variant fails only its group and the version stays valid") {
	const LocalVector<VariantState> variants = { { true, true, 64 }, { true, true, 0 }, { true, true, 48 } };
	const EndPlan plan = ShaderRD::plan_group_end(variants, make_state(true, true, true, false));
	CHECK_FALSE(plan.group_ok);
	CHECK(plan.version_valid);
	CHECK_FALSE(plan.creation_group_failed);
	CHECK_FALSE(plan.save_to_cache);
	check_free(plan, { 0, 1, 2 });
}

TEST_CASE("[ShaderRD] A lazy group whose cache entry does not load fails alone and is not saved") {
	const LocalVector<VariantState> variants = { { true, true, 64 }, { true, true, 0 } };
	const EndPlan plan = ShaderRD::plan_group_end(variants, make_state(true, true, true, true));
	CHECK_FALSE(plan.group_ok);
	CHECK(plan.version_valid);
	CHECK_FALSE(plan.creation_group_failed);
	CHECK_FALSE(plan.save_to_cache);
	check_free(plan, { 0, 1 });
}

TEST_CASE("[ShaderRD] A lazy placeholder with no bytecode is not written to the cache") {
	const LocalVector<VariantState> variants = { { true, true, 0 } };
	const EndPlan plan = ShaderRD::plan_group_end(variants, make_state(true, true, true, false));
	CHECK_FALSE(plan.group_ok);
	CHECK(plan.version_valid);
	CHECK_FALSE(plan.save_to_cache);
	check_free(plan, { 0 });
}

TEST_CASE("[ShaderRD] A null shader with bytecode still fails the group") {
	const LocalVector<VariantState> variants = { { true, false, 16 } };
	const EndPlan plan = ShaderRD::plan_group_end(variants, make_state(true, true, true, false));
	CHECK_FALSE(plan.group_ok);
	CHECK(plan.version_valid);
	CHECK_FALSE(plan.save_to_cache);
	check_free(plan, {});
}

TEST_CASE("[ShaderRD] A null variant in a group compiled at creation is not freed and leaves the version invalid") {
	const LocalVector<VariantState> variants = { { true, true, 64 }, { true, false, 0 } };
	const EndPlan plan = ShaderRD::plan_group_end(variants, make_state(false, false, true, false));
	CHECK_FALSE(plan.group_ok);
	CHECK_FALSE(plan.version_valid);
	CHECK(plan.creation_group_failed);
	CHECK_FALSE(plan.save_to_cache);
	check_free(plan, { 0 });
}

TEST_CASE("[ShaderRD] A disabled variant with no bytecode does not fail the group") {
	const LocalVector<VariantState> variants = { { false, true, 0 }, { true, true, 64 } };
	const EndPlan plan = ShaderRD::plan_group_end(variants, make_state(true, true, true, false));
	CHECK(plan.group_ok);
	CHECK(plan.version_valid);
	CHECK(plan.save_to_cache);
	check_free(plan, {});
}

TEST_CASE("[ShaderRD] A disabled variant with a null shader does not fail the group") {
	const LocalVector<VariantState> variants = { { true, true, 64 }, { false, false, 0 } };
	const EndPlan plan = ShaderRD::plan_group_end(variants, make_state(false, false, true, false));
	CHECK(plan.group_ok);
	CHECK(plan.version_valid);
	CHECK(plan.save_to_cache);
	check_free(plan, {});
}

TEST_CASE("[ShaderRD] A failed group frees the placeholder of its disabled variant too") {
	const LocalVector<VariantState> variants = { { false, true, 0 }, { true, true, 0 } };
	const EndPlan plan = ShaderRD::plan_group_end(variants, make_state(true, true, true, false));
	CHECK_FALSE(plan.group_ok);
	CHECK(plan.version_valid);
	CHECK_FALSE(plan.save_to_cache);
	check_free(plan, { 0, 1 });
}

TEST_CASE("[ShaderRD] Freed slots come in ascending order and include a trailing disabled placeholder") {
	const LocalVector<VariantState> variants = { { true, true, 64 }, { true, true, 0 }, { false, true, 0 } };
	const EndPlan plan = ShaderRD::plan_group_end(variants, make_state(true, true, true, false));
	CHECK_FALSE(plan.group_ok);
	CHECK(plan.version_valid);
	check_free(plan, { 0, 1, 2 });
}

TEST_CASE("[ShaderRD] A group with every variant disabled counts as compiled") {
	const LocalVector<VariantState> variants = { { false, false, 0 }, { false, true, 0 } };
	const EndPlan plan = ShaderRD::plan_group_end(variants, make_state(false, false, true, false));
	CHECK(plan.group_ok);
	CHECK(plan.version_valid);
	CHECK(plan.save_to_cache);
	check_free(plan, {});
}

TEST_CASE("[ShaderRD] An empty group counts as compiled") {
	const LocalVector<VariantState> variants = {};
	const EndPlan plan = ShaderRD::plan_group_end(variants, make_state(false, false, false, false));
	CHECK(plan.group_ok);
	CHECK(plan.version_valid);
	CHECK_FALSE(plan.save_to_cache);
	check_free(plan, {});
}

TEST_CASE("[ShaderRD] A failed first group leaves the version invalid") {
	const LocalVector<VariantState> variants = { { true, false, 0 }, { true, false, 0 } };
	const EndPlan plan = ShaderRD::plan_group_end(variants, make_state(false, false, true, false));
	CHECK_FALSE(plan.group_ok);
	CHECK_FALSE(plan.version_valid);
	CHECK(plan.creation_group_failed);
	CHECK_FALSE(plan.save_to_cache);
	check_free(plan, {});
}

TEST_CASE("[ShaderRD] A lazy group that compiles after a creation group failed keeps the version invalid") {
	const LocalVector<VariantState> base = { { true, false, 0 } };
	const EndPlan base_plan = ShaderRD::plan_group_end(base, make_state(false, false, true, false));
	REQUIRE_FALSE(base_plan.version_valid);

	const LocalVector<VariantState> lazy = { { true, true, 64 } };
	const EndPlan lazy_plan = ShaderRD::plan_group_end(lazy, state_after(base_plan, true));
	CHECK(lazy_plan.group_ok);
	CHECK_FALSE(lazy_plan.version_valid);
	CHECK(lazy_plan.creation_group_failed);
	check_free(lazy_plan, {});
}

TEST_CASE("[ShaderRD] A creation group that fails after a lazy group compiled makes the version invalid") {
	const LocalVector<VariantState> lazy = { { true, true, 64 } };
	const EndPlan lazy_plan = ShaderRD::plan_group_end(lazy, make_state(false, true, true, false));
	REQUIRE(lazy_plan.version_valid);

	const LocalVector<VariantState> base = { { true, false, 0 } };
	const EndPlan base_plan = ShaderRD::plan_group_end(base, state_after(lazy_plan, false));
	CHECK_FALSE(base_plan.group_ok);
	CHECK_FALSE(base_plan.version_valid);
	CHECK(base_plan.creation_group_failed);
	check_free(base_plan, {});
}

TEST_CASE("[ShaderRD] A lazy group failure does not stop a later group from keeping the version valid") {
	const LocalVector<VariantState> failed = { { true, true, 0 } };
	const EndPlan failed_plan = ShaderRD::plan_group_end(failed, make_state(true, true, true, false));
	REQUIRE(failed_plan.version_valid);
	REQUIRE_FALSE(failed_plan.creation_group_failed);

	const LocalVector<VariantState> other = { { true, true, 64 } };
	const EndPlan other_plan = ShaderRD::plan_group_end(other, state_after(failed_plan, true));
	CHECK(other_plan.group_ok);
	CHECK(other_plan.version_valid);
	CHECK_FALSE(other_plan.creation_group_failed);
	CHECK(other_plan.save_to_cache);
}

} // namespace TestShaderRDGroupFailure

#endif
