/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/exec/multi_iterator.h"

#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/storage/record_fetcher.h"
#include "mongo/stdx/memory.h"

namespace mongo {

using std::unique_ptr;
using std::vector;
using stdx::make_unique;

const char* MultiIteratorStage::kStageType = "MULTI_ITERATOR";

MultiIteratorStage::MultiIteratorStage(OperationContext* txn,
                                       WorkingSet* ws,
                                       Collection* collection)
    : _txn(txn), _collection(collection), _ws(ws), _wsidForFetch(_ws->allocate()) {
    // We pre-allocate a WSM and use it to pass up fetch requests. This should never be used
    // for anything other than passing up NEED_YIELD. We use the loc and owned obj state, but
    // the loc isn't really pointing at any obj. The obj field of the WSM should never be used.
    WorkingSetMember* member = _ws->get(_wsidForFetch);
    member->state = WorkingSetMember::LOC_AND_OWNED_OBJ;
}

void MultiIteratorStage::addIterator(unique_ptr<RecordCursor> it) {
    _iterators.push_back(std::move(it));
}

PlanStage::StageState MultiIteratorStage::work(WorkingSetID* out) {
    if (_collection == NULL) {
        Status status(ErrorCodes::InternalError, "MultiIteratorStage died on null collection");
        *out = WorkingSetCommon::allocateStatusMember(_ws, status);
        return PlanStage::DEAD;
    }

    boost::optional<Record> record;
    try {
        while (!_iterators.empty()) {
            if (auto fetcher = _iterators.back()->fetcherForNext()) {
                // Pass the RecordFetcher off up.
                WorkingSetMember* member = _ws->get(_wsidForFetch);
                member->setFetcher(fetcher.release());
                *out = _wsidForFetch;
                return NEED_YIELD;
            }

            record = _iterators.back()->next();
            if (record)
                break;
            _iterators.pop_back();
        }
    } catch (const WriteConflictException& wce) {
        // If _advance throws a WCE we shouldn't have moved.
        invariant(!_iterators.empty());
        *out = WorkingSet::INVALID_ID;
        return NEED_YIELD;
    }

    if (!record)
        return IS_EOF;

    *out = _ws->allocate();
    WorkingSetMember* member = _ws->get(*out);
    member->loc = record->id;
    member->obj = {_txn->recoveryUnit()->getSnapshotId(), record->data.releaseToBson()};
    member->state = WorkingSetMember::LOC_AND_UNOWNED_OBJ;
    return PlanStage::ADVANCED;
}

bool MultiIteratorStage::isEOF() {
    return _collection == NULL || _iterators.empty();
}

void MultiIteratorStage::kill() {
    _collection = NULL;
    _iterators.clear();
}

void MultiIteratorStage::saveState() {
    _txn = NULL;
    for (size_t i = 0; i < _iterators.size(); i++) {
        _iterators[i]->savePositioned();
    }
}

void MultiIteratorStage::restoreState(OperationContext* opCtx) {
    invariant(_txn == NULL);
    _txn = opCtx;
    for (size_t i = 0; i < _iterators.size(); i++) {
        if (!_iterators[i]->restore(opCtx)) {
            kill();
        }
    }
}

void MultiIteratorStage::invalidate(OperationContext* txn,
                                    const RecordId& dl,
                                    InvalidationType type) {
    switch (type) {
        case INVALIDATION_DELETION:
            for (size_t i = 0; i < _iterators.size(); i++) {
                _iterators[i]->invalidate(dl);
            }
            break;
        case INVALIDATION_MUTATION:
            // no-op
            break;
    }
}

vector<PlanStage*> MultiIteratorStage::getChildren() const {
    vector<PlanStage*> empty;
    return empty;
}

unique_ptr<PlanStageStats> MultiIteratorStage::getStats() {
    unique_ptr<PlanStageStats> ret =
        make_unique<PlanStageStats>(CommonStats(kStageType), STAGE_MULTI_ITERATOR);
    ret->specific = make_unique<CollectionScanStats>();
    return ret;
}

}  // namespace mongo
