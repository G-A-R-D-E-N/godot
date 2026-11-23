/**************************************************************************/
/*  test_device_memory_budget.cpp                                         */
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

TEST_FORCE_LINK(test_device_memory_budget)

#include "core/templates/local_vector.h"
#include "servers/rendering/device_memory_budget.h"

namespace TestDeviceMemoryBudget {

struct Heap {
	bool device_local = false;
	uint64_t usage = 0;
	uint64_t budget = 0;
};

static DeviceMemoryBudget sum(const LocalVector<Heap> &p_heaps) {
	return device_memory_budget_from_heaps(p_heaps.size(), [&](uint32_t p_heap, bool &r_device_local, uint64_t &r_usage, uint64_t &r_budget) {
		r_device_local = p_heaps[p_heap].device_local;
		r_usage = p_heaps[p_heap].usage;
		r_budget = p_heaps[p_heap].budget;
	});
}

TEST_CASE("[DeviceMemoryBudget] One device-local heap is reported as it is") {
	LocalVector<Heap> heaps;
	heaps.push_back({ true, 20ull << 30, 22ull << 30 });
	const DeviceMemoryBudget b = sum(heaps);
	CHECK(b.usage == 20ull << 30);
	CHECK(b.budget == 22ull << 30);
}

TEST_CASE("[DeviceMemoryBudget] Host heaps are left out, device-local usage is added and the largest device-local budget is kept") {
	LocalVector<Heap> heaps;
	heaps.push_back({ true, 12ull << 30, 30ull << 30 });
	heaps.push_back({ false, 3ull << 30, 40ull << 30 });
	heaps.push_back({ true, 100ull << 20, 214ull << 20 });
	const DeviceMemoryBudget b = sum(heaps);
	CHECK(b.usage == (12ull << 30) + (100ull << 20));
	CHECK(b.budget == 30ull << 30);
}

TEST_CASE("[DeviceMemoryBudget] A device-local heap that reports the whole card's budget again does not double it") {
	LocalVector<Heap> heaps;
	heaps.push_back({ true, 18ull << 30, 22ull << 30 });
	heaps.push_back({ true, 200ull << 20, 22ull << 30 });
	const DeviceMemoryBudget b = sum(heaps);
	CHECK(b.usage == (18ull << 30) + (200ull << 20));
	CHECK(b.budget == 22ull << 30);
}

TEST_CASE("[DeviceMemoryBudget] An unknown device-local budget makes the whole answer unknown") {
	LocalVector<Heap> heaps;
	heaps.push_back({ true, 12ull << 30, 30ull << 30 });
	heaps.push_back({ true, 1ull << 30, 0 });
	DeviceMemoryBudget b = sum(heaps);
	CHECK(b.usage == 0);
	CHECK(b.budget == 0);

	heaps.clear();
	heaps.push_back({ false, 1ull << 30, 0 });
	heaps.push_back({ true, 2ull << 30, 8ull << 30 });
	b = sum(heaps);
	CHECK(b.usage == 2ull << 30);
	CHECK(b.budget == 8ull << 30);
}

TEST_CASE("[DeviceMemoryBudget] No device-local heap gives zero") {
	LocalVector<Heap> heaps;
	DeviceMemoryBudget b = sum(heaps);
	CHECK(b.usage == 0);
	CHECK(b.budget == 0);
	heaps.push_back({ false, 1ull << 30, 16ull << 30 });
	b = sum(heaps);
	CHECK(b.usage == 0);
	CHECK(b.budget == 0);
}

} // namespace TestDeviceMemoryBudget
