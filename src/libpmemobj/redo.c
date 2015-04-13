/*
 * Copyright (c) 2015, Intel Corporation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * redo.c -- redo log implementation
 */
#include <stdlib.h>
#include <stdint.h>

#include "libpmem.h"
#include "libpmemobj.h"
#include "util.h"
#include "obj.h"
#include "redo.h"
#include "out.h"

#define	_POBJ_REDO_MIN_OFFSET	8192

/*
 * redo_log_check_offset -- (internal) check if offset is valid
 */
static inline int
redo_log_check_offset(PMEMobjpool *pop, uint64_t offset)
{
	/* XXX change the minimum offset */
	return offset >= _POBJ_REDO_MIN_OFFSET &&
		offset <= (pop->size - sizeof (uint64_t));
}

/*
 * redo_log_nflags -- (internal) get number of finish flags set
 */
static size_t
redo_log_nflags(struct redo_log *redo, size_t nentries)
{
	size_t ret = 0;
	size_t i;

	for (i = 0; i < nentries; i++) {
		if (redo[i].offset & REDO_FINISH_FLAG)
			ret++;
	}

	LOG(15, "redo %p nentries %zu nflags %zu", redo, nentries, ret);

	return ret;
}

/*
 * redo_log_store -- (internal) store redo log entry at specified index
 */
void
redo_log_store(PMEMobjpool *pop, struct redo_log *redo, size_t index,
		uint64_t offset, uint64_t value)
{
	LOG(15, "redo %p index %zu offset %ju value %ju",
			redo, index, offset, value);

	ASSERTeq(offset & REDO_FINISH_FLAG, 0);

	redo[index].offset = offset;
	redo[index].value = value;
}

/*
 * redo_log_store_last -- (internal) store last entry at specified index
 */
void
redo_log_store_last(PMEMobjpool *pop, struct redo_log *redo, size_t index,
		uint64_t offset, uint64_t value)
{
	LOG(15, "redo %p index %zu offset %ju value %ju",
			redo, index, offset, value);

	ASSERTeq(offset & REDO_FINISH_FLAG, 0);

	/* store value of last entry */
	redo[index].value = value;

	/* persist all redo log entries */
	pop->persist(redo, index * sizeof (struct redo_log));

	/* store and persist offset of last entry */
	redo[index].offset = offset | REDO_FINISH_FLAG;
	pop->persist(&redo[index].offset, sizeof (redo[index].offset));
}

/*
 * redo_log_process -- (internal) process redo log entries
 */
void
redo_log_process(PMEMobjpool *pop, struct redo_log *redo,
		size_t nentries)
{
	LOG(15, "redo %p nentries %zu", redo, nentries);

	ASSERTeq(redo_log_check(pop, redo, nentries), 1);

	uint64_t *val;
	while ((redo->offset & REDO_FINISH_FLAG) == 0) {
		val = (uint64_t *)((uintptr_t)pop->addr + redo->offset);
		*val = redo->value;

		pop->flush(val, sizeof (uint64_t));

		redo++;
	}

	uint64_t offset = redo->offset & REDO_FLAG_MASK;
	val = (uint64_t *)((uintptr_t)pop->addr + offset);
	*val = redo->value;

	pop->flush(val, sizeof (uint64_t));
	pop->drain();

	redo->offset = 0;

	pop->persist(&redo->offset, sizeof (redo->offset));
}

/*
 * redo_log_recover -- (internal) recovery of redo log
 *
 * The redo_log_recover shall be preceded by redo_log_check call.
 */
void
redo_log_recover(PMEMobjpool *pop, struct redo_log *redo,
		size_t nentries)
{
	LOG(15, "redo %p nentries %zu", redo, nentries);

	size_t nflags = redo_log_nflags(redo, nentries);
	ASSERT(nflags < 2);

	if (nflags == 1)
		redo_log_process(pop, redo, nentries);
}

/*
 * redo_log_check -- (internal) check consistency of redo log entries
 */
int
redo_log_check(PMEMobjpool *pop, struct redo_log *redo, size_t nentries)
{
	LOG(15, "redo %p nentries %zu", redo, nentries);

	size_t nflags = redo_log_nflags(redo, nentries);

	if (nflags > 1) {
		LOG(15, "redo %p to many finish flags", redo);
		return 0;
	}

	if (nflags == 1) {
		while ((redo->offset & REDO_FINISH_FLAG) == 0) {
			if (!redo_log_check_offset(pop, redo->offset)) {
				LOG(15, "redo %p invalid offset %ju",
						redo, redo->offset);
				return 0;
			}
			redo++;
		}

		uint64_t offset = redo->offset & REDO_FLAG_MASK;
		if (!redo_log_check_offset(pop, offset)) {
			LOG(15, "redo %p invalid offset %ju", redo, offset);
			return 0;
		}
	}

	return 1;
}